#pragma once

#include "cfx/fxScripting.h"
#include "cfx/core.h"
#include "resource_sdk/Resource.h"
#include "resource_sdk/msgpack.h"

#include <string>
#include <atomic>
#include <cstdlib>
#include <unordered_map>
#include <functional>

FX_DEFINE_GUID(CLSID_Runtime, 0xF3A7B9, 0x241D, 0x5E4C, 0x8A, 0x93, 0x2F, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5);

using RefCallback = std::function<std::vector<char>(const char* argsSerialized, uint32_t argsSize)>;

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
            m_host->SubmitBoundaryStart(nullptr, 0);
    }
    BoundaryGuard(const BoundaryGuard&) = delete;
    BoundaryGuard& operator=(const BoundaryGuard&) = delete;
};

class Runtime final : public fx::OMClass<Runtime, IScriptRuntime, IScriptTickRuntime, IScriptEventRuntime, IScriptRefRuntime, IScriptFileHandlingRuntime>
{
public:
    Runtime();
    ~Runtime();
    result_t OM_DECL Create (IScriptHost* host) override;
    result_t OM_DECL Destroy() override;
    void* OM_DECL GetParentObject() override { return m_parentObject; }
    void OM_DECL SetParentObject(void*) override;
    int32_t OM_DECL GetInstanceId() override { return m_instanceId; }
    result_t OM_DECL Tick() override;
    result_t OM_DECL TriggerEvent(char* eventName, char* argsSerialized, uint32_t serializedSize, char* sourceId) override;
    result_t OM_DECL CallRef(int32_t refIdx, char* argsSerialized, uint32_t argsSize, IScriptBuffer** retval) override;
    result_t OM_DECL DuplicateRef(int32_t refIdx, int32_t* newRefIdx) override;
    result_t OM_DECL RemoveRef(int32_t refIdx) override;
    int32_t OM_DECL HandlesFile(char* scriptFile, IScriptHostWithResourceData* metadata) override;
    result_t OM_DECL LoadFile(char* scriptFile) override;
    int32_t AddFuncRef(RefCallback cb);

private:
    IScriptHost* m_host = nullptr;
    void* m_parentObject = nullptr;
    int32_t m_instanceId = 0;
    void* m_libHandle = nullptr;
    fx::ResourceContext* m_ctx = nullptr;
    std::string m_resourceName;
    std::unordered_map<int32_t, RefCallback> m_refs;
    int32_t m_nextRefIdx = 1;
    int64_t m_nextBoundaryId = 1;
};
