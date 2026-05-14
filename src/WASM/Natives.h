#pragma once

#include "Imports.h"
#include "Types.h"

namespace fx
{

inline NativeCtx invokeNative(uint64_t hash, std::initializer_list<NativeArg> args, uint32_t numResults = 0, uint32_t resultPtrMask = 0)
{
    NativeCtx ctx{};
    ctx.hash = hash;
    ctx.numArgs = static_cast<uint32_t>(args.size());
    ctx.numResults = numResults;
    ctx.resultPtrMask = resultPtrMask;
    uint32_t i = 0;
    for (const auto& a : args)
    {
        ctx.args[i] = a.value;
        if (a.isPtr) ctx.ptrMask |= (1u << i);
        ++i;
    }
    __fxcpp_invoke_native(reinterpret_cast<uint32_t>(&ctx));
    return ctx;
}

inline std::string getStringResult(NativeCtx& ctx, int32_t resultIdx)
{
    int32_t len = __fxcpp_copy_string_result(
        reinterpret_cast<uint32_t>(&ctx), resultIdx, nullptr, 0);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    __fxcpp_copy_string_result(
        reinterpret_cast<uint32_t>(&ctx), resultIdx, out.data(), len + 1);
    return out;
}

}

namespace fx::natives
{

namespace detail
{
    inline void pushArgs(NativeCtx&, uint32_t) {}
    template<typename T, typename... Rest>
    inline void pushArgs(NativeCtx& ctx, uint32_t idx, T&& arg, Rest&&... rest)
    {
        using D = std::decay_t<T>;
        if constexpr (std::is_same_v<D, const char*> || std::is_same_v<D, char*>)
        {
            ctx.args[idx] = reinterpret_cast<uint64_t>(arg);
            ctx.ptrMask |= (1u << idx);
        }
        else if constexpr (std::is_same_v<D, std::string>)
        {
            ctx.args[idx] = reinterpret_cast<uint64_t>(arg.c_str());
            ctx.ptrMask |= (1u << idx);
        }
        else if constexpr (std::is_same_v<D, float>)
        {
            uint32_t bits; memcpy(&bits, &arg, 4);
            ctx.args[idx] = bits;
        }
        else if constexpr (std::is_same_v<D, bool>)
        {
            ctx.args[idx] = arg ? 1 : 0;
        }
        else if constexpr (std::is_pointer_v<D>)
        {
            ctx.args[idx] = reinterpret_cast<uint64_t>(arg);
            ctx.ptrMask |= (1u << idx);
        }
        else
        {
            ctx.args[idx] = static_cast<uint64_t>(arg);
        }
        pushArgs(ctx, idx + 1, std::forward<Rest>(rest)...);
    }
}

template<typename... TArgs>
inline NativeCtx invokeCtx(uint64_t hash, TArgs&&... args)
{
    NativeCtx ctx{};
    ctx.hash = hash;
    ctx.numArgs = static_cast<uint32_t>(sizeof...(args));
    ctx.numResults = 3;
    detail::pushArgs(ctx, 0, std::forward<TArgs>(args)...);
    __fxcpp_invoke_native(reinterpret_cast<uint32_t>(&ctx));
    return ctx;
}

template<typename... TArgs>
inline uint64_t invokeRaw(uint64_t hash, TArgs&&... args)
{
    return invokeCtx(hash, std::forward<TArgs>(args)...).args[0];
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
        NativeCtx ctx{};
        ctx.hash = hash;
        ctx.numArgs = static_cast<uint32_t>(sizeof...(args));
        ctx.numResults = 3;
        ctx.resultPtrMask = 0x1;
        detail::pushArgs(ctx, 0, std::forward<TArgs>(args)...);
        __fxcpp_invoke_native(reinterpret_cast<uint32_t>(&ctx));
        return getStringResult(ctx, 0);
    }
    else if constexpr (std::is_same_v<TResult, bool>)
    {
        return invokeRaw(hash, std::forward<TArgs>(args)...) != 0;
    }
    else if constexpr (std::is_same_v<TResult, Vector3>)
    {
        auto ctx = invokeCtx(hash, std::forward<TArgs>(args)...);
        Vector3 v;
        uint32_t bx = static_cast<uint32_t>(ctx.args[0]);
        uint32_t by = static_cast<uint32_t>(ctx.args[1]);
        uint32_t bz = static_cast<uint32_t>(ctx.args[2]);
        memcpy(&v.x, &bx, 4);
        memcpy(&v.y, &by, 4);
        memcpy(&v.z, &bz, 4);
        return v;
    }
    else if constexpr (std::is_floating_point_v<TResult>)
    {
        uint64_t raw = invokeRaw(hash, std::forward<TArgs>(args)...);
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
