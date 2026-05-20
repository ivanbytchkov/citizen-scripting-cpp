#include "../include/CppScriptRuntime.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <climits>
#include <vector>

#include <atomic>
#include <cstdarg>
#include <fcntl.h>
#include <unistd.h>

using namespace fx::cpp;

static std::atomic<uint32_t> s_nextInstanceId{ 1 };

static constexpr int64_t WASM_MEMORY_LIMIT = 256 * 1024 * 1024;
static constexpr uint64_t WASM_FUEL_AMOUNT = 1000000000ULL;
static constexpr uint32_t WORKER_RESULT_BUF_SIZE = 65536;
static constexpr int WORKER_SHUTDOWN_ATTEMPTS = 50;
static constexpr int WORKER_SHUTDOWN_INTERVAL_MS = 100;
static constexpr size_t MAX_BOOKMARKS_PER_RESOURCE = 1024;

static std::string GetResourcePath(IScriptHost* host)
{
        fx::OMPtr<IScriptHostWithResourceData> md;
        fx::OMPtr<IScriptHost> h(host);
        if (FX_FAILED(h.As(&md)) || !md.GetRef())
                return { };
        char* name = nullptr;
        if (FX_FAILED(md->GetResourceName(&name)) || !name)
                return { };
        fxNativeContext ctx{ };
        ctx.nativeIdentifier = HashString("GET_RESOURCE_PATH");
        ctx.arguments[0] = reinterpret_cast<uintptr_t>(name);
        ctx.numArguments = 1;
        ctx.numResults = 1;
        host->InvokeNative(ctx);
        const char* path = reinterpret_cast<const char*>(ctx.arguments[0]);
        return path ? std::string(path) : std::string{ };
}

static std::string GetConvar(IScriptHost* host, const char* name, const char* defaultValue)
{
        fxNativeContext ctx{ };
        ctx.nativeIdentifier = HashString("GET_CONVAR");
        ctx.arguments[0] = reinterpret_cast<uintptr_t>(name);
        ctx.arguments[1] = reinterpret_cast<uintptr_t>(defaultValue);
        ctx.numArguments = 2;
        ctx.numResults = 1;
        if (FX_FAILED(host->InvokeNative(ctx)))
                return defaultValue;
        const char* result = reinterpret_cast<const char*>(ctx.arguments[0]);
        return result ? std::string(result) : std::string(defaultValue);
}

enum class LogLevel { Warning, Error };

__attribute__((format(printf, 2, 3)))
static void Log(LogLevel level, const char* fmt, ...)
{
        va_list ap;
        va_start(ap, fmt);
        fprintf(stderr, "%s[citizen-scripting-cpp]\033[0m ", level == LogLevel::Error ? "\033[31m" : "\033[33m");
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
        va_end(ap);
}

#define LogError(...) Log(LogLevel::Error, __VA_ARGS__)
#define LogWarning(...) Log(LogLevel::Warning, __VA_ARGS__)

static bool ValidateScriptPath(const char* scriptFile, const std::string& root, std::string& resolvedPath, std::string& resolvedRoot)
{
        std::string_view scriptFileView(scriptFile);
        if (scriptFileView.find("/..") != std::string_view::npos || scriptFileView.find("../") != std::string_view::npos || scriptFileView == "..")
        {
                LogError("Rejected script path with '..': '%s'", scriptFile);
                return false;
        }
        std::string fullPath = root + "/" + scriptFile;
        char* resolvedFull = realpath(fullPath.c_str(), nullptr);
        if (!resolvedFull)
        {
                LogError("realpath failed for '%s'", fullPath.c_str());
                return false;
        }
        resolvedPath = resolvedFull;
        free(resolvedFull);
        char* resolvedRootBuf = realpath(root.c_str(), nullptr);
        if (!resolvedRootBuf)
        {
                LogError("realpath failed for root '%s'", root.c_str());
                return false;
        }
        resolvedRoot = resolvedRootBuf;
        free(resolvedRootBuf);
        if (resolvedPath.compare(0, resolvedRoot.size(), resolvedRoot) != 0 || (resolvedPath.size() > resolvedRoot.size() && resolvedPath[resolvedRoot.size()] != '/'))
        {
                LogError("Script path '%s' resolves outside resource root", scriptFile);
                return false;
        }
        return true;
}

static wasmtime_val_t I32Val(int32_t v)
{
        wasmtime_val_t r{ };
        r.kind = WASMTIME_I32;
        r.of.i32 = v;
        return r;
}

static constexpr size_t MAX_FUNC_TYPE_PARAMS = 16;

static wasm_functype_t* MakeFuncType(std::initializer_list<wasm_valkind_t> params, std::initializer_list<wasm_valkind_t> results)
{
        wasm_valtype_t* pv[MAX_FUNC_TYPE_PARAMS];
        wasm_valtype_t* rv[MAX_FUNC_TYPE_PARAMS];
        size_t pi = 0, ri = 0;
        for (auto k : params)
                if (pi < MAX_FUNC_TYPE_PARAMS)
                        pv[pi++] = wasm_valtype_new(k);
        for (auto k : results)
                if (ri < MAX_FUNC_TYPE_PARAMS)
                        rv[ri++] = wasm_valtype_new(k);
        wasm_valtype_vec_t p_vec{ }, r_vec{ };
        wasm_valtype_vec_new(&p_vec, pi, pv);
        wasm_valtype_vec_new(&r_vec, ri, rv);
        return wasm_functype_new(&p_vec, &r_vec);
}

static bool CallerMemory(wasmtime_caller_t* caller, wasmtime_context_t* ctx, uint8_t** base, size_t* sz)
{
        wasmtime_extern_t ext{ };
        if (!wasmtime_caller_export_get(caller, "memory", 6, &ext))
                return false;
        if (ext.kind != WASMTIME_EXTERN_MEMORY)
                return false;
        *base = wasmtime_memory_data(ctx, &ext.of.memory);
        *sz = wasmtime_memory_data_size(ctx, &ext.of.memory);
        return *base != nullptr;
}

static bool InBounds(size_t memSz, uint32_t offset, size_t len)
{
        return static_cast<size_t>(offset) <= memSz && len <= memSz - static_cast<size_t>(offset);
}

static inline uint32_t ArgU32(const wasmtime_val_t& v) { return static_cast<uint32_t>(v.of.i32); }

struct WasmMem
{
        uint8_t* base = nullptr;
        size_t sz = 0;
        bool init(wasmtime_caller_t* caller)
        {
                auto* ctx = wasmtime_caller_context(caller);
                return CallerMemory(caller, ctx, &base, &sz);
        }
        bool check(uint32_t offset, size_t len) const { return InBounds(sz, offset, len); }
};

static std::string ExtractWasmError(wasmtime_error_t* err, wasm_trap_t* trap)
{
        wasm_name_t msg{ };
        if (err)
        {
                wasmtime_error_message(err, &msg);
                wasmtime_error_delete(err);
        }
        else if (trap)
        {
                wasm_trap_message(trap, &msg);
                wasm_trap_delete(trap);
        }
        std::string out(msg.data, msg.size);
        wasm_byte_vec_delete(&msg);
        return out;
}

static bool WasmCall(wasmtime_store_t* store, const wasmtime_func_t& fn, const wasmtime_val_t* args, size_t nargs, wasmtime_val_t* results, size_t nresults, const char* resourceName, const char* label)
{
        wasm_trap_t* trap = nullptr;
        auto* err = wasmtime_func_call(wasmtime_store_context(store), &fn, args, nargs, results, nresults, &trap);
        if (err || trap)
        {
                auto msg = ExtractWasmError(err, trap);
                fprintf(stderr, "\033[31m[%s] %s:\033[0m %s\n", resourceName, label, msg.c_str());
                return false;
        }
        return true;
}

static void SanitizeTraceMsg(std::string& msg)
{
        size_t out = 0;
        for (size_t i = 0; i < msg.size(); ++i)
        {
                unsigned char c = static_cast<unsigned char>(msg[i]);
                if (c == '\n' || c == '\t' || (c >= 0x20 && c != 0x1B && c != 0x7F))
                {
                        if (c == 0xE2 && i + 2 < msg.size())
                        {
                                unsigned char c1 = static_cast<unsigned char>(msg[i + 1]);
                                unsigned char c2 = static_cast<unsigned char>(msg[i + 2]);
                                // U+200B-200F
                                if (c1 == 0x80 && (c2 >= 0x8B && c2 <= 0x8F))
                                {
                                        i += 2;
                                        continue;
                                }
                                // U+2028-2029
                                if (c1 == 0x80 && (c2 == 0xA8 || c2 == 0xA9))
                                {
                                        i += 2;
                                        continue;
                                }
                                // U+202A-202E
                                if (c1 == 0x80 && (c2 >= 0xAA && c2 <= 0xAE))
                                {
                                        i += 2;
                                        continue;
                                }
                                // U+2060-2064
                                if (c1 == 0x81 && (c2 >= 0xA0 && c2 <= 0xA4))
                                {
                                        i += 2;
                                        continue;
                                }
                                // U+2066-2069
                                if (c1 == 0x81 && (c2 >= 0xA6 && c2 <= 0xA9))
                                {
                                        i += 2;
                                        continue;
                                }
                        }
                        // U+FEFF
                        if (c == 0xEF && i + 2 < msg.size())
                        {
                                unsigned char c1 = static_cast<unsigned char>(msg[i + 1]);
                                unsigned char c2 = static_cast<unsigned char>(msg[i + 2]);
                                if (c1 == 0xBB && c2 == 0xBF)
                                {
                                        i += 2;
                                        continue;
                                }
                        }
                        msg[out++] = msg[i];
                }
        }
        msg.resize(out);
}

