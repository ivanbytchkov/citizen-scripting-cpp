#pragma once

#include "Types.h"
#include "msgpack.h"
#include "../cfx/fxScripting.h"

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdio>
#include <cstdarg>

#if defined(_WIN32)
#define FXCPP_RESOURCE_EXPORT __declspec(dllexport)
#else
#define FXCPP_RESOURCE_EXPORT __attribute__((visibility("default")))
#endif

namespace fx
{

class ResourceContext
{
public:
    ResourceContext(IScriptHost* host, IScriptRuntime* runtime, std::string name, IScriptRuntimeHandler* handler = nullptr, AddRefFn addRefFn = nullptr) : m_host(host), m_runtime(runtime), m_name(std::move(name)), m_handler(fx::OMPtr<IScriptRuntimeHandler>(handler)), m_addRef(std::move(addRefFn))
    {
        fx::OMPtr<IScriptHost> h(host);
        h.As(&m_metadataHost);
    }

    // Events
    void on(const std::string& event, EventHandler h);
    void registerNetEvent(const std::string& event);
    void onNet(const std::string& event, EventHandler h);
    void onTick(TickHandler h);
    void onCommand(const std::string& command, CommandHandler h);

    // Lifecycle
    void onStop(StopHandler h);

    // Timers
    int32_t setTimeout(uint32_t ms, std::function<void()> cb);
    int32_t setInterval(uint32_t ms, std::function<void()> cb);
    void clearTimer(int32_t id);

    // exports
    void addExport(const std::string& name, ExportHandler handler);
    json::Value callExport(const std::string& resource, const std::string& name, const std::vector<std::string>& args = {});

    // Emit
    void trace(const char* fmt, ...);
    void emit(const std::string& event, const std::vector<std::string>& rawArgs = {});
    void emitNet(const std::string& event, int target, const std::vector<std::string>& rawArgs = {});

    // Statebags
    void setStateBagValue(const std::string& bagName, const std::string& key, const json::Value& value, bool replicated = true);
    void setPlayerState(int serverId, const std::string& key, const json::Value& value, bool replicated = true);
    void setEntityState(int netId, const std::string& key, const json::Value& value, bool replicated = true);
    void setGlobalState(const std::string& key, const json::Value& value, bool replicated = true);

    // Metadata
    std::string getResourceMetadata(const std::string& key, int index = 0);
    int getNumResourceMetadata(const std::string& key);

    void dispatchTick();
    void dispatchEvent(const std::string& name, const json::Value& args, const std::string& source);
    void dispatchCommand(const std::string& command, const std::string& source, const std::vector<std::string>& args);
    void dispatchStop();

    IScriptHost* getHost() { return m_host; }
    IScriptRuntime* getRuntime() { return m_runtime; }
    IScriptHostWithResourceData* getMetadataHost() { return m_metadataHost.GetRef(); }
    const std::string& resourceName() const { return m_name; }

    template<typename... Args>
    void invokeNative(uint64_t hash, Args... args)
    {
        static_assert(sizeof...(args) <= 32, "Native call exceeds 32-argument limit");
        PushEnvironment env(m_handler.GetRef(), m_runtime);
        fxNativeContext ctx{};
        ctx.nativeIdentifier = hash;
        size_t idx = 0;
        ((ctx.arguments[idx++] = static_cast<uintptr_t>(args)), ...);
        ctx.numArguments = static_cast<int>(idx);
        m_host->InvokeNative(ctx);
    }

private:
    IScriptHost* m_host = nullptr;
    IScriptRuntime* m_runtime = nullptr;
    fx::OMPtr<IScriptRuntimeHandler> m_handler;
    AddRefFn m_addRef;
    fx::OMPtr<IScriptHostWithResourceData> m_metadataHost;
    std::string m_name;
    std::unordered_map<std::string, std::vector<EventHandler>> m_eventHandlers;
    std::unordered_map<std::string, std::vector<CommandHandler>> m_commandHandlers;
    std::vector<TickHandler> m_tickHandlers;
    std::unordered_map<int32_t, TimerEntry> m_timers;
    int32_t m_nextTimerId = 1;
    std::unordered_set<std::string> m_netSafeEvents;
    std::vector<StopHandler> m_stopHandlers;
};

namespace detail { inline ResourceContext* g_ctx = nullptr; }
inline ResourceContext* GetContext() { return detail::g_ctx; }

}

#include "../impl/Events.inl"
#include "../impl/Lifecycle.inl"
#include "../impl/Timers.inl"
#include "../impl/Exports.inl"
#include "../impl/Emit.inl"
#include "../impl/Statebags.inl"
#include "../impl/Metadata.inl"

#define FXCPP_RESOURCE \
    static void _fxcpp_resource_body(fx::ResourceContext&); \
    extern "C" FXCPP_RESOURCE_EXPORT \
    void fxcpp_init(fx::ResourceContext* _ctx) \
    { \
        fx::detail::g_ctx = _ctx; \
        _fxcpp_resource_body(*_ctx); \
    } \
    static void _fxcpp_resource_body([[maybe_unused]] fx::ResourceContext& ctx)
