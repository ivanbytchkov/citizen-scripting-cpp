# Building citizen-scripting-cpp

To build the runtime plugin on Linux you need the following dependencies:

- [premake5](https://premake.github.io/) - build system generator
- [Zig](https://ziglang.org/) - musl-targeting C++ compiler (FXServer is a musl binary)
- [Rust toolchain](https://rustup.rs/) - builds wasmtime from source

Then clone with submodules:

```bash
git clone --recursive https://github.com/bd53/citizen-scripting-cpp.git
cd citizen-scripting-cpp
```

### Configure and build runtime

```bash
premake5 gmake
make -C build -f citizen-scripting-cpp.make config=release \
  CC="zig cc -target x86_64-linux-musl" \
  CXX="zig c++ -target x86_64-linux-musl" \
  -j$(nproc)
```

### Install runtime

Copy `build/bin/Release/libcitizen-scripting-cpp.so` next to your FXServer binary.

### Writing a resource

To compile a resource you need `clang++` with the `wasm32` target enabled and a wasi-sysroot.

Build your resource with `tools/build`:

```bash
tools/build path/to/server.cpp # writes path/to/server.wasm
```

Reference the built `.wasm` in your manifest:

```lua
server_script 'server.wasm'
```