static wasm_trap_t* CbTrace(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        WasmMem mem;
        if (!mem.init(caller))
                return nullptr;
        uint32_t ptr = ArgU32(args[0]);
        uint32_t len = ArgU32(args[1]);
        if (!mem.check(ptr, len))
                return nullptr;
        std::string msg(reinterpret_cast<const char*>(mem.base + ptr), len);
        SanitizeTraceMsg(msg);
        if (rt->host())
                rt->host()->ScriptTrace(const_cast<char*>(msg.c_str()));
        fprintf(stderr, "\033[36m[script:%s]\033[0m %s", rt->resourceName().c_str(), msg.c_str());
        if (!msg.empty() && msg.back() != '\n')
                fputc('\n', stderr);
        return nullptr;
}

static wasm_trap_t* CbInvokeNative(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        rt->m_hasValidNativeResult = false;
        WasmMem mem;
        if (!mem.init(caller))
                return nullptr;
        uint32_t ctxPtr = ArgU32(args[0]);
        if (!mem.check(ctxPtr, sizeof(WasmNativeCtx)))
                return nullptr;
        WasmNativeCtx wctx{ };
        memcpy(&wctx, mem.base + ctxPtr, sizeof(WasmNativeCtx));
        fxNativeContext hostCtx{ };
        hostCtx.nativeIdentifier = wctx.hash;
        if (wctx.numArgs > 32)
                wctx.numArgs = 32;
        if (wctx.numResults > 32)
                wctx.numResults = 32;
        hostCtx.numArguments = static_cast<int>(wctx.numArgs);
        hostCtx.numResults = static_cast<int>(wctx.numResults);
        for (uint32_t i = 0; i < wctx.numArgs; ++i)
        {
                if ((wctx.ptrMask >> i) & 1u)
                {
                        uint32_t off = static_cast<uint32_t>(wctx.args[i]);
                        if (off == 0)
                                hostCtx.arguments[i] = 0;
                        else if (!mem.check(off, 1))
                                return nullptr;
                        else
                                hostCtx.arguments[i] = reinterpret_cast<uintptr_t>(mem.base + off);
                }
                else
                {
                        hostCtx.arguments[i] = static_cast<uintptr_t>(wctx.args[i]);
                }
        }
        if (rt->host())
                rt->host()->InvokeNative(hostCtx);
        rt->m_lastNativeCtx = hostCtx;
        rt->m_lastResultPtrMask = wctx.resultPtrMask;
        rt->m_hasValidNativeResult = true;
        for (int i = 0; i < hostCtx.numResults; ++i)
        {
                if ((wctx.resultPtrMask >> i) & 1u)
                        wctx.args[i] = (hostCtx.arguments[i] != 0) ? 1 : 0;
                else
                        wctx.args[i] = static_cast<uint64_t>(hostCtx.arguments[i]);
        }
        mem.init(caller);
        if (!mem.check(ctxPtr, sizeof(WasmNativeCtx)))
                return nullptr;
        memcpy(mem.base + ctxPtr, &wctx, sizeof(WasmNativeCtx));
        return nullptr;
}

static wasm_trap_t* CbCopyStringResult(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        if (!rt->m_hasValidNativeResult)
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        WasmMem mem;
        if (!mem.init(caller))
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        int32_t resultIdx = args[1].of.i32;
        uint32_t bufPtr = ArgU32(args[2]);
        int32_t bufMax = args[3].of.i32;
        if (resultIdx < 0 || resultIdx >= 32 || !((rt->m_lastResultPtrMask >> resultIdx) & 1u))
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        const char* str = reinterpret_cast<const char*>(rt->m_lastNativeCtx.arguments[resultIdx]);
        if (!str)
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        static constexpr size_t MAX_STRING_RESULT = 1u << 20;
        size_t len = strnlen(str, MAX_STRING_RESULT);
        size_t copy = (bufMax > 1) ? std::min<size_t>(len, static_cast<size_t>(bufMax) - 1) : 0;
        if (copy && mem.check(bufPtr, copy + 1))
        {
                memcpy(mem.base + bufPtr, str, copy);
                mem.base[bufPtr + copy] = '\0';
        }
        results[0] = I32Val(static_cast<int32_t>(len));
        return nullptr;
}

static wasm_trap_t* CbEmitEvent(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        WasmMem mem;
        if (!mem.init(caller))
                return nullptr;
        uint32_t namePtr = ArgU32(args[0]), nameLen = ArgU32(args[1]);
        uint32_t argsPtr = ArgU32(args[2]), argsLen = ArgU32(args[3]);
        if (!mem.check(namePtr, nameLen) || !mem.check(argsPtr, argsLen))
                return nullptr;
        std::string evName(reinterpret_cast<const char*>(mem.base + namePtr), nameLen);
        std::vector<uint8_t> argsCopy(mem.base + argsPtr, mem.base + argsPtr + argsLen);
        if (!rt->host())
                return nullptr;
        fxNativeContext nctx{ };
        nctx.nativeIdentifier = HashString("TRIGGER_EVENT_INTERNAL");
        nctx.arguments[0] = reinterpret_cast<uintptr_t>(evName.c_str());
        nctx.arguments[1] = reinterpret_cast<uintptr_t>(argsCopy.data());
        nctx.arguments[2] = static_cast<uintptr_t>(argsLen);
        nctx.arguments[3] = reinterpret_cast<uintptr_t>("-1");
        nctx.numArguments = 4;
        nctx.numResults = 0;
        rt->host()->InvokeNative(nctx);
        return nullptr;
}

static wasm_trap_t* CbEmitNetEvent(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        WasmMem mem;
        if (!mem.init(caller))
                return nullptr;
        uint32_t namePtr = ArgU32(args[0]), nameLen = ArgU32(args[1]);
        int32_t target = args[2].of.i32;
        uint32_t argsPtr = ArgU32(args[3]), argsLen = ArgU32(args[4]);
        if (!mem.check(namePtr, nameLen) || !mem.check(argsPtr, argsLen))
                return nullptr;
        std::string evName(reinterpret_cast<const char*>(mem.base + namePtr), nameLen);
        std::vector<uint8_t> argsCopy(mem.base + argsPtr, mem.base + argsPtr + argsLen);
        std::string targetStr = std::to_string(target);
        if (!rt->host())
                return nullptr;
        fxNativeContext nctx{ };
        nctx.nativeIdentifier = HashString("TRIGGER_CLIENT_EVENT_INTERNAL");
        nctx.arguments[0] = reinterpret_cast<uintptr_t>(evName.c_str());
        nctx.arguments[1] = reinterpret_cast<uintptr_t>(targetStr.c_str());
        nctx.arguments[2] = reinterpret_cast<uintptr_t>(argsCopy.data());
        nctx.arguments[3] = static_cast<uintptr_t>(argsLen);
        nctx.numArguments = 4;
        nctx.numResults = 0;
        rt->host()->InvokeNative(nctx);
        return nullptr;
}

static wasm_trap_t* CbCancelEvent(void* env, wasmtime_caller_t*, const wasmtime_val_t*, size_t, wasmtime_val_t*, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        rt->m_eventCanceled = true;
        if (rt->host())
        {
                fxNativeContext nctx{ };
                nctx.nativeIdentifier = HashString("CANCEL_EVENT");
                nctx.numArguments = 0;
                nctx.numResults = 0;
                rt->host()->InvokeNative(nctx);
        }
        return nullptr;
}

static wasm_trap_t* CbWasEventCanceled(void* env, wasmtime_caller_t*, const wasmtime_val_t*, size_t, wasmtime_val_t* results, size_t)
{
        results[0] = I32Val(static_cast<CppScriptRuntime*>(env)->m_eventCanceled ? 1 : 0);
        return nullptr;
}

static wasm_trap_t* CbGetResourceMetadata(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        WasmMem mem;
        if (!mem.init(caller))
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        uint32_t keyPtr = ArgU32(args[0]), keyLen = ArgU32(args[1]);
        int32_t index = args[2].of.i32;
        uint32_t bufPtr = ArgU32(args[3]);
        int32_t bufMax = args[4].of.i32;
        if (!mem.check(keyPtr, keyLen))
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        std::string key(reinterpret_cast<const char*>(mem.base + keyPtr), keyLen);
        auto* md = rt->metadataHost();
        std::string value;
        if (md)
        {
                char* val = nullptr;
                if (FX_SUCCEEDED(md->GetResourceMetaData(const_cast<char*>(key.c_str()), index, &val)) && val)
                        value = val;
        }
        size_t valueLen = value.size();
        if (bufMax > 0 && mem.check(bufPtr, static_cast<size_t>(bufMax)))
        {
                size_t copy = std::min<size_t>(valueLen, static_cast<size_t>(bufMax) - 1);
                memcpy(mem.base + bufPtr, value.data(), copy);
                mem.base[bufPtr + copy] = '\0';
        }
        results[0] = I32Val(static_cast<int32_t>(std::min<size_t>(valueLen, INT32_MAX)));
        return nullptr;
}

