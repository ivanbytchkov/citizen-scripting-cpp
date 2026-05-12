#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>

#include "../Json.h"

namespace fx::msgpack
{

namespace detail
{

struct Reader
{
    const uint8_t* p;
    const uint8_t* end;

    uint8_t u8()
    {
        if (p >= end) throw std::runtime_error("msgpack: truncated");
        return *p++;
    }

    uint16_t u16()
    {
        uint16_t v;
        if (p + 2 > end) throw std::runtime_error("msgpack: truncated");
        memcpy(&v, p, 2); p += 2;
        return static_cast<uint16_t>((v >> 8) | (v << 8));
    }

    uint32_t u32()
    {
        uint32_t v;
        if (p + 4 > end) throw std::runtime_error("msgpack: truncated");
        memcpy(&v, p, 4); p += 4;
        v = ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000u);
        return v;
    }

    uint64_t u64()
    {
        uint32_t hi = u32(), lo = u32();
        return (static_cast<uint64_t>(hi) << 32) | lo;
    }

    std::string str(size_t len)
    {
        if (p + len > end) throw std::runtime_error("msgpack: truncated");
        std::string s(reinterpret_cast<const char*>(p), len);
        p += len;
        return s;
    }

    void validateCount(uint32_t n) const
    {
        if (n > static_cast<uint32_t>(end - p))
            throw std::runtime_error("msgpack: element count exceeds remaining data");
    }

    json::Value readArray(uint32_t n)
    {
        validateCount(n);
        json::Value v;
        v.kind = json::Value::Kind::Array;
        v.children.reserve(n);
        for (uint32_t i = 0; i < n; ++i) v.children.push_back(read());
        return v;
    }

    json::Value readMap(uint32_t n)
    {
        validateCount(n);
        json::Value v;
        v.kind = json::Value::Kind::Object;
        for (uint32_t i = 0; i < n; ++i) { std::string key = read().scalar; v.fields[key] = read(); }
        return v;
    }

    json::Value readString(uint32_t n)
    {
        json::Value v;
        v.kind = json::Value::Kind::String;
        v.scalar = str(n);
        return v;
    }

    json::Value readExt(uint32_t dataLen)
    {
        int8_t type = static_cast<int8_t>(u8());
        json::Value v;
        if (type == 10) // function reference
        {
            v.kind = json::Value::Kind::FuncRef;
            v.scalar = str(dataLen);
        }
        else
        {
            if (p + dataLen > end) throw std::runtime_error("msgpack: truncated");
            p += dataLen;
        }
        return v;
    }

    json::Value read()
    {
        uint8_t b = u8();
        json::Value v;

        if ((b & 0x80) == 0)
        {
            v.kind = json::Value::Kind::Number;
            v.scalar = std::to_string(b);
            return v;
        }
        if ((b & 0xE0) == 0xE0)
        {
            v.kind = json::Value::Kind::Number;
            v.scalar = std::to_string(static_cast<int8_t>(b));
            return v;
        }
        if ((b & 0xE0) == 0xA0) return readString(b & 0x1F);
        if ((b & 0xF0) == 0x80) return readMap(b & 0x0F);
        if ((b & 0xF0) == 0x90) return readArray(b & 0x0F);

        switch (b) {
        case 0xC0: v.kind = json::Value::Kind::Null; return v;
        case 0xC2: v.kind = json::Value::Kind::Bool; v.scalar = "false"; return v;
        case 0xC3: v.kind = json::Value::Kind::Bool; v.scalar = "true"; return v;

        case 0xCA: {
            uint32_t bits = u32();
            float f; memcpy(&f, &bits, 4);
            char buf[32]; snprintf(buf, sizeof(buf), "%g", static_cast<double>(f));
            v.kind = json::Value::Kind::Number; v.scalar = buf; return v;
        }
        case 0xCB: {
            uint64_t bits = u64();
            double d; memcpy(&d, &bits, 8);
            char buf[32]; snprintf(buf, sizeof(buf), "%g", d);
            v.kind = json::Value::Kind::Number; v.scalar = buf; return v;
        }

        case 0xCC: v.kind = json::Value::Kind::Number; v.scalar = std::to_string(u8()); return v;
        case 0xCD: v.kind = json::Value::Kind::Number; v.scalar = std::to_string(u16()); return v;
        case 0xCE: v.kind = json::Value::Kind::Number; v.scalar = std::to_string(u32()); return v;
        case 0xCF: v.kind = json::Value::Kind::Number; v.scalar = std::to_string(u64()); return v;

        case 0xD0: v.kind = json::Value::Kind::Number; v.scalar = std::to_string(static_cast<int8_t> (u8())); return v;
        case 0xD1: v.kind = json::Value::Kind::Number; v.scalar = std::to_string(static_cast<int16_t>(u16())); return v;
        case 0xD2: v.kind = json::Value::Kind::Number; v.scalar = std::to_string(static_cast<int32_t>(u32())); return v;
        case 0xD3: v.kind = json::Value::Kind::Number; v.scalar = std::to_string(static_cast<int64_t>(u64())); return v;

        case 0xD9: return readString(u8());
        case 0xDA: return readString(u16());
        case 0xDB: return readString(u32());

        case 0xDC: return readArray(u16());
        case 0xDD: return readArray(u32());

        case 0xDE: return readMap(u16());
        case 0xDF: return readMap(u32());

        case 0xC7: return readExt(u8());
        case 0xC8: return readExt(u16());
        case 0xC9: return readExt(u32());
        case 0xD4: return readExt(1);
        case 0xD5: return readExt(2);
        case 0xD6: return readExt(4);
        case 0xD7: return readExt(8);
        case 0xD8: return readExt(16);

        default:
            v.kind = json::Value::Kind::Null;
            return v;
        }
    }
};

}

