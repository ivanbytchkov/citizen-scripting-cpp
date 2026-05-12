# fx-cpp-sdk

A C++ runtime for CitzenFX.

<img width="507" height="78" alt="Screenshot_20260511_200823" src="https://github.com/user-attachments/assets/8c5f351b-a243-4e39-bce4-1e638f4d7224" />
<img width="462" height="21" alt="Screenshot_20260511_201107" src="https://github.com/user-attachments/assets/03fbdbdb-bf02-434c-9140-c15e56074458" />
<img width="513" height="81" alt="Screenshot_20260511_201233" src="https://github.com/user-attachments/assets/ca1e6669-91a9-438b-8bc2-2fd44ca45fee" />
<img width="561" height="20" alt="Screenshot_20260511_201314" src="https://github.com/user-attachments/assets/e462bc8c-cd42-4ae2-a5c4-b74ea3c94b59" />

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

## License

A complete copy of the license is included in the [fx-cpp-sdk/LICENSE](./LICENSE) file.
