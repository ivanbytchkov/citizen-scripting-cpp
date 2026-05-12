#pragma once

#include "Resource.h"

namespace fx
{

inline std::string getResourceMetadata(const std::string& key, int index = 0)
{
    if (auto* ctx = detail::g_ctx)
        return ctx->getResourceMetadata(key, index);
    return {};
}

inline int getNumResourceMetadata(const std::string& key)
{
    if (auto* ctx = detail::g_ctx)
        return ctx->getNumResourceMetadata(key);
    return 0;
}

}
