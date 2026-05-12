#pragma once

#include "Resource.h"

namespace fx
{

inline int32_t setTimeout(uint32_t ms, std::function<void()> cb)
{
    if (auto* ctx = detail::g_ctx)
        return ctx->setTimeout(ms, std::move(cb));
    return -1;
}

inline int32_t setInterval(uint32_t ms, std::function<void()> cb)
{
    if (auto* ctx = detail::g_ctx)
        return ctx->setInterval(ms, std::move(cb));
    return -1;
}

inline void clearTimer(int32_t id)
{
    if (auto* ctx = detail::g_ctx)
        ctx->clearTimer(id);
}

}
