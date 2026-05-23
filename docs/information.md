# Information for citizen-scripting-cpp

[citizen-scripting-cpp](https://github.com/bd53/citizen-scripting-cpp) lets you write resources in C++, compiled as WebAssembly modules (`.wasm`).

It provides:
1. Near native performance via wasmtime
2. Sandboxed execution so resources can only interact with the server through defined host imports
3. Stackless coroutines (`co_await` / `co_return`) for async scheduling

### How it works

The runtime plugin (`libcitizen-scripting-cpp.so`) is loaded by FXServer and handles the lifecycle:
1. Author builds `server.cpp` to `server.wasm` with `tools/build` (see [building.md](./building.md)).
2. Server reads `fxmanifest.lua` -> sees `server_script 'server.wasm'`
3. Wasmtime loads the module and calls `__cfx_init`
4. `Server { }` block runs and it registers events, exports, timers, etc.
5. Runtime dispatches ticks, events, and ref calls to your handlers
6. On resource stop, `onStop` handlers run and everything is cleaned up

### Notes

- Resources are sandboxed, they can only interact with the server through the defined host imports.
- Child process spawning requires `set sv_wasmChildProcess "resource-name"` (or `"*"` for all) in .cfg.
- Worker threads require `set sv_wasmWorkerThreads "resource-name"` (or `"*"` for all) in .cfg.
- Resources are limited to 256MB of linear memory.
- Net events are filtered, only events registered with `fx::onNet` accept calls from clients.
- Worker threads are capped at 8 per resource.
- Timers are capped at 8192 per resource.

### Limitations

- Runtime is **server-side** only, client-side is not yet supported.
- Linux is the only supported platform (all contributions are accepted for Windows support).
- Resources cannot use C++ exceptions (`-fno-exceptions` is required).
- `IScriptStackWalkingRuntime` submits a basic frame but the host may not call it in all error scenarios.
- `IScriptProfiler` is not yet implemented. `profiler record` won't produce scope events from C++ resources.
- RedM is untested.
- The C++ API (events, exports, natives, timers, coroutines, statebags, etc.) has full parity with Lua, JS, and Mono-v2.