inline json::Value decode(const char* data, uint32_t size)
{
    try
    {
        detail::Reader r{ reinterpret_cast<const uint8_t*>(data), reinterpret_cast<const uint8_t*>(data) + size };
        return r.read();
    }
    catch (...)
    {
        return {};
    }
}

namespace detail
{

inline void writeU8 (std::vector<uint8_t>& b, uint8_t v)  { b.push_back(v); }

inline void writeU16(std::vector<uint8_t>& b, uint16_t v)
{
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
}

inline void writeU32(std::vector<uint8_t>& b, uint32_t v)
{
    b.push_back(static_cast<uint8_t>(v >> 24));
    b.push_back(static_cast<uint8_t>(v >> 16));
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
}

inline void writeStr(std::vector<uint8_t>& b, const std::string& s)
{
    size_t n = s.size();
    if (n <= 31)
    {
        b.push_back(static_cast<uint8_t>(0xA0 | n));
    }
    else if (n <= 255)
    {
        b.push_back(0xD9);
        b.push_back(static_cast<uint8_t>(n));
    }
    else
    {
        b.push_back(0xDA);
        writeU16(b, static_cast<uint16_t>(n));
    }
    b.insert(b.end(), s.begin(), s.end());
}

inline void writeValue(std::vector<uint8_t>& b, const json::Value& v)
{
    switch (v.kind)
    {
    case json::Value::Kind::Null:
        b.push_back(0xC0);
        break;

    case json::Value::Kind::Bool:
        b.push_back(v.scalar == "true" ? 0xC3 : 0xC2);
        break;

    case json::Value::Kind::Number: {
        char* end = nullptr;
        long long ival = std::strtoll(v.scalar.c_str(), &end, 10);
        if (end && *end == '\0')
        {
            if (ival >= 0 && ival <= 127)
            {
                b.push_back(static_cast<uint8_t>(ival));
            }
            else if (ival < 0 && ival >= -32)
            {
                b.push_back(static_cast<uint8_t>(ival));
            }
            else if (ival >= -128 && ival <= 127)
            {
                b.push_back(0xD0); b.push_back(static_cast<uint8_t>(ival));
            }
            else if (ival >= -32768 && ival <= 32767)
            {
                b.push_back(0xD1); writeU16(b, static_cast<uint16_t>(static_cast<int16_t>(ival)));
            }
            else
            {
                b.push_back(0xD2); writeU32(b, static_cast<uint32_t>(static_cast<int32_t>(ival)));
            }
        }
        else
        {
            double d = std::strtod(v.scalar.c_str(), nullptr);
            uint64_t bits; memcpy(&bits, &d, 8);
            b.push_back(0xCB);
            b.push_back(static_cast<uint8_t>(bits >> 56));
            b.push_back(static_cast<uint8_t>(bits >> 48));
            b.push_back(static_cast<uint8_t>(bits >> 40));
            b.push_back(static_cast<uint8_t>(bits >> 32));
            b.push_back(static_cast<uint8_t>(bits >> 24));
            b.push_back(static_cast<uint8_t>(bits >> 16));
            b.push_back(static_cast<uint8_t>(bits >> 8));
            b.push_back(static_cast<uint8_t>(bits));
        }
        break;
    }

    case json::Value::Kind::String:
        writeStr(b, v.scalar);
        break;

    case json::Value::Kind::Array: {
        size_t n = v.children.size();
        if (n <= 15)
            b.push_back(static_cast<uint8_t>(0x90 | n));
        else
        {
            b.push_back(0xDC);
            writeU16(b, static_cast<uint16_t>(n));
        }
        for (auto& child : v.children)
            writeValue(b, child);
        break;
    }

    case json::Value::Kind::Object: {
        size_t n = v.fields.size();
        if (n <= 15)
            b.push_back(static_cast<uint8_t>(0x80 | n));
        else
        {
            b.push_back(0xDE);
            writeU16(b, static_cast<uint16_t>(n));
        }
        for (auto& [key, val] : v.fields)
        {
            writeStr(b, key);
            writeValue(b, val);
        }
        break;
    }

    case json::Value::Kind::FuncRef: {
        size_t n = v.scalar.size();
        if (n <= 255)
        {
            b.push_back(0xC7);
            b.push_back(static_cast<uint8_t>(n));
        }
        else
        {
            b.push_back(0xC8);
            writeU16(b, static_cast<uint16_t>(n));
        }
        b.push_back(10); // function reference ext type
        b.insert(b.end(), v.scalar.begin(), v.scalar.end());
        break;
    }

    default:
        b.push_back(0xC0);
        break;
    }
}

}

inline std::vector<uint8_t> encode(const json::Value& v)
{
    std::vector<uint8_t> buf;
    detail::writeValue(buf, v);
    return buf;
}

inline std::vector<uint8_t> encodeArgs(const std::vector<std::string>& rawJsonArgs)
{
    json::Value arr;
    arr.kind = json::Value::Kind::Array;
    arr.children.reserve(rawJsonArgs.size());
    for (const auto& s : rawJsonArgs)
    {
        try { arr.children.push_back(json::parse(s)); }
        catch (...) {
            json::Value sv;
            sv.kind = json::Value::Kind::String;
            sv.scalar = s;
            arr.children.push_back(sv);
        }
    }
    return encode(arr);
}

}
