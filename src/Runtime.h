#pragma once

#include "../include/fxScripting.h"
#include "../include/core.h"
#include "Resource.h"
#include "Coroutine/Coroutine.h"
#include "Interop/MsgPackSerializer.h"
#include "Interop/MsgPackDeserializer.h"

#include <string>
#include <unordered_map>
#include <functional>

#ifdef FXCPP_WASM_SUPPORT
#include <wasmtime.h>
#endif

FX_DEFINE_GUID(CLSID_Runtime, 0xF3A7B9, 0x241D, 0x5E4C, 0x8A, 0x93, 0x2F, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5);

struct CppBoundary
{
    int64_t hint;
};

class BoundaryGuard
{
    IScriptHost* m_host;
public:
    BoundaryGuard(IScriptHost* host, int64_t hint) : m_host(host)
    {
        if (m_host)
        {
            CppBoundary b{ hint };
            m_host->SubmitBoundaryStart(reinterpret_cast<char*>(&b), sizeof(b));
        }
    }
    ~BoundaryGuard()
    {
        if (m_host)
            m_host->SubmitBoundaryEnd(nullptr, 0);
    }
    BoundaryGuard(const BoundaryGuard&) = delete;
    BoundaryGuard& operator=(const BoundaryGuard&) = delete;
};

#ifdef FXCPP_WASM_SUPPORT
struct WasmNativeCtx
{
    uint64_t hash;
    uint32_t numArgs;
    uint32_t numResults;
    uint64_t args[32];
    uint32_t ptrMask;
    uint32_t resultPtrMask;
};
static_assert(sizeof(WasmNativeCtx) == 280, "WasmNativeCtx layout mismatch");
#endif

class Runtime final : public fx::OMClass<Runtime, IScriptRuntime, IScriptTickRuntime, IScriptEventRuntime, IScriptRefRuntime, IScriptFileHandlingRuntime, IScriptTickRuntimeWithBookmarks, IScriptStackWalkingRuntime, IScriptMemInfoRuntime, IScriptWarningRuntime, IScriptProfiler>
{
public:
    Runtime();
    ~Runtime();
    result_t OM_DECL Create(IScriptHost* host) override;
    result_t OM_DECL Destroy() override;
    void* OM_DECL GetParentObject() override { return m_parentObject; }
    void OM_DECL SetParentObject(void*) override;
    int32_t OM_DECL GetInstanceId() override { return m_instanceId; }
    result_t OM_DECL Tick() override;
    result_t OM_DECL TickBookmarks(uint64_t* bookmarks, int32_t numBookmarks) override;
    result_t OM_DECL TriggerEvent(char* eventName, char* argsSerialized, uint32_t serializedSize, char* sourceId) override;
    result_t OM_DECL CallRef(int32_t refIdx, char* argsSerialized, uint32_t argsSize, IScriptBuffer** retval) override;
    result_t OM_DECL DuplicateRef(int32_t refIdx, int32_t* newRefIdx) override;
    result_t OM_DECL RemoveRef(int32_t refIdx) override;
    int32_t OM_DECL HandlesFile(char* scriptFile, IScriptHostWithResourceData* metadata) override;
    result_t OM_DECL LoadFile(char* scriptFile) override;
    result_t OM_DECL WalkStack(char* boundaryStart, uint32_t boundaryStartLength, char* boundaryEnd, uint32_t boundaryEndLength, IScriptStackWalkVisitor* visitor) override;
    result_t OM_DECL RequestMemoryUsage() override;
    result_t OM_DECL GetMemoryUsage(int64_t* memUsage) override;
    result_t OM_DECL EmitWarning(char* channel, char* message) override;
    void OM_DECL SetupFxProfiler(void* obj, int32_t resourceId) override;
    void OM_DECL ShutdownFxProfiler() override;
    int32_t AddFuncRef(fx::RefCallback cb);
    IScriptHost* host() const { return m_host.GetRef(); }
    const std::string& resourceName() const { return m_resourceName; }

#ifdef FXCPP_WASM_SUPPORT
    bool& eventCanceled() { return m_eventCanceled; }
    fxNativeContext& lastNativeCtx() { return m_lastNativeCtx; }
    uint32_t& lastResultPtrMask() { return m_lastResultPtrMask; }
    uint8_t* wasmBase();
    size_t wasmMemSize();
    uint32_t wasmAlloc(uint32_t size);
    void wasmFree(uint32_t ptr, uint32_t size);
    bool callVoid(const wasmtime_func_t& fn);
    bool callEvent(uint32_t namePtr, uint32_t nameLen, uint32_t argsPtr, uint32_t argsLen, uint32_t srcPtr, uint32_t srcLen);
    bool callInvokeRef(uint32_t callbackId, const char* argsSerialized, uint32_t argsSize, std::vector<char>& result);
    void wasmMapRef(int32_t refIdx, int32_t callbackId);
    void wasmDuplicateRef(int32_t callbackId);
    void wasmRemoveRef(int32_t callbackId);
#endif

private:
    enum class Mode { None, SharedLib, Wasm };
    Mode m_mode = Mode::None;
    fx::OMPtr<IScriptHost> m_host;
    fx::OMPtr<IScriptHostWithBookmarks> m_bookmarkHost;
    fx::OMPtr<IScriptHostWithResourceData> m_metadataHost;
    void* m_parentObject = nullptr;
    int32_t m_instanceId = 0;
    std::string m_resourceName;
    std::unordered_map<int32_t, fx::RefCallback> m_refs;
    uint32_t m_nextRefIdx = 1;
    uint64_t m_nextBoundaryId = 1;
    uint64_t nextBoundaryId();
    void* m_libHandle = nullptr;
    fx::ResourceContext* m_ctx = nullptr;
    std::string m_tempLibPath;
    std::string m_tempDir;
    void cleanupTemp();
    void cleanupLoadFailure();
    result_t loadSharedLib(const std::string& resolvedPath);

#ifdef FXCPP_WASM_SUPPORT
    wasmtime_store_t* m_store = nullptr;
    wasmtime_module_t* m_module = nullptr;
    wasmtime_linker_t* m_linker = nullptr;
    wasmtime_instance_t m_instance{};
    wasmtime_memory_t m_memory{};
    bool m_hasMemory = false;
    wasmtime_func_t m_fnTick{};
    wasmtime_func_t m_fnEvent{};
    wasmtime_func_t m_fnStop{};
    wasmtime_func_t m_fnAlloc{};
    wasmtime_func_t m_fnFree{};
    wasmtime_func_t m_fnInvokeRef{};
    wasmtime_func_t m_fnDuplicateRef{};
    wasmtime_func_t m_fnRemoveRef{};
    bool m_hasTickFn = false;
    bool m_hasEventFn = false;
    bool m_hasStopFn = false;
    bool m_hasAllocFn = false;
    bool m_hasFreeFn = false;
    bool m_hasInvokeRefFn = false;
    bool m_hasDuplicateRefFn = false;
    bool m_hasRemoveRefFn = false;
    std::unordered_map<int32_t, int32_t> m_refToCallbackId;
    bool m_eventCanceled = false;
    fxNativeContext m_lastNativeCtx{};
    uint32_t m_lastResultPtrMask = 0;
    void defineImports();
    bool resolveExports();
    void destroyWasm();
    std::string wasmErrMsg(wasmtime_error_t* err, wasm_trap_t* trap);
    result_t loadWasm(const std::string& resolvedPath);
    static wasm_engine_t* engine();
#endif
};
