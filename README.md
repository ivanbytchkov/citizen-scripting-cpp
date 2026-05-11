# fx-cpp-sdk

A C++ runtime for CitzenFX.

<img width="886" height="101" alt="Screenshot_20260511_185310" src="https://github.com/user-attachments/assets/d07ebe60-02a7-476c-bfbf-45f11001bf5c" />
<img width="638" height="27" alt="Screenshot_20260511_185329" src="https://github.com/user-attachments/assets/461c85e8-9254-4eee-a215-2dd2db24a45b" />
<img width="421" height="81" alt="Screenshot_20260511_185401" src="https://github.com/user-attachments/assets/01d285fa-b993-4778-9fb5-f6bc7d15ed23" />
<img width="715" height="23" alt="Screenshot_20260511_185433" src="https://github.com/user-attachments/assets/9026b537-a16f-48ad-ae4a-6ff8f1af77bf" />

## Building

### Configure

```bash
mkdir build && cd build
cmake .. \
  -DCMAKE_C_COMPILER="zig;cc;-target;x86_64-linux-musl" \
  -DCMAKE_CXX_COMPILER="zig;c++;-target;x86_64-linux-musl"
```

### Compile

```bash
cd build
make -j$(nproc)
```

This produces:
- `build/libcitizen-scripting-cpp.so` - the runtime component
- `build/example.so` - the example resource

## Setup

### Install

Copy `libcitizen-scripting-cpp.so` into your server directory (next to the other components).

### Register

Copy `components.json` next to the `.so`, or add the entry to the server's existing `components.json`.

## Resources

### Writing

Include `<resource_sdk/SDK.h>` and use the `FXCPP_RESOURCE` macro:

```cpp
#include <resource_sdk/SDK.h>

FXCPP_RESOURCE
{
    fx::trace("Hello from C++!\n");

    fx::on("playerConnecting", [](const std::string& source, fx::EventArgs args) {
        std::string name = args.get<std::string>(0);
        fx::trace("Player connecting: %s\n", name.c_str());
    });

    fx::on("playerDropped", [](const std::string& source, fx::EventArgs args) {
        std::string reason = args.get<std::string>(0);
        fx::trace("Player dropped: %s\n", reason.c_str());
    });

    fx::onTick([] {
        // runs every server frame
    });
}
```

### Compile

Your resource `.so` only needs the `resource_sdk/` headers:

```bash
zig c++ -target x86_64-linux-musl -std=c++23 -shared -fPIC \
  -I /path/to/fx-cpp-sdk \
  -o myresource.so myresource.cpp
```

Or with CMake (see the example target in `CMakeLists.txt`).

### Deploy

Copy the compiled resource `.so` to your resources and give it a `fxmanifest.lua`:

```lua
fx_version 'cerulean'
game 'gta5'
game 'rdr3'

server_script 'myresource.so'
```

## To do

- Windows builds should work but are untested
- RedM is also expected to work but is untested
- `fx::onCommand` is not yet functional (I think it requires `IScriptRefRuntime`)

## License

A complete copy of the license is included in the [fx-cpp-sdk/LICENSE](./LICENSE) file.
