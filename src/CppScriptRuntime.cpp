#include "../include/CppScriptRuntime.h"
#include "../include/OMComponent.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <climits>
#include <vector>

#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

using namespace fx::cpp;

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

static bool ValidateScriptPath(const char* scriptFile, const std::string& root, std::string& resolvedPath, std::string& resolvedRoot, const char* resourceName)
{
        std::string_view scriptFileView(scriptFile);
        if (scriptFileView.find("/..") != std::string_view::npos || scriptFileView.find("../") != std::string_view::npos || scriptFileView == "..")
        {
                fprintf(stderr, "[citizen-scripting-cpp] Rejected script path with '..': '%s'\n", scriptFile);
                return false;
        }
        std::string fullPath = root + "/" + scriptFile;
        char* resolvedFull = realpath(fullPath.c_str(), nullptr);
        if (!resolvedFull)
        {
                fprintf(stderr, "[citizen-scripting-cpp] realpath failed for '%s'\n", fullPath.c_str());
                return false;
        }
        resolvedPath = resolvedFull;
        free(resolvedFull);
        char* resolvedRootBuf = realpath(root.c_str(), nullptr);
        if (!resolvedRootBuf)
        {
                fprintf(stderr, "[citizen-scripting-cpp] realpath failed for root '%s'\n", root.c_str());
                return false;
        }
        resolvedRoot = resolvedRootBuf;
        free(resolvedRootBuf);
        if (resolvedPath.compare(0, resolvedRoot.size(), resolvedRoot) != 0 || (resolvedPath.size() > resolvedRoot.size() && resolvedPath[resolvedRoot.size()] != '/'))
        {
                fprintf(stderr, "[citizen-scripting-cpp] Script path '%s' resolves outside resource root\n", scriptFile);
                return false;
        }
        return true;
}

static bool CopyFileTo(const std::string& src, const std::string& dst)
{
        FILE* in = fopen(src.c_str(), "rb");
        if (!in)
                return false;
        int fd = open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0700);
        if (fd < 0)
        {
                fclose(in);
                return false;
        }
        FILE* out = fdopen(fd, "wb");
        if (!out)
        {
                close(fd);
                fclose(in);
                return false;
        }
        char buf[8192];
        size_t n;
        bool ok = true;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        {
                if (fwrite(buf, 1, n, out) != n)
                {
                        ok = false;
                        break;
                }
        }
        fclose(in);
        fclose(out);
        if (!ok)
                std::remove(dst.c_str());
        return ok;
}

#ifdef FXCPP_WASM_SUPPORT

static wasmtime_val_t i32val(int32_t v)
{
        wasmtime_val_t r{ };
        r.kind = WASMTIME_I32;
        r.of.i32 = v;
        return r;
}

static wasm_functype_t* makeFuncType(std::initializer_list<wasm_valkind_t> params, std::initializer_list<wasm_valkind_t> results)
{
        std::vector<wasm_valtype_t*> pv, rv;
        for (auto k : params)
                pv.push_back(wasm_valtype_new(k));
        for (auto k : results)
                rv.push_back(wasm_valtype_new(k));
        wasm_valtype_vec_t p_vec{ }, r_vec{ };
        wasm_valtype_vec_new(&p_vec, pv.size(), pv.data());
        wasm_valtype_vec_new(&r_vec, rv.size(), rv.data());
        return wasm_functype_new(&p_vec, &r_vec);
}

static bool callerMemory(wasmtime_caller_t* caller, wasmtime_context_t* ctx, uint8_t** base, size_t* sz)
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

static bool inBounds(size_t memSz, uint32_t offset, size_t len)
{
        if (static_cast<size_t>(offset) > memSz)
                return false;
        return len <= memSz - static_cast<size_t>(offset);
}

static bool wasmCall(wasmtime_store_t* store, const wasmtime_func_t& fn, const wasmtime_val_t* args, size_t nargs, wasmtime_val_t* results, size_t nresults, const char* resourceName, const char* label)
{
        wasm_trap_t* trap = nullptr;
        auto* err = wasmtime_func_call(wasmtime_store_context(store), &fn, args, nargs, results, nresults, &trap);
        if (err || trap)
        {
                wasm_name_t msg{ };
                if (err)
                {
                        wasmtime_error_message(err, &msg);
                        wasmtime_error_delete(err);
                }
                else
                {
                        wasm_trap_message(trap, &msg);
                        wasm_trap_delete(trap);
                }
                fprintf(stderr, "[%s/wasm] %s: %.*s\n", resourceName, label, static_cast<int>(msg.size), msg.data);
                wasm_byte_vec_delete(&msg);
                return false;
        }
        return true;
}

static wasm_trap_t* cb_trace(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        auto* ctx = wasmtime_caller_context(caller);
        uint8_t* base;
        size_t sz;
        if (!callerMemory(caller, ctx, &base, &sz))
                return nullptr;
        uint32_t ptr = static_cast<uint32_t>(args[0].of.i32);
        uint32_t len = static_cast<uint32_t>(args[1].of.i32);
        if (!inBounds(sz, ptr, len))
                return nullptr;
        std::string msg(reinterpret_cast<const char*>(base + ptr), len);
        if (rt->host())
                rt->host()->ScriptTrace(const_cast<char*>(msg.c_str()));
        fprintf(stderr, "[script:%s] %s", rt->resourceName().c_str(), msg.c_str());
        if (!msg.empty() && msg.back() != '\n')
                fputc('\n', stderr);
        return nullptr;
}

static wasm_trap_t* cb_invoke_native(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        auto* ctx = wasmtime_caller_context(caller);
        uint8_t* base;
        size_t sz;
        if (!callerMemory(caller, ctx, &base, &sz))
                return nullptr;
        uint32_t ctxPtr = static_cast<uint32_t>(args[0].of.i32);
        if (!inBounds(sz, ctxPtr, sizeof(WasmNativeCtx)))
                return nullptr;
        WasmNativeCtx wctx{ };
        memcpy(&wctx, base + ctxPtr, sizeof(WasmNativeCtx));
        fxNativeContext hostCtx{ };
        hostCtx.nativeIdentifier = wctx.hash;
        hostCtx.numArguments = static_cast<int>(wctx.numArgs);
        hostCtx.numResults = static_cast<int>(wctx.numResults);
        for (uint32_t i = 0; i < wctx.numArgs && i < 32; ++i)
        {
                if ((wctx.ptrMask >> i) & 1u)
                {
                        uint32_t off = static_cast<uint32_t>(wctx.args[i]);
                        if (off >= sz)
                                return nullptr;
                        hostCtx.arguments[i] = reinterpret_cast<uintptr_t>(base + off);
                }
                else
                {
                        hostCtx.arguments[i] = static_cast<uintptr_t>(wctx.args[i]);
                }
        }
        if (rt->host())
                rt->host()->InvokeNative(hostCtx);
        rt->lastNativeCtx() = hostCtx;
        rt->lastResultPtrMask() = wctx.resultPtrMask;
        for (int i = 0; i < hostCtx.numResults && i < 32; ++i)
        {
                if ((wctx.resultPtrMask >> i) & 1u)
                        wctx.args[i] = (hostCtx.arguments[i] != 0) ? 1 : 0;
                else
                        wctx.args[i] = static_cast<uint64_t>(hostCtx.arguments[i]);
        }
        callerMemory(caller, ctx, &base, &sz);
        if (!inBounds(sz, ctxPtr, sizeof(WasmNativeCtx)))
                return nullptr;
        memcpy(base + ctxPtr, &wctx, sizeof(WasmNativeCtx));
        return nullptr;
}

