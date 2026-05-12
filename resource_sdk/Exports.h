#pragma once

#include "Resource.h"

namespace fx
{

inline void addExport(const std::string& name, ExportHandler handler)
{
    if (auto* ctx = detail::g_ctx)
        ctx->addExport(name, std::move(handler));
}

inline json::Value callExport(const std::string& resource, const std::string& name, const std::vector<std::string>& args = {})
{
    if (auto* ctx = detail::g_ctx)
        return ctx->callExport(resource, name, args);
    return {};
}

}