static wasm_trap_t* CbGetNumResourceMetadata(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        WasmMem mem;
        if (!mem.init(caller))
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        uint32_t keyPtr = ArgU32(args[0]), keyLen = ArgU32(args[1]);
        if (!mem.check(keyPtr, keyLen))
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        std::string key(reinterpret_cast<const char*>(mem.base + keyPtr), keyLen);
        auto* md = rt->metadataHost();
        int32_t count = 0;
        if (md)
                md->GetNumResourceMetaData(const_cast<char*>(key.c_str()), &count);
        results[0] = I32Val(count);
        return nullptr;
}

static wasm_trap_t* CbIsManifestVersionV2Between(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        WasmMem mem;
        if (!mem.init(caller))
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        uint32_t lowerPtr = ArgU32(args[0]), lowerLen = ArgU32(args[1]);
        uint32_t upperPtr = ArgU32(args[2]), upperLen = ArgU32(args[3]);
        if (!mem.check(lowerPtr, lowerLen) || !mem.check(upperPtr, upperLen))
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        std::string lower(reinterpret_cast<const char*>(mem.base + lowerPtr), lowerLen);
        std::string upper(reinterpret_cast<const char*>(mem.base + upperPtr), upperLen);
        auto* mh = rt->manifestHost();
        bool result = false;
        if (mh)
                mh->IsManifestVersionV2Between(const_cast<char*>(lower.c_str()), const_cast<char*>(upper.c_str()), &result);
        results[0] = I32Val(result ? 1 : 0);
        return nullptr;
}

static wasm_trap_t* CbCreateRef(void* env, wasmtime_caller_t*, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        uint32_t callbackId = ArgU32(args[0]);
        std::weak_ptr<CppScriptRuntime::RefGuard> guardWeak = rt->m_refGuard;
        int32_t refIdx = rt->AddFuncRef([rt, callbackId, guardWeak](const char* argsSerialized, uint32_t argsSize) -> std::vector<char>
        {
                auto guard = guardWeak.lock();
                if (!guard)
                        throw std::runtime_error("Runtime destroyed, ref callback invalid");
                std::lock_guard<std::mutex> lk(guard->mu);
                if (!guard->alive)
                        throw std::runtime_error("Runtime destroyed, ref callback invalid");
                std::vector<char> result;
                if (!rt->callInvokeRef(callbackId, argsSerialized, argsSize, result))
                        throw std::runtime_error("WASM trap in ref callback");
                return result;
        });
        rt->wasmMapRef(refIdx, callbackId);
        results[0] = I32Val(refIdx);
        return nullptr;
}

static wasm_trap_t* CbCanonicalizeRef(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        WasmMem mem;
        if (!mem.init(caller))
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        int32_t refIdx = args[0].of.i32;
        uint32_t bufPtr = ArgU32(args[1]);
        int32_t bufMax = args[2].of.i32;
        if (!rt->host())
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        char* refString = nullptr;
        rt->host()->CanonicalizeRef(refIdx, rt->GetInstanceId(), &refString);
        if (!refString)
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        size_t len = strlen(refString);
        if (bufMax > 0 && mem.check(bufPtr, std::min<size_t>(len + 1, static_cast<size_t>(bufMax))))
        {
                size_t copy = std::min<size_t>(len, static_cast<size_t>(bufMax) - 1);
                memcpy(mem.base + bufPtr, refString, copy);
                mem.base[bufPtr + copy] = '\0';
        }
        fwFree(refString);
        results[0] = I32Val(static_cast<int32_t>(len));
        return nullptr;
}

static wasm_trap_t* CbRemoveRef(void* env, wasmtime_caller_t*, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        int32_t refIdx = args[0].of.i32;
        rt->RemoveRef(refIdx);
        return nullptr;
}

static wasm_trap_t* CbInvokeFunctionReference(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        WasmMem mem;
        if (!mem.init(caller))
                return nullptr;
        uint32_t refPtr = ArgU32(args[0]), refLen = ArgU32(args[1]);
        uint32_t argsPtr = ArgU32(args[2]), argsLen = ArgU32(args[3]);
        uint32_t outPtr = ArgU32(args[4]);
        if (!mem.check(refPtr, refLen) || !mem.check(argsPtr, argsLen) || !mem.check(outPtr, 8))
                return nullptr;
        std::string refStr(reinterpret_cast<const char*>(mem.base + refPtr), refLen);
        std::vector<char> argsCopy(reinterpret_cast<char*>(mem.base + argsPtr), reinterpret_cast<char*>(mem.base + argsPtr) + argsLen);
        if (!rt->host())
                return nullptr;
        fx::OMPtr<IScriptBuffer> retBuf;
        rt->host()->InvokeFunctionReference(const_cast<char*>(refStr.c_str()), argsCopy.data(), argsLen, retBuf.ReleaseAndGetAddressOf());
        uint32_t dataPtr = 0, dataLen = 0;
        if (retBuf.GetRef() && retBuf->GetLength() > 0)
        {
                dataLen = static_cast<uint32_t>(retBuf->GetLength());
                dataPtr = rt->wasmAlloc(dataLen);
                if (!dataPtr)
                        dataLen = 0;
        }
        mem.init(caller);
        if (dataPtr && dataLen && mem.check(dataPtr, dataLen))
                memcpy(mem.base + dataPtr, retBuf->GetBytes(), dataLen);
        else if (dataPtr)
        {
                rt->wasmFree(dataPtr, dataLen);
                mem.init(caller);
                dataPtr = 0;
                dataLen = 0;
        }
        if (mem.check(outPtr, 8))
        {
                memcpy(mem.base + outPtr, &dataPtr, 4);
                memcpy(mem.base + outPtr + 4, &dataLen, 4);
        }
        return nullptr;
}

static wasm_trap_t* CbGetInstanceId(void* env, wasmtime_caller_t*, const wasmtime_val_t*, size_t, wasmtime_val_t* results, size_t)
{
        results[0] = I32Val(static_cast<CppScriptRuntime*>(env)->GetInstanceId());
        return nullptr;
}

static bool HasWasmPermission(IScriptHost* host, const char* convarName, const std::string& resourceName)
{
        std::string allowed = GetConvar(host, convarName, "");
        if (allowed.empty())
                return false;
        if (allowed == "*")
                return true;
        size_t pos = 0;
        while (pos < allowed.size())
        {
                size_t end = allowed.find(',', pos);
                if (end == std::string::npos)
                        end = allowed.size();
                size_t start = pos;
                while (start < end && allowed[start] == ' ')
                        ++start;
                size_t last = end;
                while (last > start && allowed[last - 1] == ' ')
                        --last;
                if (std::string_view(allowed.data() + start, last - start) == resourceName)
                        return true;
                pos = end + 1;
        }
        return false;
}

static wasm_trap_t* CbSpawnProcess(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        WasmMem mem;
        if (!mem.init(caller))
        {
                results[0] = I32Val(-2);
                return nullptr;
        }
        uint32_t cmdPtr = ArgU32(args[0]), cmdLen = ArgU32(args[1]);
        uint32_t outBuf = ArgU32(args[2]);
        int32_t outMax = args[3].of.i32;
        if (!mem.check(cmdPtr, cmdLen))
        {
                results[0] = I32Val(-2);
                return nullptr;
        }
        if (!rt->host() || !HasWasmPermission(rt->host(), "sv_wasmChildProcess", rt->resourceName()))
        {
                LogError("Resource '%s' denied child_process permission. Add 'set sv_wasmChildProcess \"%s\"' to your server.cfg", rt->resourceName().c_str(), rt->resourceName().c_str());
                results[0] = I32Val(-1);
                return nullptr;
        }
        std::string cmd(reinterpret_cast<const char*>(mem.base + cmdPtr), cmdLen);
        LogWarning("Resource '%s' spawning child process: %.256s%s", rt->resourceName().c_str(), cmd.c_str(), cmd.size() > 256 ? "..." : "");
        fx::ProcessResult pr = fx::spawnProcess(cmd);
        rt->m_lastSpawnExitCode = pr.exitCode;
        if (pr.status == -2 || pr.status == -3)
        {
                results[0] = I32Val(pr.status);
                return nullptr;
        }
        mem.init(caller);
        int32_t written = 0;
        if (outMax > 0 && mem.check(outBuf, static_cast<size_t>(outMax)))
        {
                size_t copy = std::min<size_t>(pr.output.size(), static_cast<size_t>(outMax) - 1);
                memcpy(mem.base + outBuf, pr.output.data(), copy);
                mem.base[outBuf + copy] = '\0';
                written = static_cast<int32_t>(copy);
        }
        else if (!pr.output.empty())
        {
                LogError("spawn_process: output buffer out of bounds");
        }
        results[0] = I32Val(written);
        return nullptr;
}

