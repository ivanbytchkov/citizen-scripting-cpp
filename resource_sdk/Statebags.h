#pragma once

#include "Resource.h"

namespace fx
{

inline void setStateBagValue(const std::string& bagName, const std::string& key, const json::Value& value, bool replicated = true)
{
    if (auto* ctx = detail::g_ctx)
        ctx->setStateBagValue(bagName, key, value, replicated);
}

inline void setPlayerState(int serverId, const std::string& key, const json::Value& value, bool replicated = true)
{
    if (auto* ctx = detail::g_ctx)
        ctx->setPlayerState(serverId, key, value, replicated);
}

inline void setEntityState(int netId, const std::string& key, const json::Value& value, bool replicated = true)
{
    if (auto* ctx = detail::g_ctx)
        ctx->setEntityState(netId, key, value, replicated);
}

inline void setGlobalState(const std::string& key, const json::Value& value, bool replicated = true)
{
    if (auto* ctx = detail::g_ctx)
        ctx->setGlobalState(key, value, replicated);
}

}