static wasm_trap_t* cb_copy_string_result(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        auto* ctx = wasmtime_caller_context(caller);
        uint8_t* base;
        size_t sz;
        if (!callerMemory(caller, ctx, &base, &sz))
        {
                results[0] = i32val(0);
                return nullptr;
        }
        int32_t resultIdx = args[1].of.i32;
        uint32_t bufPtr = static_cast<uint32_t>(args[2].of.i32);
        int32_t bufMax = args[3].of.i32;
        if (resultIdx < 0 || resultIdx >= 32)
        {
                results[0] = i32val(0);
                return nullptr;
        }
        if (!((rt->lastResultPtrMask() >> resultIdx) & 1u))
        {
                results[0] = i32val(0);
                return nullptr;
        }
        const auto& hostCtx = rt->lastNativeCtx();
        const char* str = reinterpret_cast<const char*>(hostCtx.arguments[resultIdx]);
        if (!str)
        {
                results[0] = i32val(0);
                return nullptr;
        }
        size_t len = strlen(str);
        size_t copy = (bufMax > 1) ? std::min<size_t>(len, static_cast<size_t>(bufMax) - 1) : 0;
        if (copy && inBounds(sz, bufPtr, copy + 1))
        {
                memcpy(base + bufPtr, str, copy);
                base[bufPtr + copy] = '\0';
        }
        results[0] = i32val(static_cast<int32_t>(len));
        return nullptr;
}