static wasm_trap_t* CbGetLastSpawnExitCode(void* env, wasmtime_caller_t*, const wasmtime_val_t*, size_t, wasmtime_val_t* results, size_t)
{
        results[0] = I32Val(static_cast<CppScriptRuntime*>(env)->m_lastSpawnExitCode);
        return nullptr;
}

static wasm_trap_t* CbCreateWorker(void*, wasmtime_caller_t*, const wasmtime_val_t*, size_t, wasmtime_val_t*, size_t);
static wasm_trap_t* CbPollWorker(void*, wasmtime_caller_t*, const wasmtime_val_t*, size_t, wasmtime_val_t*, size_t);
static wasm_trap_t* CbScheduleBookmark(void*, wasmtime_caller_t*, const wasmtime_val_t*, size_t, wasmtime_val_t*, size_t);

struct ImportDesc
{
        const char* name;
        std::initializer_list<wasm_valkind_t> params;
        std::initializer_list<wasm_valkind_t> results;
        wasmtime_func_callback_t hostCb;
};

static const ImportDesc g_imports[] = {
        { "trace", { WASM_I32, WASM_I32 }, { }, CbTrace },
        { "invoke_native", { WASM_I32 }, { }, CbInvokeNative },
        { "copy_string_result", { WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }, CbCopyStringResult },
        { "emit_event", { WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { }, CbEmitEvent },
        { "emit_net_event", { WASM_I32, WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { }, CbEmitNetEvent },
        { "cancel_event", { }, { }, CbCancelEvent },
        { "was_event_canceled", { }, { WASM_I32 }, CbWasEventCanceled },
        { "get_resource_metadata", { WASM_I32, WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }, CbGetResourceMetadata },
        { "get_num_resource_metadata", { WASM_I32, WASM_I32 }, { WASM_I32 }, CbGetNumResourceMetadata },
        { "create_ref", { WASM_I32 }, { WASM_I32 }, CbCreateRef },
        { "canonicalize_ref", { WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }, CbCanonicalizeRef },
        { "remove_ref", { WASM_I32 }, { }, CbRemoveRef },
        { "invoke_function_reference", { WASM_I32, WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { }, CbInvokeFunctionReference },
        { "get_instance_id", { }, { WASM_I32 }, CbGetInstanceId },
        { "spawn_process", { WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }, CbSpawnProcess },
        { "get_last_spawn_exit_code", { }, { WASM_I32 }, CbGetLastSpawnExitCode },
        { "create_worker", { WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }, CbCreateWorker },
        { "poll_worker", { WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }, CbPollWorker },
        { "schedule_bookmark", { WASM_I32, WASM_I32 }, { }, CbScheduleBookmark },
        { "is_manifest_version_v2_between", { WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }, CbIsManifestVersionV2Between },
};

static constexpr size_t NUM_IMPORTS = sizeof(g_imports) / sizeof(g_imports[0]);

static wasm_functype_t** CachedImportFuncTypes()
{
        static wasm_functype_t** s_types = []
        {
                static wasm_functype_t* arr[NUM_IMPORTS];
                for (size_t i = 0; i < NUM_IMPORTS; ++i)
                        arr[i] = MakeFuncType(g_imports[i].params, g_imports[i].results);
                return arr;
        }();
        return s_types;
}

static wasm_trap_t* CbWorkerTrace(void*, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t)
{
        WasmMem mem;
        if (!mem.init(caller))
                return nullptr;
        uint32_t ptr = ArgU32(args[0]);
        uint32_t len = ArgU32(args[1]);
        if (!mem.check(ptr, len))
                return nullptr;
        std::string msg(reinterpret_cast<const char*>(mem.base + ptr), len);
        SanitizeTraceMsg(msg);
        fprintf(stderr, "[worker] %s", msg.c_str());
        if (!msg.empty() && msg.back() != '\n')
                fputc('\n', stderr);
        return nullptr;
}

static wasm_trap_t* CbWorkerStub(void*, wasmtime_caller_t*, const wasmtime_val_t*, size_t, wasmtime_val_t* results, size_t nresults)
{
        for (size_t i = 0; i < nresults; i++)
                results[i] = I32Val(0);
        return nullptr;
}

static wasm_trap_t* CbCreateWorker(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        WasmMem mem;
        if (!mem.init(caller))
        {
                results[0] = I32Val(-2);
                return nullptr;
        }
        uint32_t fnPtr = ArgU32(args[0]), fnLen = ArgU32(args[1]);
        uint32_t inPtr = ArgU32(args[2]), inLen = ArgU32(args[3]);
        if (!mem.check(fnPtr, fnLen) || (inLen > 0 && !mem.check(inPtr, inLen)))
        {
                results[0] = I32Val(-2);
                return nullptr;
        }
        if (!rt->host() || !HasWasmPermission(rt->host(), "sv_wasmWorkerThreads", rt->resourceName()))
        {
                LogError("Resource '%s' denied worker_threads permission. Add 'set sv_wasmWorkerThreads \"%s\"' to your server.cfg", rt->resourceName().c_str(), rt->resourceName().c_str());
                results[0] = I32Val(-1);
                return nullptr;
        }
        if (static_cast<int32_t>(rt->m_workers.size()) >= CppScriptRuntime::MAX_WORKERS_PER_RESOURCE)
        {
                LogError("Resource '%s' exceeded max worker limit (%d)", rt->resourceName().c_str(), CppScriptRuntime::MAX_WORKERS_PER_RESOURCE);
                results[0] = I32Val(-3);
                return nullptr;
        }
        std::string fnName(reinterpret_cast<const char*>(mem.base + fnPtr), fnLen);
        std::vector<char> inputData(mem.base + inPtr, mem.base + inPtr + inLen);
        int32_t workerId = fx::allocateId(rt->m_nextWorkerId, rt->m_workers);
        if (workerId < 0)
        {
                results[0] = I32Val(-3);
                return nullptr;
        }
        auto state = std::make_shared<CppScriptRuntime::WorkerState>();
        wasmtime_module_t* mod = wasmtime_module_clone(rt->wasmModule());
        try
        {
        state->thread = std::thread([state, mod, fnName, inputData = std::move(inputData)]()
        {
                auto* eng = CppScriptRuntime::engine();
                auto* store = wasmtime_store_new(eng, nullptr, nullptr);
                wasmtime_store_limiter(store, WASM_MEMORY_LIMIT, -1, -1, -1, -1);
                wasmtime_context_set_fuel(wasmtime_store_context(store), WASM_FUEL_AMOUNT);
                auto* linker = wasmtime_linker_new(eng);
                wasmtime_linker_allow_shadowing(linker, true);
                if (auto* wasi_err = wasmtime_linker_define_wasi(linker))
                        wasmtime_error_delete(wasi_err);
                wasi_config_t* wasi = wasi_config_new();
                wasmtime_context_set_wasi(wasmtime_store_context(store), wasi);
                auto** types = CachedImportFuncTypes();
                for (size_t i = 0; i < NUM_IMPORTS; ++i)
                {
                        const auto& imp = g_imports[i];
                        auto cb = (strcmp(imp.name, "trace") == 0) ? CbWorkerTrace : CbWorkerStub;
                        wasmtime_linker_define_func(linker, "cfx", 3, imp.name, strlen(imp.name), types[i], cb, nullptr, nullptr);
                }
                wasmtime_instance_t instance{ };
                wasm_trap_t* trap = nullptr;
                auto* err = wasmtime_linker_instantiate(linker, wasmtime_store_context(store), mod, &instance, &trap);
                if (err || trap)
                {
                        if (err)
                                wasmtime_error_delete(err);
                        if (trap)
                                wasm_trap_delete(trap);
                        std::lock_guard<std::mutex> lk(state->mutex);
                        state->status = CppScriptRuntime::WorkerState::Error;
                        wasmtime_linker_delete(linker);
                        wasmtime_store_delete(store);
                        wasmtime_module_delete(mod);
                        return;
                }
                auto* storeCtx = wasmtime_store_context(store);
                wasmtime_extern_t memExt{ };
                bool hasMem = wasmtime_instance_export_get(storeCtx, &instance, "memory", 6, &memExt) && memExt.kind == WASMTIME_EXTERN_MEMORY;
                wasmtime_extern_t allocExt{ };
                bool hasAlloc = wasmtime_instance_export_get(storeCtx, &instance, "__cfx_alloc", 11, &allocExt) && allocExt.kind == WASMTIME_EXTERN_FUNC;
                wasmtime_extern_t fnExt{ };
                if (!wasmtime_instance_export_get(storeCtx, &instance, fnName.c_str(), fnName.size(), &fnExt) || fnExt.kind != WASMTIME_EXTERN_FUNC)
                {
                        LogError("Worker export '%s' not found", fnName.c_str());
                        std::lock_guard<std::mutex> lk(state->mutex);
                        state->status = CppScriptRuntime::WorkerState::Error;
                        wasmtime_linker_delete(linker);
                        wasmtime_store_delete(store);
                        wasmtime_module_delete(mod);
                        return;
                }
                uint32_t inputPtr = 0;
                if (!inputData.empty() && hasAlloc)
                {
                        wasmtime_val_t allocArgs[1] = { I32Val(static_cast<int32_t>(inputData.size())) };
                        wasmtime_val_t allocResult[1]{ };
                        if (WasmCall(store, allocExt.of.func, allocArgs, 1, allocResult, 1, "worker", "__cfx_alloc"))
                        {
                                inputPtr = static_cast<uint32_t>(allocResult[0].of.i32);
                                if (hasMem)
                                {
                                        uint8_t* wbase = wasmtime_memory_data(storeCtx, &memExt.of.memory);
                                        size_t wsz = wasmtime_memory_data_size(storeCtx, &memExt.of.memory);
                                        if (InBounds(wsz, inputPtr, inputData.size()))
                                                memcpy(wbase + inputPtr, inputData.data(), inputData.size());
                                }
                        }
                }
                constexpr uint32_t resultBufSize = WORKER_RESULT_BUF_SIZE;
                uint32_t resultPtr = 0;
                if (hasAlloc)
                {
                        wasmtime_val_t allocArgs[1] = { I32Val(static_cast<int32_t>(resultBufSize)) };
                        wasmtime_val_t allocResult[1]{ };
                        if (WasmCall(store, allocExt.of.func, allocArgs, 1, allocResult, 1, "worker", "__cfx_alloc"))
                                resultPtr = static_cast<uint32_t>(allocResult[0].of.i32);
                }
                wasmtime_val_t callArgs[4] = {
                        I32Val(static_cast<int32_t>(inputPtr)),
                        I32Val(static_cast<int32_t>(inputData.size())),
                        I32Val(static_cast<int32_t>(resultPtr)),
                        I32Val(static_cast<int32_t>(resultBufSize))
                };
                wasmtime_val_t callResult[1]{ };
                trap = nullptr;
                err = wasmtime_func_call(storeCtx, &fnExt.of.func, callArgs, 4, callResult, 1, &trap);
                if (err || trap)
                {
                        if (err)
                                wasmtime_error_delete(err);
                        if (trap)
                                wasm_trap_delete(trap);
                        std::lock_guard<std::mutex> lk(state->mutex);
                        state->status = CppScriptRuntime::WorkerState::Error;
                        wasmtime_linker_delete(linker);
                        wasmtime_store_delete(store);
                        wasmtime_module_delete(mod);
                        return;
                }
                int32_t resultLen = callResult[0].of.i32;
                if (resultLen > static_cast<int32_t>(resultBufSize))
                        resultLen = static_cast<int32_t>(resultBufSize);
                {
                        std::lock_guard<std::mutex> lk(state->mutex);
                        if (resultLen > 0 && hasMem)
                        {
                                uint8_t* wbase = wasmtime_memory_data(storeCtx, &memExt.of.memory);
                                size_t wsz = wasmtime_memory_data_size(storeCtx, &memExt.of.memory);
                                if (InBounds(wsz, resultPtr, static_cast<size_t>(resultLen)))
                                        state->result.assign(wbase + resultPtr, wbase + resultPtr + resultLen);
                        }
                        state->status = CppScriptRuntime::WorkerState::Done;
                }
                wasmtime_linker_delete(linker);
                wasmtime_store_delete(store);
                wasmtime_module_delete(mod);
        });
        }
        catch (...)
        {
                wasmtime_module_delete(mod);
                results[0] = I32Val(-3);
                return nullptr;
        }
        rt->m_workers[workerId] = state;
        results[0] = I32Val(workerId);
        return nullptr;
}

static wasm_trap_t* CbPollWorker(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        int32_t workerId = args[0].of.i32;
        auto it = rt->m_workers.find(workerId);
        if (it == rt->m_workers.end())
        {
                results[0] = I32Val(-2);
                return nullptr;
        }
        auto state = it->second;
        CppScriptRuntime::WorkerState::Status status;
        {
                std::lock_guard<std::mutex> lk(state->mutex);
                status = state->status;
        }
        if (status == CppScriptRuntime::WorkerState::Running)
        {
                results[0] = I32Val(0);
                return nullptr;
        }
        if (status == CppScriptRuntime::WorkerState::Error)
        {
                if (state->thread.joinable())
                        state->thread.join();
                rt->m_workers.erase(it);
                results[0] = I32Val(-1);
                return nullptr;
        }
        WasmMem mem;
        uint32_t outBuf = ArgU32(args[1]);
        int32_t outMax = args[2].of.i32;
        if (!mem.init(caller) || outMax <= 0 || !mem.check(outBuf, static_cast<size_t>(outMax)))
        {
                results[0] = I32Val(-2);
                return nullptr;
        }
        std::vector<char> workerResult;
        {
                std::lock_guard<std::mutex> lk(state->mutex);
                workerResult = std::move(state->result);
        }
        int32_t written = 0;
        {
                size_t copy = std::min<size_t>(workerResult.size(), static_cast<size_t>(outMax));
                if (copy > 0)
                        memcpy(mem.base + outBuf, workerResult.data(), copy);
                written = static_cast<int32_t>(copy);
        }
        if (state->thread.joinable())
                state->thread.join();
        rt->m_workers.erase(it);
        results[0] = I32Val(written + 1);
        return nullptr;
}

static wasm_trap_t* CbScheduleBookmark(void* env, wasmtime_caller_t*, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        rt->scheduleWasmBookmark(args[0].of.i32, args[1].of.i32);
        return nullptr;
}

CppScriptRuntime::CppScriptRuntime()
{
        uint32_t raw = s_nextInstanceId.fetch_add(1, std::memory_order_relaxed);
        m_instanceId = static_cast<int32_t>(raw & 0x7FFFFFFFu);
        if (m_instanceId == 0)
                m_instanceId = static_cast<int32_t>(s_nextInstanceId.fetch_add(1, std::memory_order_relaxed) & 0x7FFFFFFFu);
}

CppScriptRuntime::~CppScriptRuntime()
{
        Destroy();
}

result_t OM_DECL CppScriptRuntime::Create(IScriptHost* host)
{
        m_host = fx::OMPtr<IScriptHost>(host);
        fx::OMPtr<IScriptHostWithResourceData> md;
        if (FX_SUCCEEDED(m_host.As(&md)) && md.GetRef())
        {
                m_metadataHost = md;
                char* name = nullptr;
                if (FX_SUCCEEDED(md->GetResourceName(&name)) && name)
                        m_resourceName = name;
        }
        fx::OMPtr<IScriptHostWithManifest> mh;
        if (FX_SUCCEEDED(m_host.As(&mh)) && mh.GetRef())
                m_manifestHost = mh;
        return FX_S_OK;
}

result_t OM_DECL CppScriptRuntime::Destroy()
{
        if (m_destroyed)
                return FX_S_OK;
        m_destroyed = true;
        {
                std::lock_guard<std::mutex> lk(m_refGuard->mu);
                m_refGuard->alive = false;
        }
        if (m_bookmarkHost.GetRef())
        {
                try
                {
                        m_bookmarkHost->RemoveBookmarks(static_cast<IScriptTickRuntimeWithBookmarks*>(this));
                }
                catch (...)
                {
                }
                m_bookmarkHost = { };
        }
        if (m_hasStopFn)
        {
                fx::PushEnvironment env(static_cast<IScriptRuntime*>(this));
                refuelWasm();
                callVoid(m_fnStop);
        }
        m_refs.clear();
        m_refToCallbackId.clear();
        destroyWasm();
        m_host = { };
        m_metadataHost = { };
        m_manifestHost = { };
        return FX_S_OK;
}

void OM_DECL CppScriptRuntime::SetParentObject(void* obj)
{
        m_parentObject = obj;
}

uint64_t CppScriptRuntime::nextBoundaryId()
{
        uint64_t id = m_nextBoundaryId;
        if (++m_nextBoundaryId == 0)
                m_nextBoundaryId = 1;
        return id;
}

int32_t CppScriptRuntime::allocRefIdx()
{
        return fx::allocateId(m_nextRefIdx, m_refs);
}

int32_t CppScriptRuntime::AddFuncRef(fx::RefCallback cb)
{
        int32_t idx = allocRefIdx();
        if (idx < 0)
                return -1;
        m_refs[idx] = std::move(cb);
        return idx;
}

wasm_engine_t* CppScriptRuntime::engine()
{
        static wasm_engine_t* g_engine = []()
        {
                wasm_config_t* config = wasm_config_new();
                wasmtime_config_consume_fuel_set(config, true);
                return wasm_engine_new_with_config(config);
        }();
        return g_engine;
}

void CppScriptRuntime::refuelWasm()
{
        if (!m_store)
                return;
        wasmtime_context_set_fuel(wasmtime_store_context(m_store), WASM_FUEL_AMOUNT);
}

void CppScriptRuntime::scheduleWasmBookmark(int32_t wasmId, int32_t deadlineMs)
{
        if (!m_bookmarkHost.GetRef())
                return;
        auto it = m_wasmToHostBookmark.find(wasmId);
        uint64_t hostId;
        if (it != m_wasmToHostBookmark.end())
        {
                hostId = it->second;
        }
        else
        {
                if (m_wasmToHostBookmark.size() >= MAX_BOOKMARKS_PER_RESOURCE)
                        return;
                hostId = m_nextWasmHostBookmarkId++;
                m_wasmToHostBookmark[wasmId] = hostId;
                m_hostToWasmBookmark[hostId] = wasmId;
        }
        if (deadlineMs < 0)
                deadlineMs = 0;
        int64_t deadline = (deadlineMs == 0) ? 0 : -static_cast<int64_t>(deadlineMs);
        m_bookmarkHost->ScheduleBookmark(static_cast<IScriptTickRuntimeWithBookmarks*>(this), hostId, deadline);
}

uint8_t* CppScriptRuntime::wasmBase()
{
        if (!m_hasMemory || !m_store)
                return nullptr;
        return wasmtime_memory_data(wasmtime_store_context(m_store), &m_memory);
}

size_t CppScriptRuntime::wasmMemSize()
{
        if (!m_hasMemory || !m_store)
                return 0;
        return wasmtime_memory_data_size(wasmtime_store_context(m_store), &m_memory);
}

uint32_t CppScriptRuntime::wasmAlloc(uint32_t size)
{
        if (!m_hasAllocFn || !m_store)
                return 0;
        wasmtime_val_t arg = I32Val(static_cast<int32_t>(size));
        wasmtime_val_t result{ };
        if (!WasmCall(m_store, m_fnAlloc, &arg, 1, &result, 1, m_resourceName.c_str(), "alloc trap"))
                return 0;
        return static_cast<uint32_t>(result.of.i32);
}

void CppScriptRuntime::wasmFree(uint32_t ptr, uint32_t size)
{
        if (!m_hasFreeFn || !m_store)
                return;
        wasmtime_val_t args[2] = { I32Val(static_cast<int32_t>(ptr)), I32Val(static_cast<int32_t>(size)) };
        if (!WasmCall(m_store, m_fnFree, args, 2, nullptr, 0, m_resourceName.c_str(), "free trap"))
                LogWarning("wasmFree trapped in '%s' (ptr=%u, size=%u)", m_resourceName.c_str(), ptr, size);
}

bool CppScriptRuntime::callVoid(const wasmtime_func_t& fn)
{
        if (!m_store)
                return false;
        return WasmCall(m_store, fn, nullptr, 0, nullptr, 0, m_resourceName.c_str(), "trap");
}

bool CppScriptRuntime::callEvent(uint32_t namePtr, uint32_t nameLen, uint32_t argsPtr, uint32_t argsLen, uint32_t srcPtr, uint32_t srcLen)
{
        if (!m_hasEventFn || !m_store)
                return false;
        wasmtime_val_t a[6] = {
                I32Val(static_cast<int32_t>(namePtr)),
                I32Val(static_cast<int32_t>(nameLen)),
                I32Val(static_cast<int32_t>(argsPtr)),
                I32Val(static_cast<int32_t>(argsLen)),
                I32Val(static_cast<int32_t>(srcPtr)),
                I32Val(static_cast<int32_t>(srcLen)),
        };
        return WasmCall(m_store, m_fnEvent, a, 6, nullptr, 0, m_resourceName.c_str(), "event trap");
}

bool CppScriptRuntime::callInvokeRef(uint32_t callbackId, const char* argsSerialized, uint32_t argsSize, std::vector<char>& result)
{
        if (!m_hasInvokeRefFn || !m_store || !m_hasAllocFn)
                return false;
        refuelWasm();
        uint32_t argsAllocSz = argsSize > 0 ? argsSize : 1;
        uint32_t argsPtr = wasmAlloc(argsAllocSz);
        if (!argsPtr)
                return false;
        uint8_t* base = wasmBase();
        size_t memSz = wasmMemSize();
        if (!InBounds(memSz, argsPtr, argsSize))
        {
                wasmFree(argsPtr, argsAllocSz);
                return false;
        }
        if (argsSize > 0)
                memcpy(base + argsPtr, argsSerialized, argsSize);
        constexpr uint32_t resultBufMax = WORKER_RESULT_BUF_SIZE;
        uint32_t resultPtr = wasmAlloc(resultBufMax);
        if (!resultPtr)
        {
                wasmFree(argsPtr, argsAllocSz);
                return false;
        }
        wasmtime_val_t a[5] = {
                I32Val(static_cast<int32_t>(callbackId)),
                I32Val(static_cast<int32_t>(argsPtr)),
                I32Val(static_cast<int32_t>(argsSize)),
                I32Val(static_cast<int32_t>(resultPtr)),
                I32Val(static_cast<int32_t>(resultBufMax)),
        };
        wasmtime_val_t ret{ };
        if (!WasmCall(m_store, m_fnInvokeRef, a, 5, &ret, 1, m_resourceName.c_str(), "invoke_ref trap"))
        {
                wasmFree(argsPtr, argsAllocSz);
                wasmFree(resultPtr, resultBufMax);
                return false;
        }
        wasmFree(argsPtr, argsAllocSz);
        int32_t actualLen = ret.of.i32;
        if (actualLen > 0)
        {
                uint32_t copyLen = std::min(static_cast<uint32_t>(actualLen), resultBufMax);
                base = wasmBase();
                memSz = wasmMemSize();
                if (InBounds(memSz, resultPtr, copyLen))
                {
                        result.resize(copyLen);
                        memcpy(result.data(), base + resultPtr, copyLen);
                }
                wasmFree(resultPtr, resultBufMax);
        }
        else
        {
                wasmFree(resultPtr, resultBufMax);
                result = std::vector<char>{ static_cast<char>(MSGPACK_EMPTY_ARRAY) };
        }
        return true;
}

void CppScriptRuntime::wasmMapRef(int32_t refIdx, int32_t callbackId)
{
        m_refToCallbackId[refIdx] = callbackId;
}

void CppScriptRuntime::wasmDuplicateRef(int32_t callbackId)
{
        if (!m_hasDuplicateRefFn || !m_store)
                return;
        wasmtime_val_t a[1] = { I32Val(callbackId) };
        wasmtime_val_t ret{ };
        WasmCall(m_store, m_fnDuplicateRef, a, 1, &ret, 1, m_resourceName.c_str(), "duplicate_ref trap");
}

void CppScriptRuntime::wasmRemoveRef(int32_t callbackId)
{
        if (!m_hasRemoveRefFn || !m_store)
                return;
        wasmtime_val_t a[1] = { I32Val(callbackId) };
        WasmCall(m_store, m_fnRemoveRef, a, 1, nullptr, 0, m_resourceName.c_str(), "remove_ref trap");
}

void CppScriptRuntime::defineImports()
{
        auto** types = CachedImportFuncTypes();
        for (size_t i = 0; i < NUM_IMPORTS; ++i)
        {
                const auto& imp = g_imports[i];
                auto* err = wasmtime_linker_define_func(m_linker, "cfx", 3, imp.name, strlen(imp.name), types[i], imp.hostCb, this, nullptr);
                if (err)
                        LogError("failed to define import '%s': %s", imp.name, ExtractWasmError(err, nullptr).c_str());
        }
}

bool CppScriptRuntime::resolveExports()
{
        auto* ctx = wasmtime_store_context(m_store);
        auto get = [&](const char* name, wasmtime_func_t& fn) -> bool
        {
                wasmtime_extern_t ext{ };
                if (!wasmtime_instance_export_get(ctx, &m_instance, name, strlen(name), &ext))
                        return false;
                if (ext.kind != WASMTIME_EXTERN_FUNC)
                        return false;
                fn = ext.of.func;
                return true;
        };
        {
                wasmtime_func_t initFn{ };
                if (!get("__cfx_init", initFn))
                {
                        LogError("'%s' missing __cfx_init export", m_resourceName.c_str());
                        return false;
                }
                if (!WasmCall(m_store, initFn, nullptr, 0, nullptr, 0, m_resourceName.c_str(), "__cfx_init trap"))
                        return false;
        }
        m_hasTickFn = get("__cfx_on_tick", m_fnTick);
        m_hasEventFn = get("__cfx_on_event", m_fnEvent);
        m_hasStopFn = get("__cfx_on_stop", m_fnStop);
        m_hasAllocFn = get("__cfx_alloc", m_fnAlloc);
        m_hasFreeFn = get("__cfx_free", m_fnFree);
        m_hasInvokeRefFn = get("__cfx_invoke_ref", m_fnInvokeRef);
        m_hasDuplicateRefFn = get("__cfx_duplicate_ref", m_fnDuplicateRef);
        m_hasRemoveRefFn = get("__cfx_remove_ref", m_fnRemoveRef);
        m_hasHasPendingWorkFn = get("__cfx_has_pending_work", m_fnHasPendingWork);
        m_hasTickBookmarksFn = get("__cfx_tick_bookmarks", m_fnTickBookmarks);
        wasmtime_extern_t memExt{ };
        if (wasmtime_instance_export_get(ctx, &m_instance, "memory", 6, &memExt) && memExt.kind == WASMTIME_EXTERN_MEMORY)
        {
                m_memory = memExt.of.memory;
                m_hasMemory = true;
        }
        return true;
}

void CppScriptRuntime::destroyWasm()
{
        for (auto& [id, w] : m_workers)
        {
                if (!w->thread.joinable())
                        continue;
                bool done = false;
                for (int i = 0; i < WORKER_SHUTDOWN_ATTEMPTS && !done; ++i)
                {
                        {
                                std::lock_guard<std::mutex> lk(w->mutex);
                                done = w->status != WorkerState::Running;
                        }
                        if (!done)
                                std::this_thread::sleep_for(std::chrono::milliseconds(WORKER_SHUTDOWN_INTERVAL_MS));
                }
                if (done)
                        w->thread.join();
                else
                {
                        LogWarning("Worker %d in '%s' did not finish within %ds, detaching", id, m_resourceName.c_str(), (WORKER_SHUTDOWN_ATTEMPTS * WORKER_SHUTDOWN_INTERVAL_MS) / 1000);
                        w->thread.detach();
                }
        }
        m_workers.clear();
        m_wasmToHostBookmark.clear();
        m_hostToWasmBookmark.clear();
        if (m_linker)
        {
                wasmtime_linker_delete(m_linker);
                m_linker = nullptr;
        }
        if (m_module)
        {
                wasmtime_module_delete(m_module);
                m_module = nullptr;
        }
        if (m_store)
        {
                wasmtime_store_delete(m_store);
                m_store = nullptr;
        }
        m_hasMemory = m_hasTickFn = m_hasEventFn = m_hasStopFn = false;
        m_hasAllocFn = m_hasFreeFn = m_hasInvokeRefFn = false;
        m_hasDuplicateRefFn = m_hasRemoveRefFn = m_hasHasPendingWorkFn = false;
        m_hasTickBookmarksFn = false;
}

result_t CppScriptRuntime::loadWasm(const std::vector<uint8_t>& wasmBytes, const std::string& sourcePath)
{
        m_store = wasmtime_store_new(engine(), this, nullptr);
        wasmtime_store_limiter(m_store, WASM_MEMORY_LIMIT, -1, -1, -1, -1);
        m_linker = wasmtime_linker_new(engine());
        wasmtime_linker_allow_shadowing(m_linker, true);
        {
                auto* err = wasmtime_linker_define_wasi(m_linker);
                if (err)
                        LogWarning("WASI linker error in '%s': %s", m_resourceName.c_str(), ExtractWasmError(err, nullptr).c_str());
                wasi_config_t* wasi = wasi_config_new();
                auto* werr = wasmtime_context_set_wasi(wasmtime_store_context(m_store), wasi);
                if (werr)
                        LogWarning("WASI context error in '%s': %s", m_resourceName.c_str(), ExtractWasmError(werr, nullptr).c_str());
        }
        defineImports();
        wasmtime_context_set_fuel(wasmtime_store_context(m_store), WASM_FUEL_AMOUNT);
        {
                wasmtime_error_t* err = wasmtime_module_new(engine(), wasmBytes.data(), wasmBytes.size(), &m_module);
                if (err)
                {
                        LogError("Compile error in '%s': %s", sourcePath.c_str(), ExtractWasmError(err, nullptr).c_str());
                        destroyWasm();
                        return FX_E_INVALIDARG;
                }
        }
        {
                wasm_trap_t* trap = nullptr;
                auto* err = wasmtime_linker_instantiate(m_linker, wasmtime_store_context(m_store), m_module, &m_instance, &trap);
                if (err || trap)
                {
                        LogError("Instantiate error in '%s': %s", sourcePath.c_str(), ExtractWasmError(err, trap).c_str());
                        destroyWasm();
                        return FX_E_INVALIDARG;
                }
        }
        {
                fx::OMPtr<IScriptHostWithBookmarks> bh;
                if (FX_SUCCEEDED(m_host.As(&bh)) && bh.GetRef())
                {
                        m_bookmarkHost = bh;
                        m_bookmarkHost->CreateBookmarks(static_cast<IScriptTickRuntimeWithBookmarks*>(this));
                }
        }

        refuelWasm();
        fx::PushEnvironment envGuard(static_cast<IScriptRuntime*>(this));
        if (!resolveExports())
        {
                destroyWasm();

                return FX_E_INVALIDARG;
        }
        {
                std::string msg = "Warning: WebAssembly '" + m_resourceName + "' has been loaded into the c++ rt. This runtime is still in beta and shouldn't be used in production, crashes and breaking changes are to be expected.";
                if (m_host.GetRef())
                        m_host->ScriptTrace(const_cast<char*>(msg.c_str()));
                LogWarning("%s", msg.c_str());
        }
        return FX_S_OK;
}

result_t OM_DECL CppScriptRuntime::Tick()
{
        if (!m_hasTickFn)
                return FX_S_OK;
        refuelWasm();
        if (m_hasHasPendingWorkFn)
        {
                wasmtime_val_t ret{ };
                if (!WasmCall(m_store, m_fnHasPendingWork, nullptr, 0, &ret, 1, m_resourceName.c_str(), "has_pending_work trap"))
                        return FX_S_OK;
                if (ret.of.i32 == 0)
                        return FX_S_OK;
        }
        fx::PushEnvironment env(static_cast<IScriptRuntime*>(this));
        BoundaryGuard boundary(m_host.GetRef(), static_cast<int64_t>(nextBoundaryId()));
        if (!callVoid(m_fnTick) && m_host.GetRef())
        {
                std::string msg = "^1SCRIPT ERROR: @" + m_resourceName + ": WASM trap in tick handler^7\n";
                m_host->ScriptTrace(const_cast<char*>(msg.c_str()));
        }
        return FX_S_OK;
}

result_t OM_DECL CppScriptRuntime::TickBookmarks(uint64_t* bookmarks, int32_t numBookmarks)
{
        if (!m_hasTickBookmarksFn || numBookmarks <= 0)
                return FX_S_OK;
        std::vector<int32_t> wasmIds;
        wasmIds.reserve(numBookmarks);
        for (int32_t i = 0; i < numBookmarks; ++i)
        {
                auto it = m_hostToWasmBookmark.find(bookmarks[i]);
                if (it != m_hostToWasmBookmark.end())
                        wasmIds.push_back(it->second);
        }
        if (wasmIds.empty())
                return FX_S_OK;
        uint32_t arrSize = static_cast<uint32_t>(wasmIds.size() * sizeof(int32_t));
        uint32_t arrPtr = wasmAlloc(arrSize);
        if (!arrPtr)
                return FX_S_OK;
        uint8_t* base = wasmBase();
        size_t memSz = wasmMemSize();
        if (!InBounds(memSz, arrPtr, arrSize))
        {
                wasmFree(arrPtr, arrSize);
                return FX_S_OK;
        }
        memcpy(base + arrPtr, wasmIds.data(), arrSize);
        fx::PushEnvironment env(static_cast<IScriptRuntime*>(this));
        BoundaryGuard boundary(m_host.GetRef(), static_cast<int64_t>(nextBoundaryId()));
        refuelWasm();
        wasmtime_val_t args[2] = { I32Val(static_cast<int32_t>(arrPtr)), I32Val(static_cast<int32_t>(wasmIds.size())) };
        WasmCall(m_store, m_fnTickBookmarks, args, 2, nullptr, 0, m_resourceName.c_str(), "tick_bookmarks trap");
        wasmFree(arrPtr, arrSize);
        return FX_S_OK;
}

result_t OM_DECL CppScriptRuntime::TriggerEvent(char* eventName, char* argsSerialized, uint32_t serializedSize, char* sourceId)
{
        if (!eventName)
                return FX_S_OK;
        if (!m_hasEventFn)
                return FX_S_OK;
        fx::PushEnvironment env(static_cast<IScriptRuntime*>(this));
        BoundaryGuard boundary(m_host.GetRef(), static_cast<int64_t>(nextBoundaryId()));
        m_eventCanceled = false;
        std::string_view name(eventName);
        std::string_view src(sourceId ? sourceId : "-1");
        uint64_t nameAllocSz64 = static_cast<uint64_t>(name.size()) + 1;
        uint64_t argsAllocSz64 = serializedSize > 0 ? static_cast<uint64_t>(serializedSize) : 1;
        uint64_t srcAllocSz64 = static_cast<uint64_t>(src.size()) + 1;
        uint64_t totalSz64 = nameAllocSz64 + argsAllocSz64 + srcAllocSz64;
        if (totalSz64 > UINT32_MAX)
                return FX_S_OK;
        uint32_t nameAllocSz = static_cast<uint32_t>(nameAllocSz64);
        uint32_t argsAllocSz = static_cast<uint32_t>(argsAllocSz64);
        uint32_t srcAllocSz = static_cast<uint32_t>(srcAllocSz64);
        uint32_t totalSz = static_cast<uint32_t>(totalSz64);
        uint32_t block = wasmAlloc(totalSz);
        if (!block)
                return FX_S_OK;
        uint32_t nameWasm = block;
        uint32_t argsWasm = block + nameAllocSz;
        uint32_t srcWasm = block + nameAllocSz + argsAllocSz;
        uint8_t* base = wasmBase();
        size_t memSz = wasmMemSize();
        if (!InBounds(memSz, block, totalSz))
        {
                wasmFree(block, totalSz);
                return FX_S_OK;
        }
        memcpy(base + nameWasm, name.data(), name.size());
        base[nameWasm + name.size()] = '\0';
        if (serializedSize > 0)
                memcpy(base + argsWasm, argsSerialized, serializedSize);
        memcpy(base + srcWasm, src.data(), src.size());
        base[srcWasm + src.size()] = '\0';
        refuelWasm();
        if (!callEvent(nameWasm, static_cast<uint32_t>(name.size()), argsWasm, serializedSize, srcWasm, static_cast<uint32_t>(src.size())) && m_host.GetRef())
        {
                std::string msg = "^1SCRIPT ERROR: @" + m_resourceName + ": WASM trap in event '" + eventName + "'^7\n";
                m_host->ScriptTrace(const_cast<char*>(msg.c_str()));
        }
        wasmFree(block, totalSz);
        return FX_S_OK;
}

result_t OM_DECL CppScriptRuntime::CallRef(int32_t refIdx, char* argsSerialized, uint32_t argsSize, IScriptBuffer** retval)
{
        auto it = m_refs.find(refIdx);
        if (it == m_refs.end())
                return FX_E_INVALIDARG;
        fx::PushEnvironment env(static_cast<IScriptRuntime*>(this));
        BoundaryGuard boundary(m_host.GetRef(), static_cast<int64_t>(nextBoundaryId()));
        std::vector<char> result;
        try
        {
                result = it->second(argsSerialized, argsSize);
        }
        catch (const std::exception& e)
        {
                std::string msg = "^1SCRIPT ERROR: @" + m_resourceName + ": Unhandled exception in ref " + std::to_string(refIdx) + ": " + e.what() + "^7\n";
                if (m_host.GetRef())
                        m_host->ScriptTrace(const_cast<char*>(msg.c_str()));
                result = std::vector<char>{ static_cast<char>(MSGPACK_EMPTY_ARRAY) };
        }
        catch (...)
        {
                std::string msg = "^1SCRIPT ERROR: @" + m_resourceName + ": Unhandled non-standard exception in ref " + std::to_string(refIdx) + "^7\n";
                if (m_host.GetRef())
                        m_host->ScriptTrace(const_cast<char*>(msg.c_str()));
                result = std::vector<char>{ static_cast<char>(MSGPACK_EMPTY_ARRAY) };
        }
        auto buf = fx::MakeNew<ScriptBuffer>(std::move(result));
        return buf->QueryInterface(IScriptBuffer::GetIID(), reinterpret_cast<void**>(retval));
}

result_t OM_DECL CppScriptRuntime::DuplicateRef(int32_t refIdx, int32_t* newRefIdx)
{
        auto it = m_refs.find(refIdx);
        if (it == m_refs.end())
                return FX_E_INVALIDARG;
        int32_t idx = allocRefIdx();
        if (idx < 0)
                return FX_E_INVALIDARG;
        *newRefIdx = idx;
        m_refs[idx] = it->second;
        auto cit = m_refToCallbackId.find(refIdx);
        if (cit != m_refToCallbackId.end())
        {
                m_refToCallbackId.emplace(*newRefIdx, cit->second);
                wasmDuplicateRef(cit->second);
        }
        return FX_S_OK;
}

result_t OM_DECL CppScriptRuntime::RemoveRef(int32_t refIdx)
{
        auto cit = m_refToCallbackId.find(refIdx);
        if (cit != m_refToCallbackId.end())
        {
                wasmRemoveRef(cit->second);
                m_refToCallbackId.erase(cit);
        }
        m_refs.erase(refIdx);
        return FX_S_OK;
}

int32_t OM_DECL CppScriptRuntime::HandlesFile(char* scriptFile, IScriptHostWithResourceData*)
{
        if (!scriptFile)
                return 0;
        std::string_view file(scriptFile);
        if (file.ends_with(".wasm"))
                return 1;
        return 0;
}

result_t OM_DECL CppScriptRuntime::LoadFile(char* scriptFile)
{
        if (!m_host.GetRef() || !scriptFile)
                return FX_E_INVALIDARG;
        std::string root = GetResourcePath(m_host.GetRef());
        if (!root.empty() && root.back() == '/')
                root.pop_back();
        if (root.empty())
        {
                LogError("Could not get resource path for '%s'", m_resourceName.c_str());
                return FX_E_INVALIDARG;
        }
        {
                fx::OMPtr<fxIStream> stream;
                if (FX_FAILED(m_host->OpenHostFile(scriptFile, stream.GetAddressOf())) || !stream.GetRef())
                {
                        LogError("Host denied access to '%s' in resource '%s'", scriptFile, m_resourceName.c_str());
                        return FX_E_INVALIDARG;
                }
        }
        std::string resolvedPath, resolvedRoot;
        if (!ValidateScriptPath(scriptFile, root, resolvedPath, resolvedRoot))
                return FX_E_INVALIDARG;
        std::string_view file(scriptFile);
        if (file.ends_with(".wasm"))
        {
                std::vector<uint8_t> wasmBytes;
                {
                        int fd = open(resolvedPath.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
                        if (fd < 0)
                        {
                                LogError("Cannot open '%s' (O_NOFOLLOW)", resolvedPath.c_str());
                                return FX_E_INVALIDARG;
                        }
                        FILE* f = fdopen(fd, "rb");
                        if (!f)
                        {
                                close(fd);
                        }
                        if (!f)
                        {
                                LogError("Cannot open '%s'", resolvedPath.c_str());
                                return FX_E_INVALIDARG;
                        }
                        fseek(f, 0, SEEK_END);
                        long sz = ftell(f);
                        fseek(f, 0, SEEK_SET);
                        if (sz <= 0)
                        {
                                fclose(f);
                                return FX_E_INVALIDARG;
                        }
                        wasmBytes.resize(static_cast<size_t>(sz));
                        if (fread(wasmBytes.data(), 1, wasmBytes.size(), f) != wasmBytes.size())
                        {
                                fclose(f);
                                LogError("Failed to read '%s'", resolvedPath.c_str());
                                return FX_E_INVALIDARG;
                        }
                        fclose(f);
                }
                return loadWasm(wasmBytes, resolvedPath);
        }
        LogError("Unsupported file type for '%s' in resource '%s'", scriptFile, m_resourceName.c_str());
        return FX_E_INVALIDARG;
}

result_t OM_DECL CppScriptRuntime::WalkStack(char*, uint32_t, char*, uint32_t, IScriptStackWalkVisitor* visitor)
{
        if (!visitor)
                return FX_S_OK;
        fx::MsgpackWriter w;
        w.mapHeader(4);
        w.str("name");
        w.str("[wasm]");
        w.str("file");
        w.str(m_resourceName);
        w.str("sourcefile");
        w.str("");
        w.str("line");
        w.encInt(0);
        visitor->SubmitStackFrame(reinterpret_cast<char*>(w.buf.data()), static_cast<uint32_t>(w.buf.size()));
        return FX_S_OK;
}

result_t OM_DECL CppScriptRuntime::RequestMemoryUsage()
{
        return FX_S_OK;
}

result_t OM_DECL CppScriptRuntime::GetMemoryUsage(int64_t* memUsage)
{
        if (!memUsage)
                return FX_E_INVALIDARG;
        *memUsage = static_cast<int64_t>(wasmMemSize());
        return FX_S_OK;
}

result_t OM_DECL CppScriptRuntime::EmitWarning(char* channel, char* message)
{
        if (message)
        {
                const char* ch = channel ? channel : "script";
                std::string msg = std::string("[warning:") + ch + "] " + message + "\n";
                if (m_host.GetRef())
                        m_host->ScriptTrace(const_cast<char*>(msg.c_str()));
                fprintf(stderr, "[script:%s] %s", m_resourceName.c_str(), msg.c_str());
        }
        return FX_S_OK;
}

void OM_DECL CppScriptRuntime::SetupFxProfiler(void*, int32_t)
{
}

void OM_DECL CppScriptRuntime::ShutdownFxProfiler()
{
}

FX_NEW_FACTORY(CppScriptRuntime);
FX_IMPLEMENTS(CLSID_CppScriptRuntime, IScriptRuntime);
FX_IMPLEMENTS(CLSID_CppScriptRuntime, IScriptFileHandlingRuntime);
FX_IMPLEMENTS(CLSID_CppScriptRuntime, IScriptTickRuntime);
FX_IMPLEMENTS(CLSID_CppScriptRuntime, IScriptEventRuntime);
FX_IMPLEMENTS(CLSID_CppScriptRuntime, IScriptRefRuntime);
FX_IMPLEMENTS(CLSID_CppScriptRuntime, IScriptTickRuntimeWithBookmarks);
FX_IMPLEMENTS(CLSID_CppScriptRuntime, IScriptStackWalkingRuntime);
FX_IMPLEMENTS(CLSID_CppScriptRuntime, IScriptMemInfoRuntime);
FX_IMPLEMENTS(CLSID_CppScriptRuntime, IScriptWarningRuntime);
FX_IMPLEMENTS(CLSID_CppScriptRuntime, IScriptProfiler);
