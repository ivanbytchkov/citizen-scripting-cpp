#pragma once

#include "Resource.h"

namespace fx
{

inline void on(const std::string& event, EventHandler handler)
{
    if (auto* ctx = detail::g_ctx)
        ctx->on(event, std::move(handler));
}

inline void registerNetEvent(const std::string& event)
{
    if (auto* ctx = detail::g_ctx)
        ctx->registerNetEvent(event);
}

inline void onNet(const std::string& event, EventHandler handler)
{
    if (auto* ctx = detail::g_ctx)
        ctx->onNet(event, std::move(handler));
}

inline void onTick(TickHandler handler)
{
    if (auto* ctx = detail::g_ctx)
        ctx->onTick(std::move(handler));
}

inline void onCommand(const std::string& command, CommandHandler handler)
{
    if (auto* ctx = detail::g_ctx)
        ctx->onCommand(command, std::move(handler));
}

inline void onStop(StopHandler handler)
{
    if (auto* ctx = detail::g_ctx)
        ctx->onStop(std::move(handler));
}

template<typename... TArgs>
inline void trace(const char* fmt, TArgs&&... args)
{
    if (auto* ctx = detail::g_ctx)
        ctx->trace(fmt, std::forward<TArgs>(args)...);
}

inline void emit(const std::string& event, const std::vector<std::string>& rawArgs = {})
{
    if (auto* ctx = detail::g_ctx)
        ctx->emit(event, rawArgs);
}

inline void emitNet(const std::string& event, int target, const std::vector<std::string>& rawArgs = {})
{
    if (auto* ctx = detail::g_ctx)
        ctx->emitNet(event, target, rawArgs);
}

}
