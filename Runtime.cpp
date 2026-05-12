#include "Runtime.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <dlfcn.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static std::string GetResourcePath(IScriptHost* host)
{
    fx::OMPtr<IScriptHostWithResourceData> md;
    fx::OMPtr<IScriptHost> h(host);
    if (FX_FAILED(h.As(&md)) || !md.GetRef())
        return {};
    char* name = nullptr;
    if (FX_FAILED(md->GetResourceName(&name)) || !name)
        return {};
    fxNativeContext ctx{};
    ctx.nativeIdentifier = HashString("GET_RESOURCE_PATH");
    ctx.arguments[0] = reinterpret_cast<uintptr_t>(name);
    ctx.numArguments = 1;
    ctx.numResults = 1;
    host->InvokeNative(ctx);
    const char* path = reinterpret_cast<const char*>(ctx.arguments[0]);
    return path ? std::string(path) : std::string{};
}

static bool EndsWith(std::string_view s, std::string_view suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

Runtime::Runtime() : m_instanceId(static_cast<int32_t>(reinterpret_cast<intptr_t>(this) & 0x7FFFFFFF))
{}

Runtime::~Runtime()
{
    Destroy();
}

result_t OM_DECL Runtime::Create(IScriptHost* host)
{
    m_host = host;
    fx::OMPtr<IScriptHostWithResourceData> md;
    fx::OMPtr<IScriptHost> h(host);
    if (FX_SUCCEEDED(h.As(&md)) && md.GetRef())
    {
        char* name = nullptr;
        if (FX_SUCCEEDED(md->GetResourceName(&name)) && name)
            m_resourceName = name;
    }
    return FX_S_OK;
}

result_t OM_DECL Runtime::Destroy()
{
    m_refs.clear();
    if (m_ctx)
    {
        m_ctx->dispatchStop();
        delete m_ctx;
        m_ctx = nullptr;
    }
#ifndef _WIN32
    if (m_libHandle) { dlclose(m_libHandle); m_libHandle = nullptr; }
#else
    if (m_libHandle) { FreeLibrary(static_cast<HMODULE>(m_libHandle)); m_libHandle = nullptr }
#endif
    m_host = nullptr;
    return FX_S_OK;
}

void OM_DECL Runtime::SetParentObject(void* obj)
{
    m_parentObject = obj;
}

result_t OM_DECL Runtime::Tick()
{
    if (!m_ctx) return FX_S_OK;
    BoundaryGuard boundary(m_host, m_nextBoundaryId++);
    try
    {
        m_ctx->dispatchTick();
    }
    catch (const std::exception& e)
    {
        m_ctx->trace("Unhandled exception in tick handler: %s\n", e.what());
    }
    catch (...)
    {
        m_ctx->trace("Unhandled non-standard exception in tick handler\n");
    }
    return FX_S_OK;
}

result_t OM_DECL Runtime::TriggerEvent(char* eventName, char* argsSerialized, uint32_t serializedSize, char* sourceId)
{
    if (!m_ctx || !eventName) return FX_S_OK;
    BoundaryGuard boundary(m_host, m_nextBoundaryId++);
    try
    {
        fx::json::Value args = fx::msgpack::decode(argsSerialized, serializedSize);
        if (args.kind != fx::json::Value::Kind::Array)
        {
            fx::json::Value wrapper;
            wrapper.kind = fx::json::Value::Kind::Array;
            wrapper.children.push_back(std::move(args));
            args = std::move(wrapper);
        }
        std::string src = sourceId ? sourceId : "-1";
        m_ctx->dispatchEvent(eventName, args, src);
    }
    catch (const std::exception& e)
    {
        m_ctx->trace("Unhandled exception in event '%s': %s\n", eventName, e.what());
    }
    catch (...)
    {
        m_ctx->trace("Unhandled non-standard exception in event '%s'\n", eventName);
    }
    return FX_S_OK;
}

int32_t Runtime::AddFuncRef(RefCallback cb)
{
    int32_t idx = m_nextRefIdx++;
    m_refs[idx] = std::move(cb);
    return idx;
}

result_t OM_DECL Runtime::CallRef(int32_t refIdx, char* argsSerialized, uint32_t argsSize, IScriptBuffer** retval)
{
    auto it = m_refs.find(refIdx);
    if (it == m_refs.end()) return FX_E_INVALIDARG;
    BoundaryGuard boundary(m_host, m_nextBoundaryId++);
    std::vector<char> result;
    try
    {
        result = it->second(argsSerialized, argsSize);
    }
    catch (const std::exception& e)
    {
        if (m_ctx)
            m_ctx->trace("Unhandled exception in ref %d: %s\n", refIdx, e.what());
        result = { static_cast<char>(0xC0) }; // msgpack nil
    }
    catch (...)
    {
        if (m_ctx)
            m_ctx->trace("Unhandled non-standard exception in ref %d\n", refIdx);
        result = { static_cast<char>(0xC0) };
    }
    auto buf = fx::MakeNew<ScriptBuffer>(std::move(result));
    return buf->QueryInterface(IScriptBuffer::GetIID(), reinterpret_cast<void**>(retval));
}

result_t OM_DECL Runtime::DuplicateRef(int32_t refIdx, int32_t* newRefIdx)
{
    auto it = m_refs.find(refIdx);
    if (it == m_refs.end()) return FX_E_INVALIDARG;
    *newRefIdx = m_nextRefIdx++;
    m_refs[*newRefIdx] = it->second;
    return FX_S_OK;
}

result_t OM_DECL Runtime::RemoveRef(int32_t refIdx)
{
    m_refs.erase(refIdx);
    return FX_S_OK;
}

int32_t OM_DECL Runtime::HandlesFile(char* scriptFile, IScriptHostWithResourceData* /*metadata*/)
{
    if (!scriptFile) return 0;
    std::string_view file(scriptFile);
#ifdef _WIN32
    return EndsWith(file, ".dll") ? 1 : 0;
#else
    return EndsWith(file, ".so") ? 1 : 0;
#endif
}

result_t OM_DECL Runtime::LoadFile(char* scriptFile)
{
    if (!m_host || !scriptFile) return FX_E_INVALIDARG;
    std::string root = GetResourcePath(m_host);
    if (!root.empty() && root.back() == '/')
        root.pop_back();
    if (root.empty())
    {
        fprintf(stderr, "[fx-cpp-sdk] Runtime: could not get resource path for '%s'\n", m_resourceName.c_str());
        return FX_E_INVALIDARG;
    }
#ifdef _WIN32
    std::string fullPath = root + "\\" + scriptFile;
    m_libHandle = LoadLibraryA(fullPath.c_str());
    if (!m_libHandle)
    {
        fprintf(stderr, "[fx-cpp-sdk] LoadLibraryA failed for '%s': error %lu\n", fullPath.c_str(), GetLastError());
        return FX_E_INVALIDARG;
    }
    auto* initFn = reinterpret_cast<void(*)(fx::ResourceContext*)>(GetProcAddress(static_cast<HMODULE>(m_libHandle), "fxcpp_init"));
#else
    std::string fullPath = root + "/" + scriptFile;
    m_libHandle = dlopen(fullPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m_libHandle)
    {
        fprintf(stderr, "[fx-cpp-sdk] dlopen failed for '%s': %s\n", fullPath.c_str(), dlerror());
        return FX_E_INVALIDARG;
    }
    auto* initFn = reinterpret_cast<void(*)(fx::ResourceContext*)>(dlsym(m_libHandle, "fxcpp_init"));
#endif
    if (!initFn)
    {
        fprintf(stderr, "[fx-cpp-sdk] '%s' has no fxcpp_init export\n", fullPath.c_str());
        return FX_E_INVALIDARG;
    }
    fx::OMPtr<IScriptRuntimeHandler> runtimeHandler;
    fx::MakeInterface(&runtimeHandler, CLSID_ScriptRuntimeHandler);
    m_ctx = new fx::ResourceContext(m_host, this, m_resourceName, runtimeHandler.GetRef(), [this](RefCallback cb) -> int32_t { return AddFuncRef(std::move(cb)); });
    fprintf(stderr, "[fx-cpp-sdk] Loaded C++ resource '%s'\n", m_resourceName.c_str());
    try
    {
        initFn(m_ctx);
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "[fx-cpp-sdk] Exception during init of '%s': %s\n", m_resourceName.c_str(), e.what());
        m_ctx->trace("Exception during resource init: %s\n", e.what());
        return FX_E_INVALIDARG;
    }
    catch (...)
    {
        fprintf(stderr, "[fx-cpp-sdk] Non-standard exception during init of '%s'\n", m_resourceName.c_str());
        m_ctx->trace("Non-standard exception during resource init\n");
        return FX_E_INVALIDARG;
    }
    return FX_S_OK;
}