static wasm_trap_t* cb_emit_event(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        auto* ctx = wasmtime_caller_context(caller);
        uint8_t* base;
        size_t sz;
        if (!callerMemory(caller, ctx, &base, &sz))
                return nullptr;
        uint32_t namePtr = static_cast<uint32_t>(args[0].of.i32);
        uint32_t nameLen = static_cast<uint32_t>(args[1].of.i32);
        uint32_t argsPtr = static_cast<uint32_t>(args[2].of.i32);
        uint32_t argsLen = static_cast<uint32_t>(args[3].of.i32);
        if (!inBounds(sz, namePtr, nameLen) || !inBounds(sz, argsPtr, argsLen))
                return nullptr;
        std::string evName(reinterpret_cast<const char*>(base + namePtr), nameLen);
        std::vector<uint8_t> argsCopy(base + argsPtr, base + argsPtr + argsLen);
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

static wasm_trap_t* cb_emit_net_event(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        auto* ctx = wasmtime_caller_context(caller);
        uint8_t* base;
        size_t sz;
        if (!callerMemory(caller, ctx, &base, &sz))
                return nullptr;
        uint32_t namePtr = static_cast<uint32_t>(args[0].of.i32);
        uint32_t nameLen = static_cast<uint32_t>(args[1].of.i32);
        int32_t target = args[2].of.i32;
        uint32_t argsPtr = static_cast<uint32_t>(args[3].of.i32);
        uint32_t argsLen = static_cast<uint32_t>(args[4].of.i32);
        if (!inBounds(sz, namePtr, nameLen) || !inBounds(sz, argsPtr, argsLen))
                return nullptr;
        std::string evName(reinterpret_cast<const char*>(base + namePtr), nameLen);
        std::vector<uint8_t> argsCopy(base + argsPtr, base + argsPtr + argsLen);
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

static wasm_trap_t* cb_cancel_event(void* env, wasmtime_caller_t*, const wasmtime_val_t*, size_t, wasmtime_val_t*, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        rt->eventCanceled() = true;
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

static wasm_trap_t* cb_was_event_canceled(void* env, wasmtime_caller_t*, const wasmtime_val_t*, size_t, wasmtime_val_t* results, size_t)
{
        results[0] = i32val(static_cast<CppScriptRuntime*>(env)->eventCanceled() ? 1 : 0);
        return nullptr;
}

static wasm_trap_t* cb_get_resource_metadata(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        auto* ctx = wasmtime_caller_context(caller);
        uint8_t* base;
        size_t sz;
        if (!callerMemory(caller, ctx, &base, &sz))
        {
                results[0] = i32val(0);
                return nullptr;
        }
        uint32_t keyPtr = static_cast<uint32_t>(args[0].of.i32);
        uint32_t keyLen = static_cast<uint32_t>(args[1].of.i32);
        int32_t index = args[2].of.i32;
        uint32_t bufPtr = static_cast<uint32_t>(args[3].of.i32);
        int32_t bufMax = args[4].of.i32;
        if (!inBounds(sz, keyPtr, keyLen))
        {
                results[0] = i32val(0);
                return nullptr;
        }
        std::string key(reinterpret_cast<const char*>(base + keyPtr), keyLen);
        fx::OMPtr<IScriptHostWithResourceData> md;
        fx::OMPtr<IScriptHost> h(rt->host());
        std::string value;
        if (FX_SUCCEEDED(h.As(&md)) && md.GetRef())
        {
                char* val = nullptr;
                if (FX_SUCCEEDED(md->GetResourceMetaData(const_cast<char*>(key.c_str()), index, &val)) && val)
                        value = val;
        }
        int32_t actualLen = static_cast<int32_t>(value.size());
        if (bufMax > 0 && inBounds(sz, bufPtr, static_cast<size_t>(std::min(bufMax, actualLen + 1))))
        {
                size_t copy = std::min<size_t>(value.size(), static_cast<size_t>(bufMax) - 1);
                memcpy(base + bufPtr, value.data(), copy);
                base[bufPtr + copy] = '\0';
        }
        results[0] = i32val(actualLen);
        return nullptr;
}

static wasm_trap_t* cb_get_num_resource_metadata(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        auto* ctx = wasmtime_caller_context(caller);
        uint8_t* base;
        size_t sz;
        if (!callerMemory(caller, ctx, &base, &sz))
        {
                results[0] = i32val(0);
                return nullptr;
        }
        uint32_t keyPtr = static_cast<uint32_t>(args[0].of.i32);
        uint32_t keyLen = static_cast<uint32_t>(args[1].of.i32);
        if (!inBounds(sz, keyPtr, keyLen))
        {
                results[0] = i32val(0);
                return nullptr;
        }
        std::string key(reinterpret_cast<const char*>(base + keyPtr), keyLen);
        fx::OMPtr<IScriptHostWithResourceData> md;
        fx::OMPtr<IScriptHost> h(rt->host());
        int32_t count = 0;
        if (FX_SUCCEEDED(h.As(&md)) && md.GetRef())
                md->GetNumResourceMetaData(const_cast<char*>(key.c_str()), &count);
        results[0] = i32val(count);
        return nullptr;
}

static wasm_trap_t* cb_is_manifest_version_v2_between(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        auto* ctx = wasmtime_caller_context(caller);
        uint8_t* base;
        size_t sz;
        if (!callerMemory(caller, ctx, &base, &sz))
        {
                results[0] = i32val(0);
                return nullptr;
        }
        uint32_t lowerPtr = static_cast<uint32_t>(args[0].of.i32);
        uint32_t lowerLen = static_cast<uint32_t>(args[1].of.i32);
        uint32_t upperPtr = static_cast<uint32_t>(args[2].of.i32);
        uint32_t upperLen = static_cast<uint32_t>(args[3].of.i32);
        if (!inBounds(sz, lowerPtr, lowerLen) || !inBounds(sz, upperPtr, upperLen))
        {
                results[0] = i32val(0);
                return nullptr;
        }
        std::string lower(reinterpret_cast<const char*>(base + lowerPtr), lowerLen);
        std::string upper(reinterpret_cast<const char*>(base + upperPtr), upperLen);
        fx::OMPtr<IScriptHostWithManifest> mh;
        fx::OMPtr<IScriptHost> h(rt->host());
        bool result = false;
        if (FX_SUCCEEDED(h.As(&mh)) && mh.GetRef())
                mh->IsManifestVersionV2Between(const_cast<char*>(lower.c_str()), const_cast<char*>(upper.c_str()), &result);
        results[0] = i32val(result ? 1 : 0);
        return nullptr;
}

static wasm_trap_t* cb_create_ref(void* env, wasmtime_caller_t*, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        uint32_t callbackId = static_cast<uint32_t>(args[0].of.i32);
        int32_t refIdx = rt->AddFuncRef([rt, callbackId](const char* argsSerialized, uint32_t argsSize) -> std::vector<char>
        {
                std::vector<char> result;
                if (!rt->callInvokeRef(callbackId, argsSerialized, argsSize, result))
                        result = { static_cast<char>(0x90) };
                return result;
        });
        rt->wasmMapRef(refIdx, callbackId);
        results[0] = i32val(refIdx);
        return nullptr;
}

static wasm_trap_t* cb_canonicalize_ref(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        auto* ctx = wasmtime_caller_context(caller);
        uint8_t* base;
        size_t sz;
        if (!callerMemory(caller, ctx, &base, &sz))
        {
                results[0] = i32val(0);
                return nullptr;
        }
        int32_t refIdx = args[0].of.i32;
        uint32_t bufPtr = static_cast<uint32_t>(args[1].of.i32);
        int32_t bufMax = args[2].of.i32;
        char* refString = nullptr;
        rt->host()->CanonicalizeRef(refIdx, rt->GetInstanceId(), &refString);
        if (!refString)
        {
                results[0] = i32val(0);
                return nullptr;
        }
        size_t len = strlen(refString);
        if (bufMax > 0 && inBounds(sz, bufPtr, std::min<size_t>(len + 1, static_cast<size_t>(bufMax))))
        {
                size_t copy = std::min<size_t>(len, static_cast<size_t>(bufMax) - 1);
                memcpy(base + bufPtr, refString, copy);
                base[bufPtr + copy] = '\0';
        }
        fwFree(refString);
        results[0] = i32val(static_cast<int32_t>(len));
        return nullptr;
}

static wasm_trap_t* cb_remove_ref(void* env, wasmtime_caller_t*, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        int32_t refIdx = args[0].of.i32;
        rt->RemoveRef(refIdx);
        return nullptr;
}

static wasm_trap_t* cb_invoke_function_reference(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        auto* ctx = wasmtime_caller_context(caller);
        uint8_t* base;
        size_t sz;
        if (!callerMemory(caller, ctx, &base, &sz))
                return nullptr;
        uint32_t refPtr = static_cast<uint32_t>(args[0].of.i32);
        uint32_t refLen = static_cast<uint32_t>(args[1].of.i32);
        uint32_t argsPtr = static_cast<uint32_t>(args[2].of.i32);
        uint32_t argsLen = static_cast<uint32_t>(args[3].of.i32);
        uint32_t outPtr = static_cast<uint32_t>(args[4].of.i32);
        if (!inBounds(sz, refPtr, refLen) || !inBounds(sz, argsPtr, argsLen) || !inBounds(sz, outPtr, 8))
                return nullptr;
        std::string refStr(reinterpret_cast<const char*>(base + refPtr), refLen);
        std::vector<char> argsCopy(reinterpret_cast<char*>(base + argsPtr), reinterpret_cast<char*>(base + argsPtr) + argsLen);
        fx::OMPtr<IScriptBuffer> retBuf;
        rt->host()->InvokeFunctionReference(const_cast<char*>(refStr.c_str()), argsCopy.data(), argsLen, retBuf.ReleaseAndGetAddressOf());
        uint32_t dataPtr = 0, dataLen = 0;
        if (retBuf.GetRef() && retBuf->GetLength() > 0)
        {
                dataLen = static_cast<uint32_t>(retBuf->GetLength());
                dataPtr = rt->wasmAlloc(dataLen);
                if (dataPtr)
                {
                        callerMemory(caller, ctx, &base, &sz);
                        if (inBounds(sz, dataPtr, dataLen))
                                memcpy(base + dataPtr, retBuf->GetBytes(), dataLen);
                }
                else
                        dataLen = 0;
        }
        callerMemory(caller, ctx, &base, &sz);
        if (inBounds(sz, outPtr, 8))
        {
                memcpy(base + outPtr, &dataPtr, 4);
                memcpy(base + outPtr + 4, &dataLen, 4);
        }
        return nullptr;
}

static wasm_trap_t* cb_get_instance_id(void* env, wasmtime_caller_t*, const wasmtime_val_t*, size_t, wasmtime_val_t* results, size_t)
{
        results[0] = i32val(static_cast<CppScriptRuntime*>(env)->GetInstanceId());
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

static wasm_trap_t* cb_spawn_process(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        auto* ctx = wasmtime_caller_context(caller);
        uint8_t* base;
        size_t sz;
        if (!callerMemory(caller, ctx, &base, &sz))
        {
                results[0] = i32val(-2);
                return nullptr;
        }
        uint32_t cmdPtr = static_cast<uint32_t>(args[0].of.i32);
        uint32_t cmdLen = static_cast<uint32_t>(args[1].of.i32);
        uint32_t outBuf = static_cast<uint32_t>(args[2].of.i32);
        int32_t outMax = args[3].of.i32;
        if (!inBounds(sz, cmdPtr, cmdLen))
        {
                results[0] = i32val(-2);
                return nullptr;
        }
        if (!HasWasmPermission(rt->host(), "sv_wasmChildProcess", rt->resourceName()))
        {
                fprintf(stderr, "[citizen-scripting-cpp/wasm] Resource '%s' denied child_process permission. Add 'set sv_wasmChildProcess \"%s\"' to your server.cfg\n", rt->resourceName().c_str(), rt->resourceName().c_str());
                results[0] = i32val(-1);
                return nullptr;
        }
        std::string cmd(reinterpret_cast<const char*>(base + cmdPtr), cmdLen);
        fx::ProcessResult pr = fx::spawnProcess(cmd);
        if (pr.status == -2)
        {
                results[0] = i32val(-2);
                return nullptr;
        }
        callerMemory(caller, ctx, &base, &sz);
        int32_t written = 0;
        if (outMax > 0 && inBounds(sz, outBuf, static_cast<size_t>(outMax)))
        {
                size_t copy = std::min<size_t>(pr.output.size(), static_cast<size_t>(outMax) - 1);
                memcpy(base + outBuf, pr.output.data(), copy);
                base[outBuf + copy] = '\0';
                written = static_cast<int32_t>(copy);
        }
        else if (!pr.output.empty())
        {
                fprintf(stderr, "[citizen-scripting-cpp/wasm] spawn_process: output buffer out of bounds\n");
        }
        results[0] = i32val(written);
        return nullptr;
}

static wasm_trap_t* cb_worker_trace(void*, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t)
{
        auto* ctx = wasmtime_caller_context(caller);
        uint8_t* base;
        size_t sz;
        if (!callerMemory(caller, ctx, &base, &sz))
                return nullptr;
        uint32_t ptr = static_cast<uint32_t>(args[0].of.i32);
        uint32_t len = static_cast<uint32_t>(args[1].of.i32);
        if (!inBounds(sz, ptr, len))
                return nullptr;
        fprintf(stderr, "[worker] %.*s", static_cast<int>(len), reinterpret_cast<const char*>(base + ptr));
        return nullptr;
}

static wasm_trap_t* cb_worker_stub(void*, wasmtime_caller_t*, const wasmtime_val_t*, size_t, wasmtime_val_t* results, size_t nresults)
{
        for (size_t i = 0; i < nresults; i++)
                results[i] = i32val(0);
        return nullptr;
}

static wasm_trap_t* cb_create_worker(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        auto* ctx = wasmtime_caller_context(caller);
        uint8_t* base;
        size_t sz;
        if (!callerMemory(caller, ctx, &base, &sz))
        {
                results[0] = i32val(-2);
                return nullptr;
        }
        uint32_t fnPtr = static_cast<uint32_t>(args[0].of.i32);
        uint32_t fnLen = static_cast<uint32_t>(args[1].of.i32);
        uint32_t inPtr = static_cast<uint32_t>(args[2].of.i32);
        uint32_t inLen = static_cast<uint32_t>(args[3].of.i32);
        if (!inBounds(sz, fnPtr, fnLen) || (inLen > 0 && !inBounds(sz, inPtr, inLen)))
        {
                results[0] = i32val(-2);
                return nullptr;
        }
        if (!HasWasmPermission(rt->host(), "sv_wasmWorkerThreads", rt->resourceName()))
        {
                fprintf(stderr, "[citizen-scripting-cpp/wasm] Resource '%s' denied worker_threads permission. Add 'set sv_wasmWorkerThreads \"%s\"' to your server.cfg\n", rt->resourceName().c_str(), rt->resourceName().c_str());
                results[0] = i32val(-1);
                return nullptr;
        }
        if (static_cast<int32_t>(rt->m_workers.size()) >= CppScriptRuntime::MAX_WORKERS_PER_RESOURCE)
        {
                fprintf(stderr, "[citizen-scripting-cpp/wasm] Resource '%s' exceeded max worker limit (%d)\n", rt->resourceName().c_str(), CppScriptRuntime::MAX_WORKERS_PER_RESOURCE);
                results[0] = i32val(-3);
                return nullptr;
        }
        std::string fnName(reinterpret_cast<const char*>(base + fnPtr), fnLen);
        std::vector<char> inputData(base + inPtr, base + inPtr + inLen);
        int32_t workerId = rt->m_nextWorkerId++;
        auto state = std::make_unique<CppScriptRuntime::WorkerState>();
        auto* statePtr = state.get();
        rt->m_workers[workerId] = std::move(state);
        wasmtime_module_t* mod = rt->wasmModule();
        statePtr->thread = std::thread([statePtr, mod, fnName, inputData = std::move(inputData)]()
        {
                auto* eng = CppScriptRuntime::engine();
                auto* store = wasmtime_store_new(eng, nullptr, nullptr);
                wasmtime_store_limiter(store, 256 * 1024 * 1024, -1, -1, -1, -1);
                wasmtime_context_set_fuel(wasmtime_store_context(store), UINT64_MAX);
                auto* linker = wasmtime_linker_new(eng);
                wasmtime_linker_allow_shadowing(linker, true);
                wasmtime_linker_define_wasi(linker);
                wasi_config_t* wasi = wasi_config_new();
                wasmtime_context_set_wasi(wasmtime_store_context(store), wasi);
                auto defFn = [&](const char* name, wasm_functype_t* ft, wasmtime_func_callback_t cb)
                {
                        wasmtime_linker_define_func(linker, "fxcpp", 5, name, strlen(name), ft, cb, nullptr, nullptr);
                        wasm_functype_delete(ft);
                };
                defFn("trace", makeFuncType({ WASM_I32, WASM_I32 }, { }), cb_worker_trace);
                defFn("invoke_native", makeFuncType({ WASM_I32 }, { }), cb_worker_stub);
                defFn("copy_string_result", makeFuncType({ WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }), cb_worker_stub);
                defFn("emit_event", makeFuncType({ WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { }), cb_worker_stub);
                defFn("emit_net_event", makeFuncType({ WASM_I32, WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { }), cb_worker_stub);
                defFn("cancel_event", makeFuncType({ }, { }), cb_worker_stub);
                defFn("was_event_canceled", makeFuncType({ }, { WASM_I32 }), cb_worker_stub);
                defFn("get_resource_metadata", makeFuncType({ WASM_I32, WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }), cb_worker_stub);
                defFn("get_num_resource_metadata", makeFuncType({ WASM_I32, WASM_I32 }, { WASM_I32 }), cb_worker_stub);
                defFn("create_ref", makeFuncType({ WASM_I32 }, { WASM_I32 }), cb_worker_stub);
                defFn("canonicalize_ref", makeFuncType({ WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }), cb_worker_stub);
                defFn("remove_ref", makeFuncType({ WASM_I32 }, { }), cb_worker_stub);
                defFn("invoke_function_reference", makeFuncType({ WASM_I32, WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { }), cb_worker_stub);
                defFn("get_instance_id", makeFuncType({ }, { WASM_I32 }), cb_worker_stub);
                defFn("spawn_process", makeFuncType({ WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }), cb_worker_stub);
                defFn("create_worker", makeFuncType({ WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }), cb_worker_stub);
                defFn("poll_worker", makeFuncType({ WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }), cb_worker_stub);
                defFn("schedule_bookmark", makeFuncType({ WASM_I32, WASM_I32 }, { }), cb_worker_stub);
                wasmtime_instance_t instance{ };
                wasm_trap_t* trap = nullptr;
                auto* err = wasmtime_linker_instantiate(linker, wasmtime_store_context(store), mod, &instance, &trap);
                if (err || trap)
                {
                        std::lock_guard<std::mutex> lk(statePtr->mutex);
                        statePtr->status = CppScriptRuntime::WorkerState::Error;
                        wasmtime_linker_delete(linker);
                        wasmtime_store_delete(store);
                        return;
                }
                auto* storeCtx = wasmtime_store_context(store);
                wasmtime_extern_t memExt{ };
                bool hasMem = wasmtime_instance_export_get(storeCtx, &instance, "memory", 6, &memExt) && memExt.kind == WASMTIME_EXTERN_MEMORY;
                wasmtime_extern_t allocExt{ };
                bool hasAlloc = wasmtime_instance_export_get(storeCtx, &instance, "fxcpp_alloc", 11, &allocExt) && allocExt.kind == WASMTIME_EXTERN_FUNC;
                wasmtime_extern_t fnExt{ };
                if (!wasmtime_instance_export_get(storeCtx, &instance, fnName.c_str(), fnName.size(), &fnExt) || fnExt.kind != WASMTIME_EXTERN_FUNC)
                {
                        fprintf(stderr, "[citizen-scripting-cpp/wasm] Worker export '%s' not found\n", fnName.c_str());
                        std::lock_guard<std::mutex> lk(statePtr->mutex);
                        statePtr->status = CppScriptRuntime::WorkerState::Error;
                        wasmtime_linker_delete(linker);
                        wasmtime_store_delete(store);
                        return;
                }
                uint32_t inputPtr = 0;
                if (!inputData.empty() && hasAlloc)
                {
                        wasmtime_val_t allocArgs[1] = { i32val(static_cast<int32_t>(inputData.size())) };
                        wasmtime_val_t allocResult[1]{ };
                        if (wasmCall(store, allocExt.of.func, allocArgs, 1, allocResult, 1, "worker", "fxcpp_alloc"))
                        {
                                inputPtr = static_cast<uint32_t>(allocResult[0].of.i32);
                                if (hasMem)
                                {
                                        uint8_t* wbase = wasmtime_memory_data(storeCtx, &memExt.of.memory);
                                        size_t wsz = wasmtime_memory_data_size(storeCtx, &memExt.of.memory);
                                        if (inBounds(wsz, inputPtr, inputData.size()))
                                                memcpy(wbase + inputPtr, inputData.data(), inputData.size());
                                }
                        }
                }
                constexpr uint32_t resultBufSize = 65536;
                uint32_t resultPtr = 0;
                if (hasAlloc)
                {
                        wasmtime_val_t allocArgs[1] = { i32val(static_cast<int32_t>(resultBufSize)) };
                        wasmtime_val_t allocResult[1]{ };
                        if (wasmCall(store, allocExt.of.func, allocArgs, 1, allocResult, 1, "worker", "fxcpp_alloc"))
                                resultPtr = static_cast<uint32_t>(allocResult[0].of.i32);
                }
                wasmtime_val_t callArgs[4] = {
                        i32val(static_cast<int32_t>(inputPtr)),
                        i32val(static_cast<int32_t>(inputData.size())),
                        i32val(static_cast<int32_t>(resultPtr)),
                        i32val(static_cast<int32_t>(resultBufSize))
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
                        std::lock_guard<std::mutex> lk(statePtr->mutex);
                        statePtr->status = CppScriptRuntime::WorkerState::Error;
                        wasmtime_linker_delete(linker);
                        wasmtime_store_delete(store);
                        return;
                }
                int32_t resultLen = callResult[0].of.i32;
                {
                        std::lock_guard<std::mutex> lk(statePtr->mutex);
                        if (resultLen > 0 && hasMem)
                        {
                                uint8_t* wbase = wasmtime_memory_data(storeCtx, &memExt.of.memory);
                                size_t wsz = wasmtime_memory_data_size(storeCtx, &memExt.of.memory);
                                if (inBounds(wsz, resultPtr, static_cast<size_t>(resultLen)))
                                        statePtr->result.assign(wbase + resultPtr, wbase + resultPtr + resultLen);
                        }
                        statePtr->status = CppScriptRuntime::WorkerState::Done;
                }
                wasmtime_linker_delete(linker);
                wasmtime_store_delete(store);
        });
        results[0] = i32val(workerId);
        return nullptr;
}

static wasm_trap_t* cb_poll_worker(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        int32_t workerId = args[0].of.i32;
        auto it = rt->m_workers.find(workerId);
        if (it == rt->m_workers.end())
        {
                results[0] = i32val(-2);
                return nullptr;
        }
        auto& state = it->second;
        std::lock_guard<std::mutex> lk(state->mutex);
        if (state->status == CppScriptRuntime::WorkerState::Running)
        {
                results[0] = i32val(0);
                return nullptr;
        }
        if (state->status == CppScriptRuntime::WorkerState::Error)
        {
                if (state->thread.joinable())
                        state->thread.join();
                rt->m_workers.erase(it);
                results[0] = i32val(-1);
                return nullptr;
        }
        auto* ctx = wasmtime_caller_context(caller);
        uint8_t* base;
        size_t sz;
        int32_t written = 0;
        uint32_t outBuf = static_cast<uint32_t>(args[1].of.i32);
        int32_t outMax = args[2].of.i32;
        if (callerMemory(caller, ctx, &base, &sz) && outMax > 0 && inBounds(sz, outBuf, static_cast<size_t>(outMax)))
        {
                size_t copy = std::min<size_t>(state->result.size(), static_cast<size_t>(outMax) - 1);
                memcpy(base + outBuf, state->result.data(), copy);
                base[outBuf + copy] = '\0';
                written = static_cast<int32_t>(copy);
        }
        if (state->thread.joinable())
                state->thread.join();
        rt->m_workers.erase(it);
        results[0] = i32val(written > 0 ? written : 1); // at least 1 to signaal done
        return nullptr;
}

static wasm_trap_t* cb_schedule_bookmark(void* env, wasmtime_caller_t*, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t)
{
        auto* rt = static_cast<CppScriptRuntime*>(env);
        rt->scheduleWasmBookmark(args[0].of.i32, args[1].of.i32);
        return nullptr;
}

#endif

CppScriptRuntime::CppScriptRuntime() : m_instanceId(static_cast<int32_t>(reinterpret_cast<intptr_t>(this) & 0x7FFFFFFF))
{
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
        return FX_S_OK;
}

result_t OM_DECL CppScriptRuntime::Destroy()
{
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
        if (m_mode == Mode::SharedLib)
        {
                if (m_ctx)
                {
                        {
                                fx::PushEnvironment env(static_cast<IScriptRuntime*>(this));
                                try
                                {
                                        m_ctx->dispatchStop();
                                }
                                catch (...)
                                {
                                }
                                try
                                {
                                        m_ctx->cleanupStateBagHandlers();
                                }
                                catch (...)
                                {
                                }
                                try
                                {
                                        m_ctx->cleanupBookmarks();
                                }
                                catch (...)
                                {
                                }
                        }
                        delete m_ctx;
                        m_ctx = nullptr;
                }
                m_refs.clear();
                if (m_libHandle)
                {
                        dlclose(m_libHandle);
                        m_libHandle = nullptr;
                }
                cleanupTemp();
        }
#ifdef FXCPP_WASM_SUPPORT
        else if (m_mode == Mode::Wasm)
        {
                if (m_hasStopFn)
                {
                        fx::PushEnvironment env(static_cast<IScriptRuntime*>(this));
                        callVoid(m_fnStop);
                }
                m_refs.clear();
                m_refToCallbackId.clear();
                destroyWasm();
        }
#endif
        m_mode = Mode::None;
        m_host = { };
        m_metadataHost = { };
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

int32_t CppScriptRuntime::AddFuncRef(fx::RefCallback cb)
{
        uint32_t idx = m_nextRefIdx;
        uint32_t start = idx;
        while (m_refs.count(static_cast<int32_t>(idx)))
        {
                if (++idx == 0)
                        idx = 1;
                if (idx == start)
                        return -1;
        }
        m_nextRefIdx = idx + 1;
        if (m_nextRefIdx == 0)
                m_nextRefIdx = 1;
        m_refs[static_cast<int32_t>(idx)] = std::move(cb);
        return static_cast<int32_t>(idx);
}

void CppScriptRuntime::cleanupTemp()
{
        if (!m_tempLibPath.empty())
        {
                std::remove(m_tempLibPath.c_str());
                m_tempLibPath.clear();
        }
        if (!m_tempDir.empty())
        {
                rmdir(m_tempDir.c_str());
                m_tempDir.clear();
        }
}

void CppScriptRuntime::cleanupLoadFailure()
{
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
        if (m_ctx)
        {
                try
                {
                        m_ctx->cleanupBookmarks();
                }
                catch (...)
                {
                }
                delete m_ctx;
                m_ctx = nullptr;
        }
        if (m_libHandle)
        {
                dlclose(m_libHandle);
                m_libHandle = nullptr;
        }
        cleanupTemp();
}

result_t CppScriptRuntime::loadSharedLib(const std::string& resolvedPath)
{
        std::string loadPath;
        {
                char tmpTemplate[] = "/tmp/fxcpp_XXXXXX";
                char* tmpDir = mkdtemp(tmpTemplate);
                if (tmpDir)
                {
                        m_tempDir = tmpDir;
                        loadPath = m_tempDir + "/lib.so";
                }
        }
        bool usedTempCopy = false;
        if (!m_tempDir.empty() && CopyFileTo(resolvedPath, loadPath))
        {
                usedTempCopy = true;
        }
        else
        {
                if (!m_tempDir.empty())
                        fprintf(stderr, "[citizen-scripting-cpp] Failed to copy '%s' to temp path '%s', loading original\n", resolvedPath.c_str(), loadPath.c_str());
                loadPath = resolvedPath;
        }
        m_libHandle = dlopen(loadPath.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!m_libHandle)
        {
                fprintf(stderr, "[citizen-scripting-cpp] dlopen failed for '%s': %s\n", loadPath.c_str(), dlerror());
                if (usedTempCopy)
                        std::remove(loadPath.c_str());
                cleanupTemp();
                return FX_E_INVALIDARG;
        }
        m_tempLibPath = usedTempCopy ? loadPath : std::string{ };
        auto* initFn = reinterpret_cast<void (*)(fx::ResourceContext*, void*)>(dlsym(m_libHandle, "fxcpp_init"));
        if (!initFn)
        {
                fprintf(stderr, "[citizen-scripting-cpp] '%s' has no fxcpp_init export\n", resolvedPath.c_str());
                dlclose(m_libHandle);
                m_libHandle = nullptr;
                cleanupTemp();
                return FX_E_INVALIDARG;
        }
        fx::OMPtr<IScriptRuntimeHandler> runtimeHandler;
        fx::MakeInterface(&runtimeHandler, CLSID_ScriptRuntimeHandler);
        {
                fx::OMPtr<IScriptHostWithBookmarks> bh;
                if (FX_SUCCEEDED(m_host.As(&bh)) && bh.GetRef())
                {
                        m_bookmarkHost = bh;
                        m_bookmarkHost->CreateBookmarks(static_cast<IScriptTickRuntimeWithBookmarks*>(this));
                }
        }
        fx::ScheduleBookmarkFn schedBookmark;
        if (m_bookmarkHost.GetRef())
        {
                schedBookmark = [this](uint64_t bm, int64_t deadline)
                {
                        if (m_bookmarkHost.GetRef())
                                m_bookmarkHost->ScheduleBookmark(static_cast<IScriptTickRuntimeWithBookmarks*>(this), bm, deadline);
                };
        }
        m_ctx = new fx::ResourceContext(m_host.GetRef(), this, m_resourceName, runtimeHandler.GetRef(), [this](fx::RefCallback cb) -> int32_t
        {
                return AddFuncRef(std::move(cb));
        },
        [this](int32_t idx)
        {
                m_refs.erase(idx);
        },
        std::move(schedBookmark));
        m_mode = Mode::SharedLib;
        fprintf(stderr, "[citizen-scripting-cpp] Loaded C++ resource '%s'\n", m_resourceName.c_str());
        fx::PushEnvironment env(static_cast<IScriptRuntime*>(this));
        bool initOk = false;
        try
        {
                initFn(m_ctx, m_libHandle);
                initOk = true;
        }
        catch (const std::exception& e)
        {
                fprintf(stderr, "[citizen-scripting-cpp] Exception during init of '%s': %s\n", m_resourceName.c_str(), e.what());
                if (m_ctx)
                        m_ctx->trace("Exception during resource init: %s\n", e.what());
        }
        catch (...)
        {
                fprintf(stderr, "[citizen-scripting-cpp] Non-standard exception during init of '%s'\n", m_resourceName.c_str());
        }
        if (!initOk)
        {
                cleanupLoadFailure();
                m_mode = Mode::None;
                return FX_E_INVALIDARG;
        }
        return FX_S_OK;
}

#ifdef FXCPP_WASM_SUPPORT

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
        wasmtime_context_set_fuel(wasmtime_store_context(m_store), 1000000000ULL);
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
                hostId = m_nextWasmHostBookmarkId++;
                m_wasmToHostBookmark[wasmId] = hostId;
                m_hostToWasmBookmark[hostId] = wasmId;
        }
        int64_t deadline = (deadlineMs == 0) ? 0 : -static_cast<int64_t>(deadlineMs);
        m_bookmarkHost->ScheduleBookmark(static_cast<IScriptTickRuntimeWithBookmarks*>(this), hostId, deadline);
}

std::string CppScriptRuntime::wasmErrMsg(wasmtime_error_t* err, wasm_trap_t* trap)
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
        wasmtime_val_t arg = i32val(static_cast<int32_t>(size));
        wasmtime_val_t result{ };
        if (!wasmCall(m_store, m_fnAlloc, &arg, 1, &result, 1, m_resourceName.c_str(), "alloc trap"))
                return 0;
        return static_cast<uint32_t>(result.of.i32);
}

void CppScriptRuntime::wasmFree(uint32_t ptr, uint32_t size)
{
        if (!m_hasFreeFn || !m_store)
                return;
        wasmtime_val_t args[2] = { i32val(static_cast<int32_t>(ptr)), i32val(static_cast<int32_t>(size)) };
        wasmCall(m_store, m_fnFree, args, 2, nullptr, 0, m_resourceName.c_str(), "free trap");
}

bool CppScriptRuntime::callVoid(const wasmtime_func_t& fn)
{
        if (!m_store)
                return false;
        return wasmCall(m_store, fn, nullptr, 0, nullptr, 0, m_resourceName.c_str(), "trap");
}

bool CppScriptRuntime::callEvent(uint32_t namePtr, uint32_t nameLen, uint32_t argsPtr, uint32_t argsLen, uint32_t srcPtr, uint32_t srcLen)
{
        if (!m_hasEventFn || !m_store)
                return false;
        wasmtime_val_t a[6] = {
                i32val(static_cast<int32_t>(namePtr)),
                i32val(static_cast<int32_t>(nameLen)),
                i32val(static_cast<int32_t>(argsPtr)),
                i32val(static_cast<int32_t>(argsLen)),
                i32val(static_cast<int32_t>(srcPtr)),
                i32val(static_cast<int32_t>(srcLen)),
        };
        return wasmCall(m_store, m_fnEvent, a, 6, nullptr, 0, m_resourceName.c_str(), "event trap");
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
        if (!inBounds(memSz, argsPtr, argsSize))
        {
                wasmFree(argsPtr, argsAllocSz);
                return false;
        }
        if (argsSize > 0)
                memcpy(base + argsPtr, argsSerialized, argsSize);
        constexpr uint32_t resultBufMax = 4096;
        uint32_t resultPtr = wasmAlloc(resultBufMax);
        if (!resultPtr)
        {
                wasmFree(argsPtr, argsAllocSz);
                return false;
        }
        wasmtime_val_t a[5] = {
                i32val(static_cast<int32_t>(callbackId)),
                i32val(static_cast<int32_t>(argsPtr)),
                i32val(static_cast<int32_t>(argsSize)),
                i32val(static_cast<int32_t>(resultPtr)),
                i32val(static_cast<int32_t>(resultBufMax)),
        };
        wasmtime_val_t ret{ };
        if (!wasmCall(m_store, m_fnInvokeRef, a, 5, &ret, 1, m_resourceName.c_str(), "invoke_ref trap"))
        {
                wasmFree(argsPtr, argsAllocSz);
                wasmFree(resultPtr, resultBufMax);
                return false;
        }
        wasmFree(argsPtr, argsAllocSz);
        int32_t actualLen = ret.of.i32;
        if (actualLen > 0)
        {
                uint32_t copyLen = static_cast<uint32_t>(actualLen);
                if (copyLen > resultBufMax)
                {
                        wasmFree(resultPtr, resultBufMax);
                        resultPtr = wasmAlloc(copyLen);
                        if (!resultPtr)
                        {
                                result = { static_cast<char>(0x90) };
                                return true;
                        }
                        wasmtime_val_t a2[5] = {
                                i32val(static_cast<int32_t>(callbackId)),
                                i32val(0),
                                i32val(0),
                                i32val(static_cast<int32_t>(resultPtr)),
                                i32val(static_cast<int32_t>(copyLen)),
                        };
                        wasmtime_val_t ret2{ };
                        if (!wasmCall(m_store, m_fnInvokeRef, a2, 5, &ret2, 1, m_resourceName.c_str(), "invoke_ref retry"))
                        {
                                wasmFree(resultPtr, copyLen);
                                result = { static_cast<char>(0x90) };
                                return true;
                        }
                        copyLen = std::min(copyLen, static_cast<uint32_t>(ret2.of.i32));
                }
                base = wasmBase();
                memSz = wasmMemSize();
                if (inBounds(memSz, resultPtr, copyLen))
                {
                        result.resize(copyLen);
                        memcpy(result.data(), base + resultPtr, copyLen);
                }
                wasmFree(resultPtr, copyLen > resultBufMax ? copyLen : resultBufMax);
        }
        else
        {
                wasmFree(resultPtr, resultBufMax);
                result = { static_cast<char>(0x90) };
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
        wasmtime_val_t a[1] = { i32val(callbackId) };
        wasmtime_val_t ret{ };
        wasmCall(m_store, m_fnDuplicateRef, a, 1, &ret, 1, m_resourceName.c_str(), "duplicate_ref trap");
}

void CppScriptRuntime::wasmRemoveRef(int32_t callbackId)
{
        if (!m_hasRemoveRefFn || !m_store)
                return;
        wasmtime_val_t a[1] = { i32val(callbackId) };
        wasmCall(m_store, m_fnRemoveRef, a, 1, nullptr, 0, m_resourceName.c_str(), "remove_ref trap");
}

void CppScriptRuntime::defineImports()
{
        auto def = [&](const char* name, wasm_functype_t* ft, wasmtime_func_callback_t cb)
        {
                auto* err = wasmtime_linker_define_func(m_linker, "fxcpp", 5, name, strlen(name), ft, cb, this, nullptr);
                wasm_functype_delete(ft);
                if (err)
                        fprintf(stderr, "[citizen-scripting-cpp/wasm] failed to define import '%s': %s\n", name, wasmErrMsg(err, nullptr).c_str());
        };
        def("trace", makeFuncType({ WASM_I32, WASM_I32 }, { }), cb_trace);
        def("invoke_native", makeFuncType({ WASM_I32 }, { }), cb_invoke_native);
        def("copy_string_result", makeFuncType({ WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }), cb_copy_string_result);
        def("emit_event", makeFuncType({ WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { }), cb_emit_event);
        def("emit_net_event", makeFuncType({ WASM_I32, WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { }), cb_emit_net_event);
        def("cancel_event", makeFuncType({ }, { }), cb_cancel_event);
        def("was_event_canceled", makeFuncType({ }, { WASM_I32 }), cb_was_event_canceled);
        def("get_resource_metadata", makeFuncType({ WASM_I32, WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }), cb_get_resource_metadata);
        def("get_num_resource_metadata", makeFuncType({ WASM_I32, WASM_I32 }, { WASM_I32 }), cb_get_num_resource_metadata);
        def("create_ref", makeFuncType({ WASM_I32 }, { WASM_I32 }), cb_create_ref);
        def("canonicalize_ref", makeFuncType({ WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }), cb_canonicalize_ref);
        def("remove_ref", makeFuncType({ WASM_I32 }, { }), cb_remove_ref);
        def("invoke_function_reference", makeFuncType({ WASM_I32, WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { }), cb_invoke_function_reference);
        def("get_instance_id", makeFuncType({ }, { WASM_I32 }), cb_get_instance_id);
        def("spawn_process", makeFuncType({ WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }), cb_spawn_process);
        def("create_worker", makeFuncType({ WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }), cb_create_worker);
        def("poll_worker", makeFuncType({ WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }), cb_poll_worker);
        def("schedule_bookmark", makeFuncType({ WASM_I32, WASM_I32 }, { }), cb_schedule_bookmark);
        def("is_manifest_version_v2_between", makeFuncType({ WASM_I32, WASM_I32, WASM_I32, WASM_I32 }, { WASM_I32 }), cb_is_manifest_version_v2_between);
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
                if (!get("fxcpp_init", initFn))
                {
                        fprintf(stderr, "[citizen-scripting-cpp/wasm] '%s' missing fxcpp_init export\n", m_resourceName.c_str());
                        return false;
                }
                if (!wasmCall(m_store, initFn, nullptr, 0, nullptr, 0, m_resourceName.c_str(), "fxcpp_init trap"))
                        return false;
        }
        m_hasTickFn = get("fxcpp_tick", m_fnTick);
        m_hasEventFn = get("fxcpp_on_event", m_fnEvent);
        m_hasStopFn = get("fxcpp_on_stop", m_fnStop);
        m_hasAllocFn = get("fxcpp_alloc", m_fnAlloc);
        m_hasFreeFn = get("fxcpp_free", m_fnFree);
        m_hasInvokeRefFn = get("fxcpp_invoke_ref", m_fnInvokeRef);
        m_hasDuplicateRefFn = get("fxcpp_duplicate_ref", m_fnDuplicateRef);
        m_hasRemoveRefFn = get("fxcpp_remove_ref", m_fnRemoveRef);
        m_hasHasPendingWorkFn = get("fxcpp_has_pending_work", m_fnHasPendingWork);
        m_hasTickBookmarksFn = get("fxcpp_tick_bookmarks", m_fnTickBookmarks);
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
                if (w->thread.joinable())
                        w->thread.join();
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

result_t CppScriptRuntime::loadWasm(const std::string& resolvedPath)
{
        std::vector<uint8_t> wasmBytes;
        {
                FILE* f = fopen(resolvedPath.c_str(), "rb");
                if (!f)
                {
                        fprintf(stderr, "[citizen-scripting-cpp/wasm] Cannot open '%s'\n", resolvedPath.c_str());
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
                        fprintf(stderr, "[citizen-scripting-cpp/wasm] Failed to read '%s'\n", resolvedPath.c_str());
                        return FX_E_INVALIDARG;
                }
                fclose(f);
        }
        m_store = wasmtime_store_new(engine(), this, nullptr);
        wasmtime_store_limiter(m_store, 256 * 1024 * 1024, -1, -1, -1, -1);
        m_linker = wasmtime_linker_new(engine());
        wasmtime_linker_allow_shadowing(m_linker, true);
        {
                auto* err = wasmtime_linker_define_wasi(m_linker);
                if (err)
                        wasmErrMsg(err, nullptr);
                wasi_config_t* wasi = wasi_config_new();
                auto* werr = wasmtime_context_set_wasi(wasmtime_store_context(m_store), wasi);
                if (werr)
                        wasmErrMsg(werr, nullptr);
        }
        defineImports();
        {
                wasmtime_error_t* err = wasmtime_module_new(engine(), wasmBytes.data(), wasmBytes.size(), &m_module);
                if (err)
                {
                        fprintf(stderr, "[citizen-scripting-cpp/wasm] Compile error in '%s': %s\n", resolvedPath.c_str(), wasmErrMsg(err, nullptr).c_str());
                        destroyWasm();
                        return FX_E_INVALIDARG;
                }
        }
        {
                wasm_trap_t* trap = nullptr;
                auto* err = wasmtime_linker_instantiate(m_linker, wasmtime_store_context(m_store), m_module, &m_instance, &trap);
                if (err || trap)
                {
                        fprintf(stderr, "[citizen-scripting-cpp/wasm] Instantiate error in '%s': %s\n", resolvedPath.c_str(), wasmErrMsg(err, trap).c_str());
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
        m_mode = Mode::Wasm;
        refuelWasm();
        fx::PushEnvironment envGuard(static_cast<IScriptRuntime*>(this));
        if (!resolveExports())
        {
                destroyWasm();
                m_mode = Mode::None;
                return FX_E_INVALIDARG;
        }
        fprintf(stderr, "[citizen-scripting-cpp] Loaded WASM resource '%s'\n", m_resourceName.c_str());
        return FX_S_OK;
}

#endif

result_t OM_DECL CppScriptRuntime::Tick()
{
        if (m_mode == Mode::SharedLib)
        {
                if (!m_ctx || !m_ctx->hasPendingWork())
                        return FX_S_OK;
                fx::PushEnvironment env(static_cast<IScriptRuntime*>(this));
                BoundaryGuard boundary(m_host.GetRef(), static_cast<int64_t>(nextBoundaryId()));
                try
                {
                        m_ctx->dispatchTick();
                }
                catch (const std::exception& e)
                {
                        m_ctx->trace("SCRIPT ERROR: @%s: Unhandled exception in tick: %s\n", m_resourceName.c_str(), e.what());
                }
                catch (...)
                {
                        m_ctx->trace("SCRIPT ERROR: @%s: Unhandled non-standard exception in tick\n", m_resourceName.c_str());
                }
        }
#ifdef FXCPP_WASM_SUPPORT
        else if (m_mode == Mode::Wasm)
        {
                if (!m_hasTickFn)
                        return FX_S_OK;
                if (m_hasHasPendingWorkFn)
                {
                        wasmtime_val_t ret{ };
                        if (wasmCall(m_store, m_fnHasPendingWork, nullptr, 0, &ret, 1, m_resourceName.c_str(), "has_pending_work trap") && ret.of.i32 == 0)
                                return FX_S_OK;
                }
                fx::PushEnvironment env(static_cast<IScriptRuntime*>(this));
                BoundaryGuard boundary(m_host.GetRef(), static_cast<int64_t>(nextBoundaryId()));
                refuelWasm();
                if (!callVoid(m_fnTick) && m_host.GetRef())
                {
                        char buf[256];
                        snprintf(buf, sizeof(buf), "SCRIPT ERROR: @%s: WASM trap in tick handler\n", m_resourceName.c_str());
                        m_host->ScriptTrace(buf);
                }
        }
#endif
        return FX_S_OK;
}

result_t OM_DECL CppScriptRuntime::TickBookmarks(uint64_t* bookmarks, int32_t numBookmarks)
{
        if (m_mode == Mode::SharedLib)
        {
                if (!m_ctx || numBookmarks <= 0)
                        return FX_S_OK;
                fx::PushEnvironment env(static_cast<IScriptRuntime*>(this));
                BoundaryGuard boundary(m_host.GetRef(), static_cast<int64_t>(nextBoundaryId()));
                m_ctx->resumeBookmarks(bookmarks, numBookmarks);
        }
#ifdef FXCPP_WASM_SUPPORT
        else if (m_mode == Mode::Wasm)
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
                if (!inBounds(memSz, arrPtr, arrSize))
                {
                        wasmFree(arrPtr, arrSize);
                        return FX_S_OK;
                }
                memcpy(base + arrPtr, wasmIds.data(), arrSize);
                fx::PushEnvironment env(static_cast<IScriptRuntime*>(this));
                BoundaryGuard boundary(m_host.GetRef(), static_cast<int64_t>(nextBoundaryId()));
                refuelWasm();
                wasmtime_val_t args[2] = { i32val(static_cast<int32_t>(arrPtr)), i32val(static_cast<int32_t>(wasmIds.size())) };
                wasmCall(m_store, m_fnTickBookmarks, args, 2, nullptr, 0, m_resourceName.c_str(), "tick_bookmarks trap");
                wasmFree(arrPtr, arrSize);
        }
#endif
        return FX_S_OK;
}

result_t OM_DECL CppScriptRuntime::TriggerEvent(char* eventName, char* argsSerialized, uint32_t serializedSize, char* sourceId)
{
        if (!eventName)
                return FX_S_OK;
        if (m_mode == Mode::SharedLib)
        {
                if (!m_ctx)
                        return FX_S_OK;
                fx::PushEnvironment env(static_cast<IScriptRuntime*>(this));
                BoundaryGuard boundary(m_host.GetRef(), static_cast<int64_t>(nextBoundaryId()));
                try
                {
                        fx::json::Value args = fx::msgpack::decode(argsSerialized, serializedSize);
                        fx::json::ensureArray(args);
                        std::string src = sourceId ? sourceId : "-1";
                        m_ctx->dispatchEvent(eventName, args, src);
                }
                catch (const std::exception& e)
                {
                        m_ctx->trace("SCRIPT ERROR: @%s: Unhandled exception in event '%s': %s\n", m_resourceName.c_str(), eventName, e.what());
                }
                catch (...)
                {
                        m_ctx->trace("SCRIPT ERROR: @%s: Unhandled non-standard exception in event '%s'\n", m_resourceName.c_str(), eventName);
                }
        }
#ifdef FXCPP_WASM_SUPPORT
        else if (m_mode == Mode::Wasm)
        {
                if (!m_hasEventFn)
                        return FX_S_OK;
                fx::PushEnvironment env(static_cast<IScriptRuntime*>(this));
                BoundaryGuard boundary(m_host.GetRef(), static_cast<int64_t>(nextBoundaryId()));
                m_eventCanceled = false;
                std::string_view name(eventName);
                std::string_view src(sourceId ? sourceId : "-1");
                uint32_t nameAllocSz = static_cast<uint32_t>(name.size()) + 1;
                uint32_t argsAllocSz = serializedSize > 0 ? serializedSize : 1;
                uint32_t srcAllocSz = static_cast<uint32_t>(src.size()) + 1;
                uint32_t totalSz = nameAllocSz + argsAllocSz + srcAllocSz;
                uint32_t block = wasmAlloc(totalSz);
                if (!block)
                        return FX_S_OK;
                uint32_t nameWasm = block;
                uint32_t argsWasm = block + nameAllocSz;
                uint32_t srcWasm = block + nameAllocSz + argsAllocSz;
                uint8_t* base = wasmBase();
                size_t memSz = wasmMemSize();
                if (!inBounds(memSz, block, totalSz))
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
                        char buf[512];
                        snprintf(buf, sizeof(buf), "SCRIPT ERROR: @%s: WASM trap in event '%s'\n", m_resourceName.c_str(), eventName);
                        m_host->ScriptTrace(buf);
                }
                wasmFree(block, totalSz);
        }
#endif
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
                char buf[512];
                snprintf(buf, sizeof(buf), "SCRIPT ERROR: @%s: Unhandled exception in ref %d: %s\n", m_resourceName.c_str(), refIdx, e.what());
                if (m_host.GetRef())
                        m_host->ScriptTrace(buf);
                fprintf(stderr, "[citizen-scripting-cpp] %s", buf);
                result = { static_cast<char>(0x90) };
        }
        catch (...)
        {
                char buf[256];
                snprintf(buf, sizeof(buf), "SCRIPT ERROR: @%s: Unhandled non-standard exception in ref %d\n", m_resourceName.c_str(), refIdx);
                if (m_host.GetRef())
                        m_host->ScriptTrace(buf);
                fprintf(stderr, "[citizen-scripting-cpp] %s", buf);
                result = { static_cast<char>(0x90) };
        }
        auto buf = fx::MakeNew<ScriptBuffer>(std::move(result));
        return buf->QueryInterface(IScriptBuffer::GetIID(), reinterpret_cast<void**>(retval));
}

result_t OM_DECL CppScriptRuntime::DuplicateRef(int32_t refIdx, int32_t* newRefIdx)
{
        auto it = m_refs.find(refIdx);
        if (it == m_refs.end())
                return FX_E_INVALIDARG;
        uint32_t idx = m_nextRefIdx;
        uint32_t start = idx;
        while (m_refs.count(static_cast<int32_t>(idx)))
        {
                if (++idx == 0)
                        idx = 1;
                if (idx == start)
                        return FX_E_INVALIDARG;
        }
        m_nextRefIdx = idx + 1;
        if (m_nextRefIdx == 0)
                m_nextRefIdx = 1;
        *newRefIdx = static_cast<int32_t>(idx);
        m_refs[*newRefIdx] = it->second;
#ifdef FXCPP_WASM_SUPPORT
        if (m_mode == Mode::Wasm)
        {
                auto cit = m_refToCallbackId.find(refIdx);
                if (cit != m_refToCallbackId.end())
                {
                        m_refToCallbackId[*newRefIdx] = cit->second;
                        wasmDuplicateRef(cit->second);
                }
        }
#endif
        return FX_S_OK;
}

result_t OM_DECL CppScriptRuntime::RemoveRef(int32_t refIdx)
{
#ifdef FXCPP_WASM_SUPPORT
        if (m_mode == Mode::Wasm)
        {
                auto cit = m_refToCallbackId.find(refIdx);
                if (cit != m_refToCallbackId.end())
                {
                        wasmRemoveRef(cit->second);
                        m_refToCallbackId.erase(cit);
                }
        }
#endif
        m_refs.erase(refIdx);
        return FX_S_OK;
}

int32_t OM_DECL CppScriptRuntime::HandlesFile(char* scriptFile, IScriptHostWithResourceData*)
{
        if (!scriptFile)
                return 0;
        std::string_view file(scriptFile);
        if (file.ends_with(".so"))
                return 1;
#ifdef FXCPP_WASM_SUPPORT
        if (file.ends_with(".wasm"))
                return 1;
#endif
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
                fprintf(stderr, "[citizen-scripting-cpp] Could not get resource path for '%s'\n", m_resourceName.c_str());
                return FX_E_INVALIDARG;
        }
        {
                fx::OMPtr<fxIStream> stream;
                if (FX_FAILED(m_host->OpenHostFile(scriptFile, stream.GetAddressOf())) || !stream.GetRef())
                {
                        fprintf(stderr, "[citizen-scripting-cpp] Host denied access to '%s' in resource '%s'\n", scriptFile, m_resourceName.c_str());
                        return FX_E_INVALIDARG;
                }
        }
        std::string resolvedPath, resolvedRoot;
        if (!ValidateScriptPath(scriptFile, root, resolvedPath, resolvedRoot, m_resourceName.c_str()))
                return FX_E_INVALIDARG;
        std::string_view file(scriptFile);

#ifdef FXCPP_WASM_SUPPORT
        if (file.ends_with(".wasm"))
                return loadWasm(resolvedPath);
#endif
        std::string allowed = GetConvar(m_host.GetRef(), "sv_allowNativeCode", "false");
        if (allowed != "true" && allowed != "1")
        {
                fprintf(stderr, "[citizen-scripting-cpp] Blocked native .so resource '%s'. Native code runs unsandboxed with full host access. To allow, add 'sv_allowNativeCode true' to your server.cfg. Consider using .wasm for sandboxed execution.\n", m_resourceName.c_str());
                return FX_E_INVALIDARG;
        }
        return loadSharedLib(resolvedPath);
}

result_t OM_DECL CppScriptRuntime::WalkStack(char*, uint32_t, char*, uint32_t, IScriptStackWalkVisitor*)
{
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
#ifdef FXCPP_WASM_SUPPORT
        if (m_mode == Mode::Wasm)
                *memUsage = static_cast<int64_t>(wasmMemSize());
        else
#endif
                *memUsage = 0;
        return FX_S_OK;
}

result_t OM_DECL CppScriptRuntime::EmitWarning(char* channel, char* message)
{
        if (message)
        {
                const char* ch = channel ? channel : "script";
                char buf[1024];
                int n = snprintf(buf, sizeof(buf), "[warning:%s] %s\n", ch, message);
                if (n > 0)
                {
                        if (m_host.GetRef())
                                m_host->ScriptTrace(buf);
                        fprintf(stderr, "[script:%s] %s", m_resourceName.c_str(), buf);
                }
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
