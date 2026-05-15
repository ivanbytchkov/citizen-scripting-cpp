#pragma once

#ifdef __wasm__
#include "CppScriptWasm.h"
#else
#include "CppScriptNative.h"
#endif

#ifndef __wasm__

namespace fx::natives
{

namespace detail
{
    inline void pushArg(fxNativeContext& ctx, size_t idx, int32_t v)
    {
        ctx.arguments[idx] = static_cast<uintptr_t>(v);
    }
    inline void pushArg(fxNativeContext& ctx, size_t idx, uint32_t v)
    {
        ctx.arguments[idx] = static_cast<uintptr_t>(v);
    }
    inline void pushArg(fxNativeContext& ctx, size_t idx, int64_t v)
    {
        ctx.arguments[idx] = static_cast<uintptr_t>(v);
    }
    inline void pushArg(fxNativeContext& ctx, size_t idx, uint64_t v)
    {
        ctx.arguments[idx] = static_cast<uintptr_t>(v);
    }
    inline void pushArg(fxNativeContext& ctx, size_t idx, float v)
    {
        uint32_t bits;
        memcpy(&bits, &v, 4);
        ctx.arguments[idx] = static_cast<uintptr_t>(bits);
    }
    inline void pushArg(fxNativeContext& ctx, size_t idx, double v)
    {
        float f = static_cast<float>(v);
        uint32_t bits;
        memcpy(&bits, &f, 4);
        ctx.arguments[idx] = static_cast<uintptr_t>(bits);
    }
    inline void pushArg(fxNativeContext& ctx, size_t idx, bool v)
    {
        ctx.arguments[idx] = v ? 1u : 0u;
    }
    inline void pushArg(fxNativeContext& ctx, size_t idx, const char* v)
    {
        ctx.arguments[idx] = reinterpret_cast<uintptr_t>(v);
    }
    inline void pushArg(fxNativeContext& ctx, size_t idx, const std::string& v)
    {
        ctx.arguments[idx] = reinterpret_cast<uintptr_t>(v.c_str());
    }
    template<typename T>
    inline void pushArg(fxNativeContext& ctx, size_t idx, T* v)
    {
        ctx.arguments[idx] = reinterpret_cast<uintptr_t>(v);
    }
}

template<typename... TArgs>
inline fxNativeContext invokeCtx(uint64_t hash, TArgs&&... args)
{
    static_assert(sizeof...(args) <= 32, "Native call exceeds 32-argument limit");
    auto* ctx = fx::detail::g_ctx;
    if (!ctx) return {};
    fxNativeContext nctx{};
    nctx.nativeIdentifier = hash;
    size_t idx = 0;
    (detail::pushArg(nctx, idx++, std::forward<TArgs>(args)), ...);
    nctx.numArguments = static_cast<int>(idx);
    nctx.numResults = 3;
    if (FX_FAILED(ctx->getHost()->InvokeNative(nctx)))
        ctx->traceNativeError();
    return nctx;
}

template<typename... TArgs>
inline uintptr_t invokeRaw(uint64_t hash, TArgs&&... args)
{
    return invokeCtx(hash, std::forward<TArgs>(args)...).arguments[0];
}

template<typename TResult = void, typename... TArgs>
inline TResult invoke(uint64_t hash, TArgs&&... args)
{
    if constexpr (std::is_same_v<TResult, void>)
    {
        invokeRaw(hash, std::forward<TArgs>(args)...);
    }
    else if constexpr (std::is_same_v<TResult, std::string>)
    {
        uintptr_t raw = invokeRaw(hash, std::forward<TArgs>(args)...);
        const char* s = reinterpret_cast<const char*>(raw);
        return s ? std::string(s) : std::string{};
    }
    else if constexpr (std::is_same_v<TResult, bool>)
    {
        return invokeRaw(hash, std::forward<TArgs>(args)...) != 0;
    }
    else if constexpr (std::is_same_v<TResult, fx::Vector3>)
    {
        auto nctx = invokeCtx(hash, std::forward<TArgs>(args)...);
        fx::Vector3 v;
        uint32_t bx = static_cast<uint32_t>(nctx.arguments[0]);
        uint32_t by = static_cast<uint32_t>(nctx.arguments[1]);
        uint32_t bz = static_cast<uint32_t>(nctx.arguments[2]);
        memcpy(&v.x, &bx, 4);
        memcpy(&v.y, &by, 4);
        memcpy(&v.z, &bz, 4);
        return v;
    }
    else if constexpr (std::is_floating_point_v<TResult>)
    {
        uintptr_t raw = invokeRaw(hash, std::forward<TArgs>(args)...);
        float f;
        uint32_t bits = static_cast<uint32_t>(raw);
        memcpy(&f, &bits, 4);
        return static_cast<TResult>(f);
    }
    else
    {
        return static_cast<TResult>(invokeRaw(hash, std::forward<TArgs>(args)...));
    }
}

}

#endif
