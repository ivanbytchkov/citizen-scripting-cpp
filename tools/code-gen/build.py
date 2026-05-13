#!/usr/bin/env python3
import json
import os
import sys
import urllib.request
from collections import defaultdict

CFX_URL = "https://runtime.fivem.net/doc/natives_cfx.json"
GAME_URLS = {
    "fivem": "https://runtime.fivem.net/doc/natives.json",
    "redm": "https://runtime.fivem.net/doc/natives_rdr3.json",
}

PARAM_TYPE_MAP = {
    "int": "int",
    "float": "float",
    "BOOL": "bool",
    "char*": "const char*",
    "Hash": "uint32_t",
    "Entity": "int",
    "Player": "int",
    "Vehicle": "int",
    "Ped": "int",
    "Object": "int",
    "Pickup": "int",
    "Blip": "int",
    "Cam": "int",
    "FireId": "int",
    "Interior": "int",
    "ScrHandle": "int",
    "Any": "int",
    "long": "int64_t",
    "int*": "int*",
    "float*": "float*",
    "BOOL*": "int*",
    "Hash*": "uint32_t*",
    "Entity*": "int*",
    "Player*": "int*",
    "Vehicle*": "int*",
    "Ped*": "int*",
    "Object*": "int*",
    "Pickup*": "int*",
    "Blip*": "int*",
    "Cam*": "int*",
    "FireId*": "int*",
    "Interior*": "int*",
    "ScrHandle*": "int*",
    "Any*": "int*",
    "long*": "int64_t*",
    "Vector3*": "Vector3*",
}

RETURN_TYPE_MAP = {
    "void": ("void", None),
    "int": ("int", "int"),
    "float": ("float", "float"),
    "BOOL": ("bool", "bool"),
    "char*": ("std::string", "std::string"),
    "Hash": ("uint32_t", "uint32_t"),
    "Entity": ("int", "int"),
    "Player": ("int", "int"),
    "Vehicle": ("int", "int"),
    "Ped": ("int", "int"),
    "Object": ("int", "int"),
    "Pickup": ("int", "int"),
    "Blip": ("int", "int"),
    "Cam": ("int", "int"),
    "FireId": ("int", "int"),
    "Interior": ("int", "int"),
    "ScrHandle": ("int", "int"),
    "Any": ("int", "int"),
    "long": ("int64_t", "int64_t"),
    "Vector3": ("Vector3", "Vector3"),
}


def screaming_to_pascal(name: str) -> str:
    return "".join(word.capitalize() for word in name.split("_"))


def to_safe_param(name: str) -> str:
    keywords = {"class", "struct", "enum", "union", "template", "operator",
                "new", "delete", "this", "return", "switch", "case",
                "default", "break", "continue", "goto", "if", "else",
                "for", "while", "do", "try", "catch", "throw", "register",
                "auto", "const", "static", "extern", "volatile", "inline",
                "virtual", "explicit", "friend", "namespace", "using",
                "typedef", "typename", "public", "private", "protected",
                "override", "final", "nullptr", "true", "false", "bool",
                "int", "float", "double", "char", "long", "short",
                "signed", "unsigned", "void", "object", "near", "far"}
    return name + "_" if name in keywords else name


def generate_wrapper(native: dict) -> str | None:
    name = native.get("name")
    if not name:
        return None

    hash_str = native.get("hash", "0x0")
    params = native.get("params", [])
    result_type = native.get("results", "void")

    if result_type == "object":
        return None

    if result_type not in RETURN_TYPE_MAP:
        return None

    cpp_ret, invoke_ret = RETURN_TYPE_MAP[result_type]
    func_name = screaming_to_pascal(name)

    cpp_params = []
    call_args = []
    skip = False
    for p in params:
        ptype = p.get("type", "int")
        pname = to_safe_param(p.get("name", "p"))

        if ptype == "Vector3*":
            skip = True
            break

        if ptype not in PARAM_TYPE_MAP:
            skip = True
            break

        cpp_type = PARAM_TYPE_MAP[ptype]
        cpp_params.append(f"{cpp_type} {pname}")
        call_args.append(pname)

    if skip:
        return None

    param_str = ", ".join(cpp_params)
    args_str = ", ".join(call_args)
    comma_args = f", {args_str}" if args_str else ""

    if invoke_ret is None:
        body = f"invoke({hash_str}{comma_args});"
    else:
        body = f"return invoke<{invoke_ret}>({hash_str}{comma_args});"

    return f"    inline {cpp_ret} {func_name}({param_str})\n    {{\n        {body}\n    }}"


