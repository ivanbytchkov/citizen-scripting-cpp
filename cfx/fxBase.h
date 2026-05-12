#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>

typedef uint32_t result_t;

#define FX_S_OK 0x0
#define FX_E_NOTIMPL 0x80004001
#define FX_E_NOINTERFACE 0x80004002
#define FX_E_INVALIDARG 0x80070057

#define FX_SUCCEEDED(x) (((x) & 0x80000000) == 0)
#define FX_FAILED(x) (!FX_SUCCEEDED(x))

struct guid_t
{
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};

#define FX_DEFINE_GUID(name, d1, d2, d3, a, b, c, d, e, f, g, h) \
    static constexpr guid_t name = { d1, d2, d3, { a, b, c, d, e, f, g, h } }

namespace fx
{
    inline bool GuidEquals(const guid_t& l, const guid_t& r)
    {
        return memcmp(&l, &r, sizeof(guid_t)) == 0;
    }
    inline guid_t GetNullGuid() { return {}; }
    inline bool IsNullGuid(const guid_t& g) { return GuidEquals(g, GetNullGuid()); }
}

inline bool operator==(const guid_t& a, const guid_t& b) { return fx::GuidEquals(a, b); }
inline bool operator!=(const guid_t& a, const guid_t& b) { return !fx::GuidEquals(a, b); }
inline bool operator<(const guid_t& a, const guid_t& b) { return memcmp(&a, &b, sizeof(guid_t)) < 0; }

#ifdef _MSC_VER
#define OM_DECL __stdcall
#define FXCPP_EXPORT __declspec(dllexport)
#define EXPORTED_TYPE __declspec(dllexport)
#else
#define OM_DECL
#define FXCPP_EXPORT __attribute__((visibility("default")))
#define EXPORTED_TYPE __attribute__((visibility("default")))
#endif

#define NS_IMETHOD_(rv) virtual rv OM_DECL
#define NS_IMETHOD NS_IMETHOD_(result_t)
#define NS_DECLARE_STATIC_IID_ACCESSOR(iid_const) \
    static inline guid_t GetIID() { return iid_const; }

inline void* fwAlloc(size_t size) { return malloc(size); }
inline void fwFree(void* p) { free(p); }

inline constexpr uint32_t HashString(std::string_view str)
{
    uint32_t hash = 0;
    for (char c : str)
    {
        hash += (c >= 'A' && c <= 'Z') ? static_cast<uint32_t>(c | 0x20) : static_cast<uint32_t>(c);
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);
    return hash;
}
