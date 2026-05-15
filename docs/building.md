# Building citizen-scripting-cpp

To build the runtime plugin on Linux you need the following dependencies:

- [g++](https://gcc.gnu.org/) with C++23 support (GCC 13 or higher recommended)
- [premake5](https://premake.github.io/)
- [Rust toolchain](https://rustup.rs/) - for building wasmtime from source
- [Zig](https://ziglang.org/) - for musl libc cross-compilation
- [clang++](https://clang.llvm.org/) with [WASI sysroot](https://github.com/WebAssembly/wasi-sdk) - for compiling `.wasm` resources
- [FXServer](https://runtime.fivem.net/artifacts/fivem/build_proot_linux/master/)

Then clone with submodules and build wasmtime first (one-time):

```bash
git clone --recursive https://github.com/bd53/citizen-scripting-cpp.git
cd citizen-scripting-cpp

cargo build --release -p wasmtime-c-api \
  --target x86_64-unknown-linux-musl \
  --manifest-path vendor/wasmtime/Cargo.toml
```

Generate and build:

```bash
premake5 gmake2 --wasm
cd build && make -f citizen-scripting-cpp.make config=release \
  CC="zig cc -target x86_64-linux-musl" \
  CXX="zig c++ -target x86_64-linux-musl" \
  -j$(nproc)
```

### Install the runtime

After building the runtime, you should have `build/bin/Release/libcitizen-scripting-cpp.so`, copy it next to FXServer binary.

### Compile a resource

Use the `tools/build` utility, it auto-detects the target from the output extension and sets all required flags:

```bash
# Native shared library
/path/to/citizen-scripting-cpp/tools/build server.cpp -o server.so

# WebAssembly module
/path/to/citizen-scripting-cpp/tools/build server.cpp -o server.wasm
```

Extra flags are passed through to the compiler (e.g. `-O3`, `-Wall`, `-DDEBUG`).

### Automated build and deploy

`tools/deploy` runs the full pipeline, code generation, premake, compilation, and deployment:

```bash
export FX_SERVER_DIR=/path/to/cfx-server
export FX_RESOURCE_DIR=/path/to/resources/example

# Build and deploy (native)                                                                            
tools/deploy              

# Build and deploy (WASM) 
tools/deploy --type wasm
```

This will:
1. Initialize the wasmtime submodule if needed
2. Regenerate the native database (`src/Native/DB.h`)
3. Run premake5 and compile the runtime
4. Compile the example resource (`tools/example`) in whichever mode specified (`native` / `wasm`)
5. Deploy the runtime `.so` and example resource to your server

### Regenerating the native database

`src/Native/DB.h` is auto-generated from upstream native definitions.

Utility `tools/deploy` does this automatically, but you can also run it standalone:

```bash
# FiveM (GTA5) natives
python3 tools/code-gen/build.py

# RedM (RDR3) natives
python3 tools/code-gen/build.py --game redm
```

### Skip building the runtime

If building the runtime is complex, download `libcitizen-scripting-cpp.so` from the [releases](https://github.com/bd53/citizen-scripting-cpp/releases) and drop it next to your FXServer binary.

To compile resources you only need:

- [clang++](https://clang.llvm.org/) with [WASI sysroot](https://github.com/WebAssembly/wasi-sdk)
- This repository (for the SDK headers and `tools/build`)

```bash
git clone https://github.com/bd53/citizen-scripting-cpp.git

# Native shared library
/path/to/citizen-scripting-cpp/tools/build server.cpp -o server.so

# WebAssembly module
/path/to/citizen-scripting-cpp/tools/build server.cpp -o server.wasm
```

**Known issues**

- WASM resource fails to load with missing fxcpp_init export

  The resource was not compiled with the required exports.

  You should be using `tools/build` or passing the correct `-Wl,--export=...` flags manually.

- wasmtime-c-api build fails with a linker error

  Ensure `zig` is in your `PATH` and the Rust target is installed:
  ```bash
  rustup target add x86_64-unknown-linux-musl
  ```

- WASM resource traps immediately on the first tick

  It's possible the resource is doing too much work in a single host call, hitting the fuel limit (1 billion instructions per call).

  Break up heavy work across multiple ticks or offload it to a worker via `fx::createWorker`.

- dlopen() failed: symbol not found when the server starts

  The `.so` was compiled against a different set of symbols than what the server expects. 

  Make sure you're building with the musl toolchain (`zig cc -target x86_64-linux-musl`) when targeting FXServer, which is also a musl binary.

- error: cannot use 'try' with exceptions disabled

  WASM resources must be compiled with `-fno-exceptions`. `tools/build` sets this automatically, if you're invoking clang++ manually, add `-fno-exceptions` to your flags.