def generate_namespace_block(namespace: str, natives: list[dict]) -> tuple[str, int] | None:
    wrappers = []
    for n in sorted(natives, key=lambda x: x.get("name", "")):
        w = generate_wrapper(n)
        if w:
            wrappers.append(w)

    if not wrappers:
        return None

    body = "\n\n".join(wrappers)
    ns_lower = namespace.lower()

    block = f"namespace {ns_lower}\n{{\n\n{body}\n\n}} // namespace {ns_lower}"
    return block, len(wrappers)


def fetch_json(url: str) -> any:
    print(f"Fetching {url}...")
    req = urllib.request.Request(url, headers={"User-Agent": "citizen-scripting-cpp-code-gen"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.loads(resp.read())


def parse_cfx_natives(data: list | dict) -> dict[str, list[dict]]:
    by_ns = defaultdict(list)
    if isinstance(data, list):
        for entry in data:
            ns = entry.get("ns", "CFX")
            by_ns[ns].append(entry)
    elif isinstance(data, dict):
        for key, entry in data.items():
            if isinstance(entry, dict) and "name" in entry:
                ns = entry.get("ns", "CFX")
                by_ns[ns].append(entry)
            elif isinstance(entry, dict):
                for hash_key, native in entry.items():
                    if isinstance(native, dict) and "name" in native:
                        ns = native.get("ns", key)
                        by_ns[ns].append(native)
    return dict(by_ns)


def parse_game_natives(data: dict) -> dict[str, list[dict]]:
    by_ns = defaultdict(list)
    for ns, natives in data.items():
        if not isinstance(natives, dict):
            continue
        for hash_key, native in natives.items():
            if isinstance(native, dict) and "name" in native:
                by_ns[ns].append(native)
    return dict(by_ns)


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--game", choices=GAME_URLS.keys(), default="fivem")
    parser.add_argument("--output", default="src/Native/DB.h")
    args = parser.parse_args()

    output_path = args.output
    game = args.game

    os.makedirs(os.path.dirname(output_path), exist_ok=True)

    all_ns: dict[str, list[dict]] = {}

    try:
        cfx_data = fetch_json(CFX_URL)
        for ns, natives in parse_cfx_natives(cfx_data).items():
            all_ns.setdefault(ns, []).extend(natives)
    except Exception as e:
        print(f"Failed to fetch CFX natives: {e}")

    try:
        game_data = fetch_json(GAME_URLS[game])
        for ns, natives in parse_game_natives(game_data).items():
            all_ns.setdefault(ns, []).extend(natives)
    except Exception as e:
        print(f"Failed to fetch {game} natives: {e}")

    if not all_ns:
        print("No natives fetched")
        sys.exit(1)

    blocks = []
    total_count = 0
    for ns in sorted(all_ns.keys()):
        natives = all_ns[ns]
        result = generate_namespace_block(ns, natives)
        if not result:
            continue

        block, count = result
        total_count += count
        blocks.append((ns, block, count))
        print(f"  {ns}: {count} wrappers")

    body = "\n\n".join(block for _, block, _ in blocks)

    header = f"""// Auto-generated, do not edit.
#pragma once

#include "Native.h"

namespace fx::natives
{{

{body}

}} // namespace fx::natives
"""

    with open(output_path, "w") as f:
        f.write(header)

    print(f"\nGenerated {total_count} wrappers across {len(blocks)} namespaces")
    print(f"Output: {output_path}")


if __name__ == "__main__":
    main()
