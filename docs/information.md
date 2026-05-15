# Information for citizen-scripting-cpp

[citizen-scripting-cpp](https://github.com/bd53/citizen-scripting-cpp) lets you write resources in C++, compiled as native shared libraries (`.so`) or WebAssembly modules (`.wasm`).

It provides:
1. Native code with zero interop overhead, no GC pauses
2. Optional WebAssembly mode isolates resources

### How it works

The runtime plugin (`libcitizen-scripting-cpp.so`) is loaded by FXServer and handles the lifecycle:
1. Server reads `fxmanifest.lua` -> sees `server_script 'server.so'` (or `.wasm`)
2. Runtime loads your library and calls `fxcpp_init`
3. `Server { }` block runs and it registers events, exports, timers, etc.
4. Runtime dispatches ticks, events, and ref calls to your handlers
5. On resource stop, `onStop` handlers run and everything is cleaned up

### Notes

- This runtime is **server-side** only, client-side is not yet supported.
- Linux is the only supported platform (this doesn't mean not open to a PR for Windows support, all contributions are accepted).
- `.so` resources run as native code and have full access to the host system, only load `.so` resources you trust.
- Loading a `.so` resource is blocked by default, requires `sv_allowNativeCode true` in .cfg.
- `.wasm` resources are sandboxed via wasmtime, they can only interact with the server through the defined host imports.
- `.wasm` child process spawning requires `set sv_wasmChildProcess "resource-name"` (or `"*"` for all) in your .cfg.
- `.wasm` worker threads require `set sv_wasmWorkerThreads "resource-name"` (or `"*"` for all) in your .cfg.
- Net events are filtered so only events registered with `fx::onNet` accept calls from clients, matching other ScRT's.

### Limitations

- As mentioned previously, client-side is **not** yet supported.
- WASM resources cannot use C++ exceptions (`-fno-exceptions` is required).
- `IScriptStackWalkingRuntime` is no-op. C++ resources won't appear in cross-runtime stack traces from `FORMAT_STACK_TRACE`.
- `IScriptProfiler` is not yet implemented. `profiler record` won't produce scope events from C++ resources.

### Compatibility

- The C++ API (events, exports, natives, timers, coroutines, statebags, etc.) has full parity with Lua, JS, and Mono-v2.
- The same source file can target both `.so` and `.wasm`, only the `#include` differs (`SDK.h` vs `WASM.h`), all API calls are identical.

### Differences

#### Engine and execution

| | Lua | Node | Mono-v2 | .so | .wasm |
|---|---|---|---|---|---|
| Engine | Lua 5.4 | V8 + Node.js | Mono JIT | Native (dlopen) | wasmtime |
| Language std | Lua 5.4 | ES2020+ | .NET | C++23 | C++23 |
| GC | Incremental | V8 GC | Mono GC | None (manual) | None (manual) |
| Sandboxing | Restricted stdlib | Filesystem permissions | AppDomain isolation | None | WASM sandbox |
| Custom allocator | rpmalloc | V8 heap limits | Mono allocator | System malloc | WASM linear memory |

#### Interfaces implemented

All runtimes implement `IScriptRuntime`, `IScriptFileHandlingRuntime`, `IScriptEventRuntime`, and `IScriptRefRuntime`.

| Interface | Lua | Node | Mono-v2 | C++ |
|---|---|---|---|---|
| `IScriptTickRuntimeWithBookmarks` | Yes | No | No | Yes |
| `IScriptTickRuntime` | No | Yes | Yes (tick-less) | Yes |
| `IScriptStackWalkingRuntime` | Yes | Yes | No | No-op |
| `IScriptMemInfoRuntime` | Yes | No | Yes | Yes |
| `IScriptProfiler` | Yes | No (uses V8 CpuProfiler) | Yes | No-op |
| `IScriptDebugRuntime` | Yes | No | Yes | No |
| `IScriptWarningRuntime` | Yes | Yes | No | Yes |

#### Scheduling

| | Lua | Node | Mono-v2 | C++ |
|---|---|---|---|---|
| Tick model | Bookmark scheduler | UV loop timer | Tick-less (scheduled time) | Bookmark scheduler |
| Coroutines | `CreateThread` / `Wait` | `Promises` / `async-await` | `async` / `await` (Task) | `co_await` / `fx::Wait{}` |

Mono-v2's tick-less optimization skips runtime entry/exit entirely when there's no scheduled work, `GetCurrentSchedulerTime() < m_sharedData.m_scheduledTime` short-circuits the tick.

Lua and C++ use the bookmark scheduler (`IScriptTickRuntimeWithBookmarks`), which lets the host call `TickBookmarks` with only the bookmarks that are ready, avoiding unnecessary tick overhead.

#### Stdlib restrictions

| | Lua | Node | Mono-v2 | .so | .wasm |
|---|---|---|---|---|---|
| File I/O | `io`/`os` (server only) | Filesystem permission callbacks | Full .NET IO | Full POSIX | WASI (host-controlled) |
| Spawning processes | No | `child_process` (permission-gated) | `System.Diagnostics` | Full access | `fx::spawnProcess` (permission-gated) |
| Workers/threads | No | `worker_threads` (permission-gated) | `System.Threading` | Full access | `fx::createWorker` (permission-gated) |
