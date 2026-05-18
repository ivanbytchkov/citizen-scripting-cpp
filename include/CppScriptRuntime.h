#pragma once

#include <cstdint>
#include <string>
#include <string_view>

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

inline constexpr uint8_t MSGPACK_EMPTY_ARRAY = 0x90;

namespace fx
{

struct ProcessResult
{
        int32_t status; // bytes written on success, -1 permission denied, -2 error, -3 timeout
        std::string output;
};

}

#ifdef __wasm__

#include <bit>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <coroutine>
#include <memory>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <vector>
#include <functional>

namespace fxw_internal
{

inline constexpr size_t MAX_TIMERS = 8192;
inline constexpr size_t MAX_COROUTINES = 4096;

struct Value
{
        enum class Kind
        {
                Null,
                Bool,
                Number,
                String,
                Array,
                Object,
                FuncRef
        } kind = Kind::Null;
        std::string scalar; // String, FuncRef
        double numVal = 0.0;
        bool boolVal = false;
        bool isIntegral = false;
        std::vector<Value> children;
        std::vector<std::string> keys;
        Value() = default;
        Value(Value&&) noexcept = default;
        Value& operator=(Value&&) noexcept = default;
        Value(const Value&) = default;
        Value& operator=(const Value&) = default;
        Value(int v) : kind(Kind::Number), numVal(v), isIntegral(true)
        {
        }
        Value(int64_t v) : kind(Kind::Number), numVal(static_cast<double>(v)), isIntegral(true)
        {
        }
        Value(double v) : kind(Kind::Number), numVal(v)
        {
        }
        Value(float v) : kind(Kind::Number), numVal(static_cast<double>(v))
        {
        }
        Value(bool b) : kind(Kind::Bool), boolVal(b)
        {
        }
        Value(const char* s) : kind(s ? Kind::String : Kind::Null), scalar(s ? s : "")
        {
        }
        Value(std::string s) : kind(Kind::String), scalar(std::move(s))
        {
        }
        std::string asStr(std::string_view def = "") const
        {
                return kind == Kind::String ? scalar : std::string(def);
        }
        double asNum(double def = 0.0) const
        {
                return kind == Kind::Number ? numVal : def;
        }
        int asInt(int def = 0) const
        {
                return kind == Kind::Number ? static_cast<int>(numVal) : def;
        }
        float asFloat(float def = 0.f) const
        {
                return kind == Kind::Number ? static_cast<float>(numVal) : def;
        }
        bool asBool(bool def = false) const
        {
                return kind == Kind::Bool ? boolVal : def;
        }
        bool isNull() const
        {
                return kind == Kind::Null;
        }
        size_t size() const
        {
                return children.size();
        }
        static const Value& nullValue()
        {
                static const Value nil;
                return nil;
        }
        const Value& at(size_t i) const
        {
                return i < children.size() ? children[i] : nullValue();
        }
        bool has(const std::string& key) const
        {
                for (size_t i = 0; i < keys.size(); ++i)
                        if (keys[i] == key)
                                return true;
                return false;
        }
        const Value& operator[](const std::string& key) const
        {
                for (size_t i = 0; i < keys.size(); ++i)
                        if (keys[i] == key)
                                return children[i];
                return nullValue();
        }
};

struct Reader
{
        const uint8_t* p;
        const uint8_t* end;
        bool error = false;
        uint8_t u8()
        {
                if (p >= end)
                {
                        error = true;
                        return 0;
                }
                return *p++;
        }
        uint16_t u16()
        {
                uint8_t a = u8(), b = u8();
                return (uint16_t)(a << 8 | b);
        }
        uint32_t u32()
        {
                uint16_t a = u16(), b = u16();
                return (uint32_t)(a << 16 | b);
        }
        uint64_t u64()
        {
                uint32_t a = u32(), b = u32();
                return (uint64_t)a << 32 | b;
        }
        std::string str(uint32_t n)
        {
                if (p + n > end)
                {
                        error = true;
                        n = (uint32_t)(end - p);
                }
                std::string s((const char*)p, n);
                p += n;
                return s;
        }
        Value readMap(uint32_t n, int d)
        {
                if (n == 1)
                {
                        auto key = read(d + 1);
                        auto val = read(d + 1);
                        if (key.kind == Value::Kind::String && key.scalar == "__cfx_functionReference")
                        {
                                Value v;
                                v.kind = Value::Kind::FuncRef;
                                v.scalar = val.asStr();
                                return v;
                        }
                        Value v;
                        v.kind = Value::Kind::Object;
                        v.keys.push_back(std::move(key.scalar));
                        v.children.push_back(std::move(val));
                        return v;
                }
                Value v;
                v.kind = Value::Kind::Object;
                for (uint32_t i = 0; i < n; ++i)
                {
                        auto key = read(d + 1);
                        auto val = read(d + 1);
                        v.keys.push_back(key.kind == Value::Kind::String ? std::move(key.scalar) : std::to_string(i));
                        v.children.push_back(std::move(val));
                }
                return v;
        }
        Value readExt(uint32_t n)
        {
                uint8_t t = u8();
                if (t == 10)
                {
                        Value v;
                        v.kind = Value::Kind::FuncRef;
                        v.scalar = str(n);
                        return v;
                }
                p += n;
                return { };
        }
        Value read(int d = 0)
        {
                if (d > 64 || p >= end || error)
                        return { };
                uint8_t b = u8();
                Value v;
                if ((b & 0x80) == 0)
                {
                        v.kind = Value::Kind::Number;
                        v.numVal = b;
                        v.isIntegral = true;
                        return v;
                }
                if ((b & 0xE0) == 0xE0)
                {
                        v.kind = Value::Kind::Number;
                        v.numVal = static_cast<int8_t>(b);
                        v.isIntegral = true;
                        return v;
                }
                if ((b & 0xE0) == 0xA0)
                {
                        v.kind = Value::Kind::String;
                        v.scalar = str(b & 0x1F);
                        return v;
                }
                if ((b & 0xF0) == 0x90)
                {
                        v.kind = Value::Kind::Array;
                        uint32_t n = b & 0x0F;
                        for (uint32_t i = 0; i < n; ++i)
                                v.children.push_back(read(d + 1));
                        return v;
                }
                if ((b & 0xF0) == 0x80)
                        return readMap(b & 0x0F, d);
                switch (b)
                {
                        case 0xC0:
                                return { };
                        case 0xC2:
                                v.kind = Value::Kind::Bool;
                                v.boolVal = false;
                                return v;
                        case 0xC3:
                                v.kind = Value::Kind::Bool;
                                v.boolVal = true;
                                return v;
                        case 0xC4:
                        case 0xD9:
                        {
                                v.kind = Value::Kind::String;
                                v.scalar = str(u8());
                                return v;
                        }
                        case 0xC5:
                        case 0xDA:
                        {
                                v.kind = Value::Kind::String;
                                v.scalar = str(u16());
                                return v;
                        }
                        case 0xC6:
                        case 0xDB:
                        {
                                v.kind = Value::Kind::String;
                                v.scalar = str(u32());
                                return v;
                        }
                        case 0xCC:
                                v.kind = Value::Kind::Number;
                                v.numVal = u8();
                                v.isIntegral = true;
                                return v;
                        case 0xCD:
                                v.kind = Value::Kind::Number;
                                v.numVal = u16();
                                v.isIntegral = true;
                                return v;
                        case 0xCE:
                                v.kind = Value::Kind::Number;
                                v.numVal = u32();
                                v.isIntegral = true;
                                return v;
                        case 0xCF:
                                v.kind = Value::Kind::Number;
                                v.numVal = static_cast<double>(u64());
                                v.isIntegral = true;
                                return v;
                        case 0xD0:
                                v.kind = Value::Kind::Number;
                                v.numVal = static_cast<int8_t>(u8());
                                v.isIntegral = true;
                                return v;
                        case 0xD1:
                                v.kind = Value::Kind::Number;
                                v.numVal = static_cast<int16_t>(u16());
                                v.isIntegral = true;
                                return v;
                        case 0xD2:
                                v.kind = Value::Kind::Number;
                                v.numVal = static_cast<int32_t>(u32());
                                v.isIntegral = true;
                                return v;
                        case 0xD3:
                                v.kind = Value::Kind::Number;
                                v.numVal = static_cast<double>(static_cast<int64_t>(u64()));
                                v.isIntegral = true;
                                return v;
                        case 0xCA:
                                v.kind = Value::Kind::Number;
                                v.numVal = static_cast<double>(std::bit_cast<float>(u32()));
                                return v;
                        case 0xCB:
                                v.kind = Value::Kind::Number;
                                v.numVal = std::bit_cast<double>(u64());
                                return v;
                        case 0xDC:
                                v.kind = Value::Kind::Array;
                                for (uint32_t i = 0, n = u16(); i < n; ++i)
                                        v.children.push_back(read(d + 1));
                                return v;
                        case 0xDD:
                                v.kind = Value::Kind::Array;
                                for (uint32_t i = 0, n = u32(); i < n; ++i)
                                        v.children.push_back(read(d + 1));
                                return v;
                        // ext8/ext16/ext32
                        case 0xC7:
                                return readExt(u8());
                        case 0xC8:
                                return readExt(u16());
                        case 0xC9:
                                return readExt(u32());
                        // map16/map32
                        case 0xDE:
                                return readMap(u16(), d);
                        case 0xDF:
                                return readMap(u32(), d);
                        default:
                                return { };
                }
        }
};

inline Value decode(const void* data, uint32_t size)
{
        Reader r{ (const uint8_t*)data, (const uint8_t*)data + size, false };
        auto v = r.read();
        return r.error ? Value{ } : v;
}

inline void ensureArray(Value& v)
{
        if (v.kind != Value::Kind::Array)
        {
                Value arr;
                arr.kind = Value::Kind::Array;
                arr.children.push_back(std::move(v));
                v = std::move(arr);
        }
}

struct Writer
{
        std::vector<uint8_t> buf;
        void pu8(uint8_t v)
        {
                buf.push_back(v);
        }
        void pu16(uint16_t v)
        {
                buf.push_back(v >> 8);
                buf.push_back(v & 0xFF);
        }
        void pu32(uint32_t v)
        {
                pu16(v >> 16);
                pu16(v & 0xFFFF);
        }
        void str(std::string_view s)
        {
                uint32_t n = (uint32_t)s.size();
                if (n <= 31)
                        pu8(0xA0 | (uint8_t)n);
                else if (n <= 255)
                {
                        pu8(0xD9);
                        pu8((uint8_t)n);
                }
                else if (n <= 65535)
                {
                        pu8(0xDA);
                        pu16((uint16_t)n);
                }
                else
                {
                        pu8(0xDB);
                        pu32(n);
                }
                buf.insert(buf.end(), s.begin(), s.end());
        }
        void arrayHeader(uint32_t n)
        {
                if (n <= 15)
                        pu8(0x90 | (uint8_t)n);
                else if (n <= 65535)
                {
                        pu8(0xDC);
                        pu16((uint16_t)n);
                }
                else
                {
                        pu8(0xDD);
                        pu32(n);
                }
        }
        void mapHeader(uint32_t n)
        {
                if (n <= 15)
                        pu8(0x80 | (uint8_t)n);
                else if (n <= 65535)
                {
                        pu8(0xDE);
                        pu16((uint16_t)n);
                }
                else
                {
                        pu8(0xDF);
                        pu32(n);
                }
        }
        void encNull()
        {
                pu8(0xC0);
        }
        void encBool(bool v)
        {
                pu8(v ? 0xC3 : 0xC2);
        }
        void encInt(int64_t v)
        {
                if (v >= 0 && v <= 127)
                {
                        pu8((uint8_t)v);
                }
                else if (v < 0 && v >= -32)
                {
                        pu8((uint8_t)(int8_t)v);
                }
                else if (v >= -128 && v <= 127)
                {
                        pu8(0xD0);
                        pu8((uint8_t)(int8_t)v);
                }
                else if (v >= -32768 && v <= 32767)
                {
                        pu8(0xD1);
                        pu16((uint16_t)(int16_t)v);
                }
                else if (v >= -2147483648LL && v <= 2147483647LL)
                {
                        pu8(0xD2);
                        pu32((uint32_t)(int32_t)v);
                }
                else
                {
                        pu8(0xD3);
                        pu32((uint32_t)((uint64_t)v >> 32));
                        pu32((uint32_t)v);
                }
        }
        void encDouble(double d)
        {
                uint64_t bits;
                memcpy(&bits, &d, 8);
                pu8(0xCB);
                for (int i = 7; i >= 0; --i)
                        pu8((bits >> (i * 8)) & 0xFF);
        }
        void encValue(const Value& v)
        {
                switch (v.kind)
                {
                        case Value::Kind::Null:
                                encNull();
                                break;
                        case Value::Kind::Bool:
                                encBool(v.boolVal);
                                break;
                        case Value::Kind::Number:
                                if (v.isIntegral)
                                        encInt(static_cast<int64_t>(v.numVal));
                                else
                                        encDouble(v.numVal);
                                break;
                        case Value::Kind::String:
                                str(v.scalar);
                                break;
                        case Value::Kind::FuncRef:
                        {
                                uint32_t n = (uint32_t)v.scalar.size();
                                if (n <= 255)
                                {
                                        pu8(0xC7);
                                        pu8((uint8_t)n);
                                }
                                else if (n <= 65535)
                                {
                                        pu8(0xC8);
                                        pu16((uint16_t)n);
                                }
                                else
                                {
                                        pu8(0xC9);
                                        pu32(n);
                                }
                                pu8(10);
                                buf.insert(buf.end(), v.scalar.begin(), v.scalar.end());
                                break;
                        }
                        case Value::Kind::Array:
                                arrayHeader(static_cast<uint32_t>(v.children.size()));
                                for (auto& c : v.children)
                                        encValue(c);
                                break;
                        case Value::Kind::Object:
                                mapHeader(static_cast<uint32_t>(v.keys.size()));
                                for (size_t i = 0; i < v.keys.size(); ++i)
                                {
                                        str(v.keys[i]);
                                        if (i < v.children.size())
                                                encValue(v.children[i]);
                                        else
                                                encNull();
                                }
                                break;
                }
        }
};

inline std::vector<uint8_t> encode(const Value& v)
{
        Writer w;
        w.encValue(v);
        return std::move(w.buf);
}

template<typename T>
inline void encodeOne(Writer& w, T&& v)
{
        using D = std::decay_t<T>;
        if constexpr (std::is_same_v<D, std::string> || std::is_same_v<D, std::string_view> || std::is_same_v<D, const char*> || std::is_same_v<D, char*>)
                w.str(std::string_view(v));
        else if constexpr (std::is_same_v<D, bool>)
                w.encBool(v);
        else if constexpr (std::is_same_v<D, Value>)
                w.encValue(v);
        else if constexpr (std::is_floating_point_v<D>)
                w.encDouble((double)v);
        else if constexpr (std::is_integral_v<D>)
                w.encInt((int64_t)v);
        else
                w.encNull();
}

using RefCallbackFn = std::function<std::vector<uint8_t>(const uint8_t*, uint32_t)>;

}

namespace fx::json
{

using Value = fxw_internal::Value;
inline Value makeNull()
{
        return { };
}

inline void ensureArray(Value& v)
{
        fxw_internal::ensureArray(v);
}

inline std::string quote(std::string_view s)
{
        std::string out;
        out.reserve(s.size() + 2);
        out += '"';
        for (char c : s)
        {
                switch (c)
                {
                        case '"':
                                out += "\\\"";
                                break;
                        case '\\':
                                out += "\\\\";
                                break;
                        case '\n':
                                out += "\\n";
                                break;
                        case '\r':
                                out += "\\r";
                                break;
                        case '\t':
                                out += "\\t";
                                break;
                        default:
                                if (static_cast<unsigned char>(c) < 0x20)
                                {
                                        char buf[8];
                                        snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                                        out += buf;
                                }
                                else
                                        out += c;
                }
        }
        out += '"';
        return out;
}

class JsonObj
{
    public:
        JsonObj& set(std::string_view key, std::string_view value)
        {
                append(key, quote(value));
                return *this;
        }
        JsonObj& set(std::string_view key, int64_t value)
        {
                append(key, std::to_string(value));
                return *this;
        }
        JsonObj& set(std::string_view key, int value)
        {
                return set(key, static_cast<int64_t>(value));
        }
        JsonObj& set(std::string_view key, uint32_t v)
        {
                return set(key, static_cast<int64_t>(v));
        }
        JsonObj& set(std::string_view key, uint64_t v)
        {
                char buf[21];
                snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
                append(key, buf);
                return *this;
        }
        JsonObj& set(std::string_view key, double value)
        {
                char buf[32];
                snprintf(buf, sizeof(buf), "%g", value);
                append(key, buf);
                return *this;
        }
        JsonObj& set(std::string_view key, bool value)
        {
                append(key, value ? "true" : "false");
                return *this;
        }
        JsonObj& setRaw(std::string_view key, std::string_view rawValue)
        {
                append(key, std::string(rawValue));
                return *this;
        }
        std::string build() const
        {
                return "{" + m_body + "}";
        }

    private:
        void append(std::string_view key, const std::string& rawVal)
        {
                if (!m_body.empty())
                        m_body += ',';
                m_body += quote(key);
                m_body += ':';
                m_body += rawVal;
        }
        std::string m_body;
};

namespace detail
{
        struct Parser
        {
                std::string_view src;
                size_t pos = 0;
                bool error = false;
                char peek() const
                {
                        return pos < src.size() ? src[pos] : '\0';
                }
                char consume()
                {
                        return pos < src.size() ? src[pos++] : '\0';
                }
                void skipWs()
                {
                        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n' || src[pos] == '\r'))
                                ++pos;
                }
                bool expect(char c)
                {
                        skipWs();
                        if (consume() != c)
                        {
                                error = true;
                                return false;
                        }
                        return true;
                }
                std::string parseString()
                {
                        if (!expect('"'))
                                return { };
                        std::string out;
                        while (pos < src.size())
                        {
                                char c = consume();
                                if (c == '"')
                                        return out;
                                if (c == '\\')
                                {
                                        char e = consume();
                                        switch (e)
                                        {
                                                case '"':
                                                        out += '"';
                                                        break;
                                                case '\\':
                                                        out += '\\';
                                                        break;
                                                case '/':
                                                        out += '/';
                                                        break;
                                                case 'n':
                                                        out += '\n';
                                                        break;
                                                case 'r':
                                                        out += '\r';
                                                        break;
                                                case 't':
                                                        out += '\t';
                                                        break;
                                                case 'u':
                                                {
                                                        unsigned cp = 0;
                                                        for (int i = 0; i < 4; ++i)
                                                        {
                                                                char h = consume();
                                                                cp <<= 4;
                                                                if (h >= '0' && h <= '9')
                                                                        cp |= h - '0';
                                                                else if (h >= 'a' && h <= 'f')
                                                                        cp |= h - 'a' + 10;
                                                                else if (h >= 'A' && h <= 'F')
                                                                        cp |= h - 'A' + 10;
                                                        }
                                                        if (cp >= 0xD800 && cp <= 0xDBFF)
                                                        {
                                                                if (consume() == '\\' && consume() == 'u')
                                                                {
                                                                        unsigned lo = 0;
                                                                        for (int i = 0; i < 4; ++i)
                                                                        {
                                                                                char h = consume();
                                                                                lo <<= 4;
                                                                                if (h >= '0' && h <= '9')
                                                                                        lo |= h - '0';
                                                                                else if (h >= 'a' && h <= 'f')
                                                                                        lo |= h - 'a' + 10;
                                                                                else if (h >= 'A' && h <= 'F')
                                                                                        lo |= h - 'A' + 10;
                                                                        }
                                                                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                                                                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                                                }
                                                        }
                                                        if (cp < 0x80)
                                                                out += static_cast<char>(cp);
                                                        else if (cp < 0x800)
                                                        {
                                                                out += static_cast<char>(0xC0 | (cp >> 6));
                                                                out += static_cast<char>(0x80 | (cp & 0x3F));
                                                        }
                                                        else if (cp < 0x10000)
                                                        {
                                                                out += static_cast<char>(0xE0 | (cp >> 12));
                                                                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                                                                out += static_cast<char>(0x80 | (cp & 0x3F));
                                                        }
                                                        else
                                                        {
                                                                out += static_cast<char>(0xF0 | (cp >> 18));
                                                                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                                                                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                                                                out += static_cast<char>(0x80 | (cp & 0x3F));
                                                        }
                                                        break;
                                                }
                                                default:
                                                        out += e;
                                        }
                                }
                                else
                                        out += c;
                        }
                        error = true;
                        return out;
                }
                static constexpr int MAX_DEPTH = 128;
                Value parseValue(int depth = 0)
                {
                        if (depth > MAX_DEPTH || error)
                                return { };
                        skipWs();
                        Value v;
                        char c = peek();
                        if (c == '"')
                        {
                                v.kind = Value::Kind::String;
                                v.scalar = parseString();
                        }
                        else if (c == '{')
                        {
                                v.kind = Value::Kind::Object;
                                consume();
                                skipWs();
                                if (peek() == '}')
                                {
                                        consume();
                                        return v;
                                }
                                while (!error)
                                {
                                        skipWs();
                                        std::string key = parseString();
                                        if (error)
                                                break;
                                        skipWs();
                                        if (!expect(':'))
                                                break;
                                        v.keys.push_back(std::move(key));
                                        v.children.push_back(parseValue(depth + 1));
                                        if (error)
                                                break;
                                        skipWs();
                                        char sep = peek();
                                        if (sep == ',')
                                                consume();
                                        else if (sep == '}')
                                        {
                                                consume();
                                                break;
                                        }
                                        else
                                        {
                                                error = true;
                                                break;
                                        }
                                }
                        }
                        else if (c == '[')
                        {
                                v.kind = Value::Kind::Array;
                                consume();
                                skipWs();
                                if (peek() == ']')
                                {
                                        consume();
                                        return v;
                                }
                                while (!error)
                                {
                                        v.children.push_back(parseValue(depth + 1));
                                        if (error)
                                                break;
                                        skipWs();
                                        char sep = peek();
                                        if (sep == ',')
                                                consume();
                                        else if (sep == ']')
                                        {
                                                consume();
                                                break;
                                        }
                                        else
                                        {
                                                error = true;
                                                break;
                                        }
                                }
                        }
                        else if (c == 't')
                        {
                                if (pos + 4 > src.size() || src.compare(pos, 4, "true") != 0)
                                {
                                        error = true;
                                        return { };
                                }
                                v.kind = Value::Kind::Bool;
                                v.boolVal = true;
                                pos += 4;
                        }
                        else if (c == 'f')
                        {
                                if (pos + 5 > src.size() || src.compare(pos, 5, "false") != 0)
                                {
                                        error = true;
                                        return { };
                                }
                                v.kind = Value::Kind::Bool;
                                v.boolVal = false;
                                pos += 5;
                        }
                        else if (c == 'n')
                        {
                                if (pos + 4 > src.size() || src.compare(pos, 4, "null") != 0)
                                {
                                        error = true;
                                        return { };
                                }
                                v.kind = Value::Kind::Null;
                                pos += 4;
                        }
                        else
                        {
                                v.kind = Value::Kind::Number;
                                size_t start = pos;
                                bool hasDot = false, hasExp = false;
                                if (peek() == '-')
                                        ++pos;
                                while (pos < src.size() && (src[pos] >= '0' && src[pos] <= '9'))
                                        ++pos;
                                if (pos < src.size() && src[pos] == '.')
                                {
                                        hasDot = true;
                                        ++pos;
                                        while (pos < src.size() && (src[pos] >= '0' && src[pos] <= '9'))
                                                ++pos;
                                }
                                if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E'))
                                {
                                        hasExp = true;
                                        ++pos;
                                        if (pos < src.size() && (src[pos] == '+' || src[pos] == '-'))
                                                ++pos;
                                        while (pos < src.size() && (src[pos] >= '0' && src[pos] <= '9'))
                                                ++pos;
                                }
                                if (pos == start)
                                {
                                        error = true;
                                        return { };
                                }
                                std::string numStr(src.data() + start, pos - start);
                                v.numVal = std::strtod(numStr.c_str(), nullptr);
                                v.isIntegral = !hasDot && !hasExp;
                        }
                        return v;
                }
        };

}

inline Value parse(std::string_view json)
{
        detail::Parser p{ json, 0, false };
        auto v = p.parseValue();
        return p.error ? Value{ } : v;
}

}

namespace fx
{

struct NativeCtx
{
        uint64_t hash;
        uint32_t numArgs;
        uint32_t numResults;
        uint64_t args[32];
        uint32_t ptrMask;
        uint32_t resultPtrMask;
};
static_assert(sizeof(NativeCtx) == 280);

struct NativeArg
{
        uint64_t value = 0;
        bool isPtr = false;
        NativeArg() = default;
        NativeArg(int32_t v) : value((uint64_t)(uint32_t)v), isPtr(false)
        {
        }
        NativeArg(uint32_t v) : value((uint64_t)v), isPtr(false)
        {
        }
        NativeArg(int64_t v) : value((uint64_t)v), isPtr(false)
        {
        }
        NativeArg(uint64_t v) : value(v), isPtr(false)
        {
        }
        NativeArg(bool v) : value(v ? 1 : 0), isPtr(false)
        {
        }
        NativeArg(float v)
        {
                uint32_t b;
                memcpy(&b, &v, 4);
                value = b;
                isPtr = false;
        }
        NativeArg(double v)
        {
                uint64_t b;
                memcpy(&b, &v, 8);
                value = b;
                isPtr = false;
        }
        static NativeArg ptr(const void* p)
        {
                NativeArg a;
                a.value = (uint64_t)(uintptr_t)p;
                a.isPtr = true;
                return a;
        }
        static NativeArg ptr(void* p)
        {
                return ptr((const void*)p);
        }
};

struct Vector3
{
        float x = 0.f, y = 0.f, z = 0.f;
};

class EventArgs
{
    public:
        explicit EventArgs(fxw_internal::Value arr) : m_arr(std::move(arr))
        {
        }
        size_t size() const
        {
                return m_arr.size();
        }
        std::string str(size_t i) const
        {
                return m_arr.at(i).asStr();
        }
        int integer(size_t i) const
        {
                return m_arr.at(i).asInt();
        }
        double number(size_t i) const
        {
                return m_arr.at(i).asNum();
        }
        float floating(size_t i) const
        {
                return m_arr.at(i).asFloat();
        }
        bool boolean(size_t i) const
        {
                return m_arr.at(i).asBool();
        }
        bool isNull(size_t i) const
        {
                return m_arr.at(i).isNull();
        }
        std::string funcRef(size_t i) const
        {
                const auto& v = m_arr.at(i);
                return v.kind == fxw_internal::Value::Kind::FuncRef ? v.scalar : std::string{ };
        }
        template<typename T>
        T get(size_t i) const;

    private:
        fxw_internal::Value m_arr;
};

template<>
inline std::string EventArgs::get<std::string>(size_t i) const
{
        return str(i);
}

template<>
inline int EventArgs::get<int>(size_t i) const
{
        return integer(i);
}

template<>
inline double EventArgs::get<double>(size_t i) const
{
        return number(i);
}

template<>
inline float EventArgs::get<float>(size_t i) const
{
        return floating(i);
}

template<>
inline bool EventArgs::get<bool>(size_t i) const
{
        return boolean(i);
}

using EventHandler = std::function<void(const std::string& source, const EventArgs&)>;
using TickHandler = std::function<void()>;
using StopHandler = std::function<void()>;
using CommandHandler = std::function<void(const std::string& source, const std::vector<std::string>& args)>;
using ExportHandler = std::function<json::Value(const EventArgs&)>;
using StateBagChangeHandler = std::function<void(const std::string& bagName, const std::string& key, const json::Value& value, int source, bool replicated)>;
using HttpCallback = std::function<void(int statusCode, const std::string& body, const std::string& headers)>;

}

extern "C" {
        __attribute__((import_module("fxcpp"), import_name("trace"))) void __fxcppTrace(const char* ptr, uint32_t len);
        __attribute__((import_module("fxcpp"), import_name("invoke_native"))) void __fxcppInvokeNative(uint32_t ctx_ptr);
        __attribute__((import_module("fxcpp"), import_name("copy_string_result"))) int32_t __fxcppCopyStringResult(uint32_t ctx_ptr, int32_t result_idx, char* buf, int32_t buf_max);
        __attribute__((import_module("fxcpp"), import_name("emit_event"))) void __fxcppEmitEvent(const char* name, uint32_t name_len, const uint8_t* args, uint32_t args_len);
        __attribute__((import_module("fxcpp"), import_name("emit_net_event"))) void __fxcppEmitNetEvent(const char* name, uint32_t name_len, int32_t target, const uint8_t* args, uint32_t args_len);
        __attribute__((import_module("fxcpp"), import_name("cancel_event"))) void __fxcppCancelEvent();
        __attribute__((import_module("fxcpp"), import_name("was_event_canceled"))) int32_t __fxcppWasEventCanceled();
        __attribute__((import_module("fxcpp"), import_name("get_resource_metadata"))) int32_t __fxcppGetResourceMetadata(const char* key, uint32_t key_len, int32_t index, char* buf, int32_t buf_max);
        __attribute__((import_module("fxcpp"), import_name("get_num_resource_metadata"))) int32_t __fxcppGetNumResourceMetadata(const char* key, uint32_t key_len);
        __attribute__((import_module("fxcpp"), import_name("create_ref"))) int32_t __fxcppCreateRef(int32_t callback_id);
        __attribute__((import_module("fxcpp"), import_name("canonicalize_ref"))) int32_t __fxcppCanonicalizeRef(int32_t ref_idx, char* buf, int32_t buf_max);
        __attribute__((import_module("fxcpp"), import_name("remove_ref"))) void __fxcppRemoveRef(int32_t ref_idx);
        __attribute__((import_module("fxcpp"), import_name("invoke_function_reference"))) void __fxcppInvokeFunctionReference(const char* ref_str, uint32_t ref_len, const char* args, uint32_t args_len, void* out);
        __attribute__((import_module("fxcpp"), import_name("get_instance_id"))) int32_t __fxcppGetInstanceId();
        __attribute__((import_module("fxcpp"), import_name("spawn_process"))) int32_t __fxcppSpawnProcess(const char* cmd, uint32_t cmd_len, char* out_buf, int32_t out_buf_max);
        __attribute__((import_module("fxcpp"), import_name("create_worker"))) int32_t __fxcppCreateWorker(const char* fn_name, uint32_t fn_name_len, const char* input, uint32_t input_len);
        __attribute__((import_module("fxcpp"), import_name("poll_worker"))) int32_t __fxcppPollWorker(int32_t worker_id, char* out_buf, int32_t out_buf_max);
        __attribute__((import_module("fxcpp"), import_name("schedule_bookmark"))) void __fxcppScheduleBookmark(int32_t bookmark_id, int32_t deadline_ms);
        __attribute__((import_module("fxcpp"), import_name("is_manifest_version_v2_between"))) int32_t __fxcppIsManifestVersionV2Between(const char* lower, uint32_t lower_len, const char* upper, uint32_t upper_len);
}

namespace fxw_internal
{

struct TimerEntry
{
        int32_t id;
        std::chrono::steady_clock::time_point nextFire;
        uint32_t intervalMs;
        std::function<void()> callback;
};

struct EventHandlerEntry
{
        int32_t id;
        fx::EventHandler handler;
};

struct Context
{
        std::string resourceName;
        std::vector<fx::TickHandler> ticks;
        std::vector<fx::StopHandler> stops;
        std::unordered_map<std::string, std::vector<EventHandlerEntry>> events;
        std::unordered_map<int32_t, std::string> handlerEventMap;
        int32_t nextEventHandlerId = 1;
        std::unordered_map<std::string, std::vector<fx::CommandHandler>> commands;
        std::unordered_set<std::string> netSafeEvents;
        std::unordered_map<int32_t, TimerEntry> timers;
        int32_t nextTimerId = 1;
        std::unordered_map<int32_t, int32_t> stateBagHandlerRefs; // cookie -> hostRef
        std::unordered_map<int32_t, fx::HttpCallback> httpCallbacks;
        bool httpResponseRegistered = false;
        int32_t addTimer(uint32_t ms, uint32_t intervalMs, std::function<void()> cb)
        {
                if (timers.size() >= MAX_TIMERS)
                        return -1;
                int32_t id = nextTimerId;
                int32_t start = id;
                while (timers.count(id))
                {
                        if (++id <= 0)
                                id = 1;
                        if (id == start)
                                return -1;
                }
                nextTimerId = id + 1;
                if (nextTimerId <= 0)
                        nextTimerId = 1;
                auto fire = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
                timers[id] = { id, fire, intervalMs, std::move(cb) };
                return id;
        }
        void dispatchTick()
        {
                auto now = std::chrono::steady_clock::now();
                std::vector<int32_t> expired;
                for (auto& [id, t] : timers)
                        if (now >= t.nextFire)
                                expired.push_back(id);
                for (auto id : expired)
                {
                        auto it = timers.find(id);
                        if (it == timers.end())
                                continue;
                        auto cb = it->second.callback;
                        if (it->second.intervalMs > 0)
                        {
                                auto next = it->second.nextFire + std::chrono::milliseconds(it->second.intervalMs);
                                it->second.nextFire = (next > now) ? next : now;
                        }
                        else
                                timers.erase(it);
#if __cpp_exceptions
                        try
                        {
                                cb();
                        }
                        catch (const std::exception& e)
                        {
                                fprintf(stderr, "[script:%s] Unhandled exception in timer %d: %s\n", resourceName.c_str(), id, e.what());
                        }
                        catch (...)
                        {
                                fprintf(stderr, "[script:%s] Unhandled non-standard exception in timer %d\n", resourceName.c_str(), id);
                        }
#else
                        cb();
#endif
                }
                auto tickSnapshot = ticks;
                for (auto& h : tickSnapshot)
                {
#if __cpp_exceptions
                        try
                        {
                                h();
                        }
                        catch (const std::exception& e)
                        {
                                fprintf(stderr, "[script:%s] Unhandled exception in tick handler: %s\n", resourceName.c_str(), e.what());
                        }
                        catch (...)
                        {
                                fprintf(stderr, "[script:%s] Unhandled non-standard exception in tick handler\n", resourceName.c_str());
                        }
#else
                        h();
#endif
                }
        }
        void dispatchStop()
        {
                for (auto& h : stops)
                        h();
        }
        void dispatchEvent(const char* name, uint32_t nameLen, const uint8_t* args, uint32_t argsLen, const char* src, uint32_t srcLen)
        {
                std::string key(name, nameLen);
                auto it = events.find(key);
                if (it == events.end())
                        return;
                std::string srcStr(src, srcLen);
                if (srcStr.size() >= 4 && srcStr.compare(0, 4, "net:") == 0)
                {
                        if (netSafeEvents.find(key) == netSafeEvents.end())
                                return;
                }
                Value decoded = decode(args, argsLen);
                ensureArray(decoded);
                fx::EventArgs ea(std::move(decoded));
                auto handlers = it->second;
                for (auto& entry : handlers)
                {
#if __cpp_exceptions
                        try
                        {
                                entry.handler(srcStr, ea);
                        }
                        catch (const std::exception& e)
                        {
                                fprintf(stderr, "[script:%s] Unhandled exception in event '%s': %s\n", resourceName.c_str(), key.c_str(), e.what());
                        }
                        catch (...)
                        {
                                fprintf(stderr, "[script:%s] Unhandled non-standard exception in event '%s'\n", resourceName.c_str(), key.c_str());
                        }
#else
                        entry.handler(srcStr, ea);
#endif
                        if (__fxcppWasEventCanceled())
                                break;
                }
        }
};

inline Context*& currentContext()
{
        static Context* p = nullptr;
        return p;
}

inline std::unordered_map<int32_t, RefCallbackFn>& refCallbacks()
{
        static std::unordered_map<int32_t, RefCallbackFn> s_map;
        return s_map;
}

inline std::unordered_map<int32_t, int32_t>& refCounts()
{
        static std::unordered_map<int32_t, int32_t> s_map;
        return s_map;
}

inline int32_t& nextCallbackId()
{
        static int32_t s_id = 1;
        return s_id;
}

}

namespace fx::detail
{

inline int32_t createRef(fxw_internal::RefCallbackFn cb)
{
        auto& nextId = fxw_internal::nextCallbackId();
        auto& callbacks = fxw_internal::refCallbacks();
        int32_t id = nextId;
        int32_t start = id;
        while (callbacks.count(id))
        {
                if (++id <= 0)
                        id = 1;
                if (id == start)
                        return -1;
        }
        nextId = id + 1;
        if (nextId <= 0)
                nextId = 1;
        callbacks[id] = std::move(cb);
        fxw_internal::refCounts()[id] = 1;
        int32_t hostRef = __fxcppCreateRef(id);
        return hostRef;
}

inline std::string canonicalizeRef(int32_t refIdx)
{
        int32_t len = __fxcppCanonicalizeRef(refIdx, nullptr, 0);
        if (len <= 0)
                return { };
        std::string out(static_cast<size_t>(len), '\0');
        __fxcppCanonicalizeRef(refIdx, out.data(), len + 1);
        return out;
}

inline void removeRef(int32_t refIdx)
{
        __fxcppRemoveRef(refIdx);
}

inline std::vector<uint8_t> invokeFunctionReference(const std::string& ref, const uint8_t* args, uint32_t argsLen)
{
        struct
        {
                uint32_t ptr;
                uint32_t len;
        } out{ };
        __fxcppInvokeFunctionReference(ref.c_str(), static_cast<uint32_t>(ref.size()), reinterpret_cast<const char*>(args), argsLen, &out);
        if (!out.ptr || !out.len)
                return { };
        auto* data = reinterpret_cast<uint8_t*>(out.ptr);
        std::vector<uint8_t> result(data, data + out.len);
        free(data);
        return result;
}

template<typename T>
struct fn_traits : fn_traits<decltype(&std::decay_t<T>::operator())>
{
};

template<typename C, typename R, typename... Args>
struct fn_traits<R (C::*)(Args...) const>
{
        using tuple_type = std::tuple<Args...>;
};

template<typename C, typename R, typename... Args>
struct fn_traits<R (C::*)(Args...)>
{
        using tuple_type = std::tuple<Args...>;
};

template<typename C, typename R, typename... Args>
struct fn_traits<R (C::*)(Args...) const noexcept>
{
        using tuple_type = std::tuple<Args...>;
};

template<typename C, typename R, typename... Args>
struct fn_traits<R (C::*)(Args...) noexcept>
{
        using tuple_type = std::tuple<Args...>;
};

template<typename R, typename... Args>
struct fn_traits<R (*)(Args...)>
{
        using tuple_type = std::tuple<Args...>;
};

template<typename R, typename... Args>
struct fn_traits<R (*)(Args...) noexcept>
{
        using tuple_type = std::tuple<Args...>;
};

template<typename F, typename ArgsTuple, size_t... Is>
void InvokeTypedHandler(F& fn, const std::string& source, const EventArgs& args, std::index_sequence<Is...>)
{
        fn(source, args.get<std::decay_t<std::tuple_element_t<Is + 1, ArgsTuple>>>(Is)...);
}

template<typename F>
EventHandler WrapTypedHandler(F&& fn)
{
        using args_tuple = typename fn_traits<std::decay_t<F>>::tuple_type;
        constexpr size_t n = std::tuple_size_v<args_tuple>;
        static_assert(n >= 1, "Handler must accept at least source parameter");
        return [f = std::forward<F>(fn)](const std::string& source, const EventArgs& args) mutable
        {
                InvokeTypedHandler<decltype(f), args_tuple>(f, source, args, std::make_index_sequence<n - 1>{ });
        };
}

}

namespace fx
{

inline NativeCtx invokeNative(uint64_t hash, std::initializer_list<NativeArg> args, uint32_t numResults = 0, uint32_t resultPtrMask = 0)
{
        NativeCtx ctx{ };
        ctx.hash = hash;
        ctx.numArgs = static_cast<uint32_t>(args.size());
        ctx.numResults = numResults;
        ctx.resultPtrMask = resultPtrMask;
        uint32_t i = 0;
        for (const auto& a : args)
        {
                ctx.args[i] = a.value;
                if (a.isPtr)
                        ctx.ptrMask |= (1u << i);
                ++i;
        }
        __fxcppInvokeNative(reinterpret_cast<uint32_t>(&ctx));
        return ctx;
}

inline std::string getStringResult(NativeCtx& ctx, int32_t resultIdx)
{
        int32_t len = __fxcppCopyStringResult(
        reinterpret_cast<uint32_t>(&ctx), resultIdx, nullptr, 0);
        if (len <= 0)
                return { };
        std::string out(static_cast<size_t>(len), '\0');
        __fxcppCopyStringResult(
        reinterpret_cast<uint32_t>(&ctx), resultIdx, out.data(), len + 1);
        return out;
}

}

namespace fx::natives
{

namespace detail
{
        inline void pushArgs(NativeCtx&, uint32_t)
        {
        }
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
                        uint32_t bits;
                        memcpy(&bits, &arg, 4);
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
        NativeCtx ctx{ };
        ctx.hash = hash;
        ctx.numArgs = static_cast<uint32_t>(sizeof...(args));
        ctx.numResults = 3;
        detail::pushArgs(ctx, 0, std::forward<TArgs>(args)...);
        __fxcppInvokeNative(reinterpret_cast<uint32_t>(&ctx));
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
                NativeCtx ctx{ };
                ctx.hash = hash;
                ctx.numArgs = static_cast<uint32_t>(sizeof...(args));
                ctx.numResults = 3;
                ctx.resultPtrMask = 0x1;
                detail::pushArgs(ctx, 0, std::forward<TArgs>(args)...);
                __fxcppInvokeNative(reinterpret_cast<uint32_t>(&ctx));
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

namespace fx
{

template<typename F>
inline int32_t on(const std::string& event, F&& handler)
{
        EventHandler h;
        if constexpr (std::is_convertible_v<std::decay_t<F>, EventHandler>)
                h = EventHandler(std::forward<F>(handler));
        else
                h = detail::WrapTypedHandler(std::forward<F>(handler));
        if (auto* c = fxw_internal::currentContext())
        {
                bool first = c->events.find(event) == c->events.end() || c->events[event].empty();
                int32_t token = c->nextEventHandlerId;
                int32_t start = token;
                while (c->handlerEventMap.count(token))
                {
                        if (++token <= 0)
                                token = 1;
                        if (token == start)
                                return -1;
                }
                c->nextEventHandlerId = token + 1;
                if (c->nextEventHandlerId <= 0)
                        c->nextEventHandlerId = 1;
                c->events[event].push_back({ token, std::move(h) });
                c->handlerEventMap[token] = event;
                if (first)
                {
                        NativeCtx ctx{ };
                        ctx.hash = HashString("REGISTER_RESOURCE_AS_EVENT_HANDLER");
                        ctx.numArgs = 1;
                        ctx.args[0] = reinterpret_cast<uint64_t>(event.c_str());
                        ctx.ptrMask = 1;
                        __fxcppInvokeNative(reinterpret_cast<uint32_t>(&ctx));
                }
                return token;
        }
        return -1;
}

template<typename F>
inline int32_t onNet(const std::string& event, F&& handler)
{
        if (auto* c = fxw_internal::currentContext())
                c->netSafeEvents.insert(event);
        return on(event, std::forward<F>(handler));
}

inline void registerNetEvent(const std::string& event)
{
        if (auto* c = fxw_internal::currentContext())
                c->netSafeEvents.insert(event);
}

inline void removeEventHandler(int32_t token)
{
        auto* c = fxw_internal::currentContext();
        if (!c)
                return;
        auto mapIt = c->handlerEventMap.find(token);
        if (mapIt == c->handlerEventMap.end())
                return;
        std::string event = mapIt->second;
        c->handlerEventMap.erase(mapIt);
        auto it = c->events.find(event);
        if (it == c->events.end())
                return;
        auto& vec = it->second;
        vec.erase(std::remove_if(vec.begin(), vec.end(), [token](const fxw_internal::EventHandlerEntry& e)
                  {
                          return e.id == token;
                  }),
        vec.end());
}

template<typename... TArgs>
inline void emit(const std::string& event, TArgs&&... vals)
{
        fxw_internal::Writer w;
        w.arrayHeader(static_cast<uint32_t>(sizeof...(vals)));
        (fxw_internal::encodeOne(w, std::forward<TArgs>(vals)), ...);
        __fxcppEmitEvent(event.c_str(), static_cast<uint32_t>(event.size()), w.buf.data(), static_cast<uint32_t>(w.buf.size()));
}

template<typename... TArgs>
inline void emitNet(const std::string& event, int target, TArgs&&... vals)
{
        fxw_internal::Writer w;
        w.arrayHeader(static_cast<uint32_t>(sizeof...(vals)));
        (fxw_internal::encodeOne(w, std::forward<TArgs>(vals)), ...);
        __fxcppEmitNetEvent(event.c_str(), static_cast<uint32_t>(event.size()), target, w.buf.data(), static_cast<uint32_t>(w.buf.size()));
}

template<typename... TArgs>
inline void emitLatent(const std::string& event, int target, int bps, TArgs&&... vals)
{
        fxw_internal::Writer w;
        w.arrayHeader(static_cast<uint32_t>(sizeof...(vals)));
        (fxw_internal::encodeOne(w, std::forward<TArgs>(vals)), ...);
        std::string targetStr = std::to_string(target);
        invokeNative(HashString("TRIGGER_LATENT_CLIENT_EVENT_INTERNAL"), { NativeArg::ptr(event.c_str()), NativeArg::ptr(targetStr.c_str()), NativeArg::ptr(w.buf.data()), NativeArg(static_cast<int32_t>(w.buf.size())), NativeArg(static_cast<int32_t>(bps)) });
}

inline void cancelEvent()
{
        __fxcppCancelEvent();
}

inline bool wasEventCanceled()
{
        return __fxcppWasEventCanceled() != 0;
}

template<typename F>
inline int32_t onResourceStart(F&& handler)
{
        return on("onResourceStart", [h = std::forward<F>(handler)](const std::string& /*source*/, const EventArgs& args)
        {
                if (args.size() > 0)
                        h(args.str(0));
        });
}

template<typename F>
inline int32_t onResourceStop(F&& handler)
{
        return on("onResourceStop", [h = std::forward<F>(handler)](const std::string& /*source*/, const EventArgs& args)
        {
                if (args.size() > 0)
                        h(args.str(0));
        });
}

template<typename... TArgs>
inline void trace(const char* fmt, TArgs&&... args)
{
        static_assert((... && (std::is_trivially_copyable_v<std::decay_t<TArgs>>)), "fx::trace arguments must be trivially copyable (use .c_str() for std::string)");
        if constexpr (sizeof...(args) == 0)
        {
                if (fmt && fmt[0])
                        __fxcppTrace(fmt, static_cast<uint32_t>(strlen(fmt)));
        }
        else
        {
                int len = snprintf(nullptr, 0, fmt, std::forward<TArgs>(args)...);
                if (len <= 0)
                        return;
                std::string buf(static_cast<size_t>(len), '\0');
                snprintf(buf.data(), buf.size() + 1, fmt, std::forward<TArgs>(args)...);
                __fxcppTrace(buf.data(), static_cast<uint32_t>(buf.size()));
        }
}

inline void traceStr(const std::string& msg)
{
        if (!msg.empty())
                __fxcppTrace(msg.data(), static_cast<uint32_t>(msg.size()));
}

inline void onTick(TickHandler h)
{
        if (auto* c = fxw_internal::currentContext())
                c->ticks.push_back(std::move(h));
}

inline void onStop(StopHandler h)
{
        if (auto* c = fxw_internal::currentContext())
                c->stops.push_back(std::move(h));
}

inline std::string getResourceMetadata(const std::string& key, int index = 0)
{
        int32_t len = __fxcppGetResourceMetadata(key.c_str(), static_cast<uint32_t>(key.size()), index, nullptr, 0);
        if (len <= 0)
                return { };
        std::string out(static_cast<size_t>(len), '\0');
        __fxcppGetResourceMetadata(key.c_str(), static_cast<uint32_t>(key.size()), index, out.data(), len + 1);
        return out;
}

inline int getNumResourceMetadata(const std::string& key)
{
        return __fxcppGetNumResourceMetadata(key.c_str(), static_cast<uint32_t>(key.size()));
}

inline bool isManifestVersionV2Between(const std::string& lower, const std::string& upper)
{
        return __fxcppIsManifestVersionV2Between(lower.c_str(), static_cast<uint32_t>(lower.size()), upper.c_str(), static_cast<uint32_t>(upper.size())) != 0;
}

inline std::string getCurrentResourceName()
{
        auto ctx = invokeNative(HashString("GET_CURRENT_RESOURCE_NAME"), { }, 1, 0x1);
        return getStringResult(ctx, 0);
}

inline std::string getInvokingResource()
{
        auto ctx = invokeNative(HashString("GET_INVOKING_RESOURCE"), { }, 1, 0x1);
        return getStringResult(ctx, 0);
}

inline std::string getResourcePath(const std::string& resource)
{
        auto ctx = invokeNative(HashString("GET_RESOURCE_PATH"), { NativeArg::ptr(resource.c_str()) }, 1, 0x1);
        return getStringResult(ctx, 0);
}

inline std::string getConvar(const std::string& name, const std::string& defaultValue = "")
{
        auto ctx = invokeNative(HashString("GET_CONVAR"), { NativeArg::ptr(name.c_str()), NativeArg::ptr(defaultValue.c_str()) }, 1, 0x1);
        return getStringResult(ctx, 0);
}

inline int getConvarInt(const std::string& name, int defaultValue = 0)
{
        auto ctx = invokeNative(HashString("GET_CONVAR_INT"), { NativeArg::ptr(name.c_str()), NativeArg(defaultValue) }, 1);
        return static_cast<int>(ctx.args[0]);
}

inline void setConvar(const std::string& name, const std::string& value)
{
        invokeNative(HashString("SET_CONVAR"), { NativeArg::ptr(name.c_str()), NativeArg::ptr(value.c_str()) });
}

inline void setConvarReplicated(const std::string& name, const std::string& value)
{
        invokeNative(HashString("SET_CONVAR_REPLICATED"), { NativeArg::ptr(name.c_str()), NativeArg::ptr(value.c_str()) });
}

inline std::string getResourceState(const std::string& resource)
{
        auto ctx = invokeNative(HashString("GET_RESOURCE_STATE"), { NativeArg::ptr(resource.c_str()) }, 1, 0x1);
        return getStringResult(ctx, 0);
}

inline int getNumResources()
{
        auto ctx = invokeNative(HashString("GET_NUM_RESOURCES"), { }, 1);
        return static_cast<int>(ctx.args[0]);
}

inline std::string getResourceByIndex(int index)
{
        auto ctx = invokeNative(HashString("GET_RESOURCE_BY_FIND_INDEX"), { NativeArg(static_cast<int32_t>(index)) }, 1, 0x1);
        return getStringResult(ctx, 0);
}

inline std::vector<std::string> getResources()
{
        std::vector<std::string> result;
        int num = getNumResources();
        for (int i = 0; i < num; i++)
        {
                std::string name = getResourceByIndex(i);
                if (!name.empty())
                        result.push_back(std::move(name));
        }
        return result;
}

inline void setStateBagValue(const std::string& bagName, const std::string& key, const json::Value& value, bool replicated = true)
{
        auto encoded = fxw_internal::encode(value);
        invokeNative(HashString("SET_STATE_BAG_VALUE"), { NativeArg::ptr(bagName.c_str()), NativeArg::ptr(key.c_str()), NativeArg::ptr(encoded.data()), NativeArg(static_cast<int32_t>(encoded.size())), NativeArg(replicated ? 1 : 0) });
}

inline void setPlayerState(int serverId, const std::string& key, const json::Value& value, bool replicated = true)
{
        setStateBagValue("player:" + std::to_string(serverId), key, value, replicated);
}

inline void setEntityState(int netId, const std::string& key, const json::Value& value, bool replicated = true)
{
        setStateBagValue("entity:" + std::to_string(netId), key, value, replicated);
}

inline void setGlobalState(const std::string& key, const json::Value& value, bool replicated = true)
{
        setStateBagValue("global", key, value, replicated);
}

inline json::Value getStateBagValue(const std::string& bagName, const std::string& key)
{
        auto ctx = invokeNative(HashString("GET_STATE_BAG_VALUE"), { NativeArg::ptr(bagName.c_str()), NativeArg::ptr(key.c_str()) }, 2);
        uintptr_t dataAddr = static_cast<uintptr_t>(ctx.args[0]);
        size_t size = static_cast<size_t>(ctx.args[1]);
        if (!dataAddr || size == 0)
                return json::makeNull();
        return fxw_internal::decode(reinterpret_cast<const void*>(dataAddr), static_cast<uint32_t>(size));
}

inline json::Value getPlayerState(int serverId, const std::string& key)
{
        return getStateBagValue("player:" + std::to_string(serverId), key);
}

inline json::Value getEntityState(int netId, const std::string& key)
{
        return getStateBagValue("entity:" + std::to_string(netId), key);
}

inline json::Value getGlobalState(const std::string& key)
{
        return getStateBagValue("global", key);
}

inline bool stateBagHasKey(const std::string& bagName, const std::string& key)
{
        auto ctx = invokeNative(HashString("STATE_BAG_HAS_KEY"), { NativeArg::ptr(bagName.c_str()), NativeArg::ptr(key.c_str()) }, 1);
        return ctx.args[0] != 0;
}

inline std::vector<std::string> getStateBagKeys(const std::string& bagName)
{
        auto ctx = invokeNative(HashString("GET_STATE_BAG_KEYS"), { NativeArg::ptr(bagName.c_str()) }, 2);
        uintptr_t dataAddr = static_cast<uintptr_t>(ctx.args[0]);
        size_t size = static_cast<size_t>(ctx.args[1]);
        if (!dataAddr || size == 0)
                return { };
        auto arr = fxw_internal::decode(reinterpret_cast<const void*>(dataAddr), static_cast<uint32_t>(size));
        std::vector<std::string> keys;
        for (size_t i = 0; i < arr.size(); i++)
                keys.push_back(arr.at(i).asStr());
        return keys;
}

inline int32_t addStateBagChangeHandler(const std::string& keyFilter, const std::string& bagFilter, StateBagChangeHandler handler)
{
        int32_t hostRef = detail::createRef([handler](const uint8_t* args, uint32_t argsSize) -> std::vector<uint8_t>
        {
                auto decoded = fxw_internal::decode(args, argsSize);
                fxw_internal::ensureArray(decoded);
                if (decoded.size() >= 5)
                {
                        std::string bagName = decoded.at(0).asStr();
                        std::string key = decoded.at(1).asStr();
                        json::Value value = decoded.at(2);
                        int source = decoded.at(3).asInt();
                        bool replicated = decoded.at(4).asBool();
                        handler(bagName, key, value, source, replicated);
                }
                return { MSGPACK_EMPTY_ARRAY };
        });
        std::string cbRef = detail::canonicalizeRef(hostRef);
        if (cbRef.empty())
        {
                detail::removeRef(hostRef);
                return -1;
        }
        const char* keyArg = keyFilter.empty() ? nullptr : keyFilter.c_str();
        const char* bagArg = bagFilter.empty() ? nullptr : bagFilter.c_str();
        auto ctx = invokeNative(HashString("ADD_STATE_BAG_CHANGE_HANDLER"), { NativeArg::ptr(keyArg), NativeArg::ptr(bagArg), NativeArg::ptr(cbRef.c_str()) }, 1);
        int32_t cookie = static_cast<int32_t>(ctx.args[0]);
        if (auto* c = fxw_internal::currentContext())
                c->stateBagHandlerRefs[cookie] = hostRef;
        return cookie;
}

inline void removeStateBagChangeHandler(int32_t cookie)
{
        invokeNative(HashString("REMOVE_STATE_BAG_CHANGE_HANDLER"), { NativeArg(cookie) });
        if (auto* c = fxw_internal::currentContext())
        {
                auto it = c->stateBagHandlerRefs.find(cookie);
                if (it != c->stateBagHandlerRefs.end())
                {
                        detail::removeRef(it->second);
                        c->stateBagHandlerRefs.erase(it);
                }
        }
}

inline void addExport(const std::string& name, ExportHandler handler)
{
        int32_t hostRef = detail::createRef([handler](const uint8_t* args, uint32_t argsSize) -> std::vector<uint8_t>
        {
                auto decoded = fxw_internal::decode(args, argsSize);
                fxw_internal::ensureArray(decoded);
                EventArgs ea(std::move(decoded));
                json::Value result = handler(ea);
                fxw_internal::Value arr;
                arr.kind = fxw_internal::Value::Kind::Array;
                arr.children.push_back(std::move(result));
                return fxw_internal::encode(arr);
        });
        std::string exportRef = detail::canonicalizeRef(hostRef);
        if (exportRef.empty())
        {
                detail::removeRef(hostRef);
                return;
        }
        std::string resName = getCurrentResourceName();
        std::string eventName = "__cfx_export_" + resName + "_" + name;
        on(eventName, [exportRef](const std::string&, EventArgs args)
        {
                if (args.size() == 0)
                        return;
                std::string setterRef = args.funcRef(0);
                if (setterRef.empty())
                        return;
                fxw_internal::Value refVal;
                refVal.kind = fxw_internal::Value::Kind::FuncRef;
                refVal.scalar = exportRef;
                fxw_internal::Value arr;
                arr.kind = fxw_internal::Value::Kind::Array;
                arr.children.push_back(std::move(refVal));
                auto payload = fxw_internal::encode(arr);
                detail::invokeFunctionReference(setterRef, payload.data(), static_cast<uint32_t>(payload.size()));
        });
}

inline json::Value callExport(const std::string& resource, const std::string& name, std::initializer_list<json::Value> args = { })
{
        auto capturedRef = std::make_shared<std::string>();
        int32_t setterRef = detail::createRef([capturedRef](const uint8_t* data, uint32_t size) -> std::vector<uint8_t>
        {
                auto decoded = fxw_internal::decode(data, size);
                if (decoded.kind == fxw_internal::Value::Kind::FuncRef)
                        *capturedRef = decoded.scalar;
                else if (decoded.kind == fxw_internal::Value::Kind::Array && decoded.size() > 0 && decoded.at(0).kind == fxw_internal::Value::Kind::FuncRef)
                        *capturedRef = decoded.at(0).scalar;
                return { MSGPACK_EMPTY_ARRAY };
        });
        std::string setterRefStr = detail::canonicalizeRef(setterRef);
        if (setterRefStr.empty())
                return { };
        fxw_internal::Value setterVal;
        setterVal.kind = fxw_internal::Value::Kind::FuncRef;
        setterVal.scalar = setterRefStr;
        fxw_internal::Value setterArr;
        setterArr.kind = fxw_internal::Value::Kind::Array;
        setterArr.children.push_back(std::move(setterVal));
        auto setterPayload = fxw_internal::encode(setterArr);
        std::string eventName = "__cfx_export_" + resource + "_" + name;
        __fxcppEmitEvent(eventName.c_str(), static_cast<uint32_t>(eventName.size()), setterPayload.data(), static_cast<uint32_t>(setterPayload.size()));
        detail::removeRef(setterRef);
        if (capturedRef->empty())
                return { };
        fxw_internal::Value argArr;
        argArr.kind = fxw_internal::Value::Kind::Array;
        argArr.children.assign(args.begin(), args.end());
        auto userPayload = fxw_internal::encode(argArr);
        auto retData = detail::invokeFunctionReference(*capturedRef, userPayload.data(), static_cast<uint32_t>(userPayload.size()));
        if (retData.empty())
                return { };
        auto result = fxw_internal::decode(retData.data(), static_cast<uint32_t>(retData.size()));
        if (result.kind == fxw_internal::Value::Kind::Array && result.size() > 0)
                return result.at(0);
        return result;
}

inline void onCommand(const std::string& command, CommandHandler h)
{
        if (auto* c = fxw_internal::currentContext())
                c->commands[command].push_back(h);
        int32_t hostRef = detail::createRef([command](const uint8_t* args, uint32_t argsSize) -> std::vector<uint8_t>
        {
                auto decoded = fxw_internal::decode(args, argsSize);
                fxw_internal::ensureArray(decoded);
                std::string source = decoded.size() > 0 ? std::to_string(decoded.at(0).asInt()) : "0";
                std::vector<std::string> cmdArgs;
                if (decoded.size() > 1 && decoded.at(1).kind == fxw_internal::Value::Kind::Array)
                        for (size_t i = 0; i < decoded.at(1).size(); ++i)
                                cmdArgs.push_back(decoded.at(1).at(i).asStr());
                auto* ctx = fxw_internal::currentContext();
                if (ctx)
                {
                        auto it = ctx->commands.find(command);
                        if (it != ctx->commands.end())
                                for (auto& handler : it->second)
                                        handler(source, cmdArgs);
                }
                return { MSGPACK_EMPTY_ARRAY };
        });
        std::string cbRef = detail::canonicalizeRef(hostRef);
        if (cbRef.empty())
        {
                detail::removeRef(hostRef);
                return;
        }
        invokeNative(HashString("REGISTER_COMMAND"), { NativeArg::ptr(command.c_str()), NativeArg::ptr(cbRef.c_str()), NativeArg(0) });
}

}

namespace fx
{

inline void performHttpRequest(const std::string& url, HttpCallback cb, const std::string& method = "GET", const std::string& data = "", const std::string& headers = "{}")
{
        std::string safeHeaders = "{}";
        {
                json::detail::Parser p{ headers, 0, false };
                auto v = p.parseValue();
                p.skipWs();
                if (!p.error && p.pos == headers.size() && v.kind == fxw_internal::Value::Kind::Object)
                        safeHeaders = headers;
        }
        std::string payload = "{\"url\":" + json::quote(url) + ",\"method\":" + json::quote(method) + ",\"data\":" + json::quote(data) + ",\"headers\":" + safeHeaders + "}";
        auto ctx = invokeNative(HashString("PERFORM_HTTP_REQUEST_INTERNAL"), { NativeArg::ptr(payload.c_str()), NativeArg(static_cast<int32_t>(payload.size())) }, 1);
        int32_t token = static_cast<int32_t>(ctx.args[0]);
        if (token == -1)
                return;
        auto* c = fxw_internal::currentContext();
        if (!c)
                return;
        c->httpCallbacks[token] = std::move(cb);
        if (!c->httpResponseRegistered)
        {
                c->httpResponseRegistered = true;
                on("__cfx_internal:httpResponse", [](const std::string&, const EventArgs& args)
                {
                        auto* ctx = fxw_internal::currentContext();
                        if (!ctx || args.size() < 4)
                                return;
                        int32_t tok = args.get<int>(0);
                        auto it = ctx->httpCallbacks.find(tok);
                        if (it == ctx->httpCallbacks.end())
                                return;
                        auto userCb = std::move(it->second);
                        ctx->httpCallbacks.erase(it);
                        int status = args.get<int>(1);
                        std::string body = args.str(2);
                        std::string respHeaders = args.str(3);
#if __cpp_exceptions
                        try
                        {
                                userCb(status, body, respHeaders);
                        }
                        catch (...)
                        {
                        }
#else
                        userCb(status, body, respHeaders);
#endif
                });
        }
}

inline int32_t setTimeout(uint32_t ms, std::function<void()> cb)
{
        auto* c = fxw_internal::currentContext();
        return c ? c->addTimer(ms, 0, std::move(cb)) : -1;
}

inline int32_t setInterval(uint32_t ms, std::function<void()> cb)
{
        auto* c = fxw_internal::currentContext();
        return c ? c->addTimer(ms, ms, std::move(cb)) : -1;
}

inline void clearTimer(int32_t id)
{
        auto* c = fxw_internal::currentContext();
        if (c)
                c->timers.erase(id);
}

inline void setKvp(const std::string& key, const std::string& value)
{
        natives::invoke(HashString("SET_RESOURCE_KVP"), key.c_str(), value.c_str());
}

inline void setKvpInt(const std::string& key, int value)
{
        natives::invoke(HashString("SET_RESOURCE_KVP_INT"), key.c_str(), value);
}

inline void setKvpFloat(const std::string& key, float value)
{
        natives::invoke(HashString("SET_RESOURCE_KVP_FLOAT"), key.c_str(), value);
}

inline std::string getKvpString(const std::string& key)
{
        return natives::invoke<std::string>(HashString("GET_RESOURCE_KVP_STRING"), key.c_str());
}

inline int getKvpInt(const std::string& key)
{
        return natives::invoke<int>(HashString("GET_RESOURCE_KVP_INT"), key.c_str());
}

inline float getKvpFloat(const std::string& key)
{
        return natives::invoke<float>(HashString("GET_RESOURCE_KVP_FLOAT"), key.c_str());
}

inline void deleteKvp(const std::string& key)
{
        natives::invoke(HashString("DELETE_RESOURCE_KVP"), key.c_str());
}

inline void flushKvp()
{
        natives::invoke(HashString("FLUSH_RESOURCE_KVP"));
}

inline std::vector<std::string> findKvp(const std::string& prefix)
{
        std::vector<std::string> keys;
        int handle = natives::invoke<int>(HashString("START_FIND_KVP"), prefix.c_str());
        if (handle == -1)
                return keys;
        while (true)
        {
                std::string key = natives::invoke<std::string>(HashString("FIND_KVP"), handle);
                if (key.empty())
                        break;
                keys.push_back(std::move(key));
        }
        natives::invoke(HashString("END_FIND_KVP"), handle);
        return keys;
}

}

namespace fxw_internal
{

struct WasmPromise;
using WasmCoroutineHandle = std::coroutine_handle<WasmPromise>;

struct WasmTask;

struct WasmPromise
{
        int64_t waitMs = 0;
#if __cpp_exceptions
        std::exception_ptr exception;
#endif
        WasmTask get_return_object();
        std::suspend_always initial_suspend() noexcept
        {
                return { };
        }
        std::suspend_always final_suspend() noexcept
        {
                return { };
        }
        void return_void()
        {
        }
#if __cpp_exceptions
        void unhandled_exception()
        {
                exception = std::current_exception();
        }
#else
        void unhandled_exception() noexcept
        {
                __builtin_abort();
        }
#endif
};

struct WasmTask
{
        using promise_type = WasmPromise;
        WasmCoroutineHandle handle;
};

inline WasmTask WasmPromise::get_return_object()
{
        return WasmTask{ WasmCoroutineHandle::from_promise(*this) };
}

struct CoroutineEntry
{
        WasmCoroutineHandle handle;
        std::shared_ptr<void> prevent_destruct;
        std::chrono::steady_clock::time_point resumeAt;
};

inline std::unordered_map<int32_t, CoroutineEntry>& coroutines()
{
        static std::unordered_map<int32_t, CoroutineEntry> s_map;
        return s_map;
}

inline int32_t& nextCoroutineId()
{
        static int32_t s_id = 1;
        return s_id;
}

inline void resumeCoroutineById(int32_t id)
{
        auto& coros = coroutines();
        auto it = coros.find(id);
        if (it == coros.end())
                return;
        auto handle = it->second.handle;
        handle.promise().waitMs = 0;
#if __cpp_exceptions
        handle.promise().exception = nullptr;
#endif
        handle.resume();
        if (handle.done())
        {
#if __cpp_exceptions
                if (handle.promise().exception)
                {
                        const char* rn = currentContext() ? currentContext()->resourceName.c_str() : "?";
                        try
                        {
                                std::rethrow_exception(handle.promise().exception);
                        }
                        catch (const std::exception& e)
                        {
                                fprintf(stderr, "[script:%s] Unhandled exception in thread: %s\n", rn, e.what());
                        }
                        catch (...)
                        {
                                fprintf(stderr, "[script:%s] Unhandled non-standard exception in thread\n", rn);
                        }
                }
#endif
                handle.destroy();
                coros.erase(it);
        }
        else
        {
                int64_t waitMs = handle.promise().waitMs;
                auto now = std::chrono::steady_clock::now();
                it->second.resumeAt = (waitMs > 0) ? now + std::chrono::milliseconds(waitMs) : now;
                __fxcppScheduleBookmark(id, static_cast<int32_t>(waitMs));
        }
}

inline void resumeCoroutines()
{
        auto& coros = coroutines();
        if (coros.empty())
                return;
        auto now = std::chrono::steady_clock::now();
        std::vector<int32_t> ready;
        for (auto& [id, entry] : coros)
                if (now >= entry.resumeAt)
                        ready.push_back(id);
        for (auto id : ready)
                resumeCoroutineById(id);
}

inline void cleanupCoroutines()
{
        auto& coros = coroutines();
        for (auto& [id, entry] : coros)
        {
                if (entry.handle)
                        entry.handle.destroy();
        }
        coros.clear();
}

}

namespace fx
{

using ScriptTask = fxw_internal::WasmTask;

struct Wait
{
        int64_t ms;
        explicit Wait(int64_t ms) : ms(ms)
        {
        }
        bool await_ready() const noexcept
        {
                return false;
        }
        void await_suspend(fxw_internal::WasmCoroutineHandle h) const noexcept
        {
                h.promise().waitMs = ms;
        }
        void await_resume() const noexcept
        {
        }
};

template<typename F>
inline void createThread(F&& fn)
{
        auto* c = fxw_internal::currentContext();
        if (!c)
                return;
        auto& coros = fxw_internal::coroutines();
        if (coros.size() >= fxw_internal::MAX_COROUTINES)
                return;
        auto stored = std::make_shared<std::decay_t<F>>(std::forward<F>(fn));
        auto task = (*stored)();
        auto& nextId = fxw_internal::nextCoroutineId();
        int32_t id = nextId;
        int32_t start = id;
        while (coros.count(id))
        {
                if (++id <= 0)
                        id = 1;
                if (id == start)
                        return;
        }
        nextId = id + 1;
        if (nextId <= 0)
                nextId = 1;
        coros[id] = { task.handle, std::move(stored), std::chrono::steady_clock::now() };
        __fxcppScheduleBookmark(id, 0);
}

inline ProcessResult spawnProcess(const std::string& command, int32_t maxOutput = 65536)
{
        ProcessResult result{ };
        std::string buf(static_cast<size_t>(maxOutput), '\0');
        result.status = __fxcppSpawnProcess(command.c_str(), static_cast<uint32_t>(command.size()), buf.data(), maxOutput);
        if (result.status > 0)
                buf.resize(static_cast<size_t>(result.status));
        else
                buf.clear();
        result.output = std::move(buf);
        return result;
}

}

#define FXCPP_WORKER(name)                                                                                  \
        static int32_t name##_impl(const char* input, int32_t input_len, char* result, int32_t result_max); \
        extern "C" __attribute__((export_name(#name)))                                                      \
        int32_t                                                                                             \
        name(intptr_t ip, int32_t il, intptr_t rp, int32_t rm)                                              \
        {                                                                                                   \
                return name##_impl(reinterpret_cast<const char*>(ip), il, reinterpret_cast<char*>(rp), rm); \
        }                                                                                                   \
        static int32_t name##_impl(const char* input, int32_t input_len, char* result, int32_t result_max)

namespace fx
{

struct WorkerResult
{
        int32_t status; // 0 = running, > 0 = bytes written (done), -1 = error, -2 = invalid
        std::string output;
};

inline int32_t createWorker(const std::string& fnName, const std::string& input = "")
{
        return __fxcppCreateWorker(fnName.c_str(), static_cast<uint32_t>(fnName.size()), input.c_str(), static_cast<uint32_t>(input.size()));
}

inline WorkerResult pollWorker(int32_t workerId, int32_t maxOutput = 65536)
{
        WorkerResult result{ };
        std::string buf(static_cast<size_t>(maxOutput), '\0');
        int32_t raw = __fxcppPollWorker(workerId, buf.data(), maxOutput);
        if (raw > 0)
        {
                result.status = raw;
                size_t written = static_cast<size_t>(raw - 1);
                buf.resize(written);
        }
        else
        {
                result.status = raw;
                buf.clear();
        }
        result.output = std::move(buf);
        return result;
}

}

#define FXCPP_WASM_EXPORT(name) extern "C" __attribute__((export_name(#name)))

FXCPP_WASM_EXPORT(fxcpp_alloc)
void* fxcppAlloc(uint32_t size)
{
        return malloc(size);
}

FXCPP_WASM_EXPORT(fxcpp_free)
void fxcppFree(void* ptr, uint32_t)
{
        free(ptr);
}

FXCPP_WASM_EXPORT(fxcpp_has_pending_work)
int32_t fxcppHasPendingWork()
{
        if (!fxw_internal::coroutines().empty())
                return 1;
        auto* c = fxw_internal::currentContext();
        if (!c)
                return 0;
        return (!c->ticks.empty() || !c->timers.empty()) ? 1 : 0;
}

FXCPP_WASM_EXPORT(fxcpp_tick)
void fxcppTick()
{
        fxw_internal::resumeCoroutines();
        if (auto* c = fxw_internal::currentContext())
                c->dispatchTick();
}

FXCPP_WASM_EXPORT(fxcpp_tick_bookmarks)
void fxcppTickBookmarks(int32_t* bookmarks, int32_t count)
{
        for (int32_t i = 0; i < count; ++i)
                fxw_internal::resumeCoroutineById(bookmarks[i]);
}

FXCPP_WASM_EXPORT(fxcpp_on_event)
void fxcppOnEvent(const char* name, uint32_t nameLen, const uint8_t* args, uint32_t argsLen, const char* src, uint32_t srcLen)
{
        if (auto* c = fxw_internal::currentContext())
                c->dispatchEvent(name, nameLen, args, argsLen, src, srcLen);
}

FXCPP_WASM_EXPORT(fxcpp_on_stop)
void fxcppOnStop()
{
        fxw_internal::cleanupCoroutines();
        if (auto* c = fxw_internal::currentContext())
        {
                for (auto& [cookie, hostRef] : c->stateBagHandlerRefs)
                {
                        fx::invokeNative(HashString("REMOVE_STATE_BAG_CHANGE_HANDLER"), { fx::NativeArg(cookie) });
                        fx::detail::removeRef(hostRef);
                }
                c->stateBagHandlerRefs.clear();
                c->dispatchStop();
        }
}

FXCPP_WASM_EXPORT(fxcpp_invoke_ref)
int32_t fxcppInvokeRef(int32_t callback_id, const uint8_t* args_ptr, uint32_t args_len, uint8_t* result_ptr, uint32_t result_max)
{
        auto& callbacks = fxw_internal::refCallbacks();
        auto it = callbacks.find(callback_id);
        if (it == callbacks.end())
                return 0;
        auto result = it->second(args_ptr, args_len);
        uint32_t copy = std::min<uint32_t>(static_cast<uint32_t>(result.size()), result_max);
        if (copy > 0 && result_ptr)
                memcpy(result_ptr, result.data(), copy);
        return static_cast<int32_t>(result.size());
}

FXCPP_WASM_EXPORT(fxcpp_duplicate_ref)
int32_t fxcppDuplicateRef(int32_t callback_id)
{
        auto& counts = fxw_internal::refCounts();
        auto it = counts.find(callback_id);
        if (it != counts.end())
                ++it->second;
        return callback_id;
}

FXCPP_WASM_EXPORT(fxcpp_remove_ref)
void fxcppRemoveRef(int32_t callback_id)
{
        auto& counts = fxw_internal::refCounts();
        auto it = counts.find(callback_id);
        if (it == counts.end())
                return;
        if (--it->second <= 0)
        {
                counts.erase(it);
                fxw_internal::refCallbacks().erase(callback_id);
        }
}

#define FXCPP_WASM_ENTRY                                                                                                \
        static void _fxcpp_wasm_body();                                                                                 \
        FXCPP_WASM_EXPORT(fxcpp_init)                                                                                   \
        void fxcpp_init()                                                                                               \
        {                                                                                                               \
                static fxw_internal::Context s_ctx;                                                                     \
                fxw_internal::currentContext() = &s_ctx;                                                                \
                {                                                                                                       \
                        static constexpr const char k[] = "resource_name";                                              \
                        int32_t len = __fxcppGetResourceMetadata(k, sizeof(k) - 1, 0, nullptr, 0);                   \
                        if (len > 0)                                                                                    \
                        {                                                                                               \
                                s_ctx.resourceName.resize(static_cast<size_t>(len));                                    \
                                __fxcppGetResourceMetadata(k, sizeof(k) - 1, 0, s_ctx.resourceName.data(), len + 1); \
                        }                                                                                               \
                }                                                                                                       \
                _fxcpp_wasm_body();                                                                                     \
        }                                                                                                               \
        static void _fxcpp_wasm_body()
#define Server FXCPP_WASM_ENTRY

#include "../src/DB.h"

#else

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <type_traits>

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

inline guid_t GetNullGuid()
{
        return { };
}

inline bool IsNullGuid(const guid_t& g)
{
        return GuidEquals(g, GetNullGuid());
}
}

inline bool operator==(const guid_t& a, const guid_t& b)
{
        return fx::GuidEquals(a, b);
}

inline bool operator!=(const guid_t& a, const guid_t& b)
{
        return !fx::GuidEquals(a, b);
}

inline bool operator<(const guid_t& a, const guid_t& b)
{
        return memcmp(&a, &b, sizeof(guid_t)) < 0;
}

#define OM_DECL
#define FXCPP_EXPORT __attribute__((visibility("default")))

#define NS_IMETHOD_(rv) virtual rv OM_DECL
#define NS_IMETHOD NS_IMETHOD_(result_t)
#define NS_DECLARE_STATIC_IID_ACCESSOR(iid_const) \
        static inline guid_t GetIID()             \
        {                                         \
                return iid_const;                 \
        }

inline void* fwAlloc(size_t size)
{
        return malloc(size);
}
inline void fwFree(void* p)
{
        free(p);
}

class fxIBase
{
    public:
        NS_IMETHOD_(result_t)
        QueryInterface(const guid_t& riid, void** outObject) = 0;
        NS_IMETHOD_(uint32_t)
        AddRef() = 0;
        NS_IMETHOD_(uint32_t)
        Release() = 0;
};

class fwRefCountable
{
        std::atomic<uint32_t> m_count{ 0 };

    public:
        virtual ~fwRefCountable() = default;

        virtual void AddRef()
        {
                m_count.fetch_add(1, std::memory_order_relaxed);
        }
        virtual bool Release()
        {
                if (m_count.fetch_sub(1, std::memory_order_acq_rel) == 1)
                {
                        delete this;
                        return true;
                }
                return false;
        }
};

template<class T>
class fwRefContainer
{
        T* m_ref = nullptr;

    public:
        fwRefContainer() = default;
        fwRefContainer(T* ref) : m_ref(ref)
        {
                if (m_ref)
                        m_ref->AddRef();
        }
        fwRefContainer(const fwRefContainer& o) : m_ref(o.m_ref)
        {
                if (m_ref)
                        m_ref->AddRef();
        }
        fwRefContainer(fwRefContainer&& o) noexcept : m_ref(o.m_ref)
        {
                o.m_ref = nullptr;
        }
        ~fwRefContainer()
        {
                if (m_ref)
                        m_ref->Release();
        }
        fwRefContainer& operator=(const fwRefContainer& o)
        {
                if (o.m_ref)
                        o.m_ref->AddRef();
                if (m_ref)
                        m_ref->Release();
                m_ref = o.m_ref;
                return *this;
        }
        fwRefContainer& operator=(fwRefContainer&& o) noexcept
        {
                if (m_ref)
                        m_ref->Release();
                m_ref = o.m_ref;
                o.m_ref = nullptr;
                return *this;
        }
        T* GetRef() const
        {
                return m_ref;
        }
        T* operator->() const
        {
                return m_ref;
        }
        explicit operator bool() const
        {
                return m_ref != nullptr;
        }
};

struct fxNativeContext
{
        uintptr_t arguments[32];
        int numArguments;
        int numResults;
        uint64_t nativeIdentifier;
};

namespace fx
{

template<class T>
class OMPtr
{
        T* m_ref = nullptr;

    public:
        OMPtr() = default;
        ~OMPtr()
        {
                if (m_ref)
                {
                        m_ref->Release();
                        m_ref = nullptr;
                }
        }
        OMPtr(T* ref) : m_ref(ref)
        {
                if (m_ref)
                        m_ref->AddRef();
        }
        OMPtr(const OMPtr& o) : m_ref(o.m_ref)
        {
                if (m_ref)
                        m_ref->AddRef();
        }
        OMPtr(OMPtr&& o) noexcept : m_ref(o.m_ref)
        {
                o.m_ref = nullptr;
        }
        OMPtr& operator=(const OMPtr& o)
        {
                if (o.m_ref)
                        o.m_ref->AddRef();
                if (m_ref)
                        m_ref->Release();
                m_ref = o.m_ref;
                return *this;
        }
        OMPtr& operator=(OMPtr&& o) noexcept
        {
                if (m_ref)
                        m_ref->Release();
                m_ref = o.m_ref;
                o.m_ref = nullptr;
                return *this;
        }
        T* GetRef() const
        {
                return m_ref;
        }
        T* operator->() const
        {
                return m_ref;
        }
        explicit operator bool() const
        {
                return m_ref != nullptr;
        }
        T** GetAddressOf()
        {
                return &m_ref;
        }
        T** ReleaseAndGetAddressOf()
        {
                if (m_ref)
                {
                        m_ref->Release();
                        m_ref = nullptr;
                }
                return &m_ref;
        }
        result_t CopyTo(T** ptr) const
        {
                if (m_ref)
                        m_ref->AddRef();
                *ptr = m_ref;
                return FX_S_OK;
        }
        template<class TOther>
        result_t As(OMPtr<TOther>* p)
        {
                if (!m_ref)
                        return FX_E_INVALIDARG;
                return m_ref->QueryInterface(TOther::GetIID(), reinterpret_cast<void**>(p->ReleaseAndGetAddressOf()));
        }
};

template<typename TInterface>
inline result_t MakeInterface(OMPtr<TInterface>* ptr, const guid_t& clsid = GetNullGuid());
}

namespace fx
{

template<typename TClass, typename... TInterfaces>
class OMClass : public TInterfaces...
{
        std::atomic<int32_t> m_refCount{ 0 };

    protected:
        OMClass() = default;
        virtual ~OMClass() = default;
        void* operator new(size_t) noexcept
        {
                return nullptr;
        }
        void* operator new(size_t, void* p)
        {
                return p;
        }

    public:
        template<typename TNew, typename... TArg>
        friend OMPtr<TNew> MakeNew(TArg...);
        template<typename TNew, typename... TArg>
        friend fxIBase* MakeNewBase(TArg...);
        virtual result_t OM_DECL QueryInterface(const guid_t& riid, void** outObject) override
        {
                result_t res = FX_E_NOINTERFACE;
                ([&]
                {
                        if (res == FX_E_NOINTERFACE && riid == TInterfaces::GetIID())
                        {
                                res = FX_S_OK;
                                *outObject = static_cast<TInterfaces*>(this);
                                AddRef();
                        }
                }(),
                ...);
                if (res == FX_E_NOINTERFACE)
                {
                        constexpr guid_t iunknown = { 0, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
                        if (riid == iunknown)
                        {
                                res = FX_S_OK;
                                *outObject = this;
                                AddRef();
                        }
                }
                return res;
        }
        virtual uint32_t OM_DECL AddRef() override
        {
                return static_cast<uint32_t>(m_refCount.fetch_add(1) + 1);
        }
        virtual uint32_t OM_DECL Release() override
        {
                auto prev = m_refCount.fetch_sub(1, std::memory_order_acq_rel);
                assert(prev > 0 && "Release() called on object with refcount <= 0 (double-free?)");
                if (prev == 1)
                {
                        this->~OMClass();
                        fwFree(this);
                        return 0;
                }
                return static_cast<uint32_t>(prev - 1);
        }
        fxIBase* GetBaseRef()
        {
                using First = std::tuple_element_t<0, std::tuple<TInterfaces...>>;
                return static_cast<First*>(this);
        }
};

template<typename TClass, typename... TArg>
inline OMPtr<TClass> MakeNew(TArg... args)
{
        TClass* inst = new (fwAlloc(sizeof(TClass))) TClass(args...);
        OMPtr<TClass> ret(inst);
        return ret;
}

template<typename TClass, typename... TArg>
inline fxIBase* MakeNewBase(TArg... args)
{
        TClass* inst = new (fwAlloc(sizeof(TClass))) TClass(args...);
        inst->AddRef();
        return inst->GetBaseRef();
}

}

extern "C" intptr_t fxFindFirstImpl(const guid_t& iid, guid_t* clsid);
extern "C" int32_t fxFindNextImpl(intptr_t handle, guid_t* clsid);
extern "C" void fxFindImplClose(intptr_t handle);
extern "C" result_t fxCreateObjectInstance(const guid_t& guid, const guid_t& iid, void** objectRef);

namespace fx
{

template<typename TInterface>
inline result_t MakeInterface(OMPtr<TInterface>* ptr, const guid_t& clsid)
{
        return fxCreateObjectInstance(clsid, TInterface::GetIID(), reinterpret_cast<void**>(ptr->ReleaseAndGetAddressOf()));
}
}

struct OMFactoryDef
{
        guid_t guid;
        fxIBase* (*factory)();
        OMFactoryDef* next = nullptr;
        OMFactoryDef(const guid_t& g, fxIBase* (*f)()) : guid(g), factory(f)
        {
                next = s_factories;
                s_factories = this;
        }
        static OMFactoryDef* s_factories;
};

struct OMImplementsDef
{
        guid_t iid;
        guid_t clsid;
        OMImplementsDef* next = nullptr;
        OMImplementsDef(const guid_t& c, const guid_t& i) : iid(i), clsid(c)
        {
                next = s_impls;
                s_impls = this;
        }
        static OMImplementsDef* s_impls;
};

#define FX_NEW_FACTORY(RuntimeClass)                                     \
        static OMFactoryDef _g_##RuntimeClass##_factory                  \
        {                                                                \
                CLSID_##RuntimeClass,                                    \
                []() -> fxIBase* {                                       \
                        return fx::MakeNewBase<fx::cpp::RuntimeClass>(); \
                }                                                        \
        }

#define FX_IMPLEMENTS(clsid, iface)            \
        static OMImplementsDef _g_impl_##iface \
        {                                      \
                clsid, iface::GetIID()         \
        }

#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>
#include <sys/wait.h>

#include <wasmtime.h>

namespace fx
{

using RefCallback = std::function<std::vector<char>(const char* argsSerialized, uint32_t argsSize)>;
using AddRefFn = std::function<int32_t(RefCallback)>;
using RemoveRefFn = std::function<void(int32_t)>;
using ScheduleBookmarkFn = std::function<void(uint64_t, int64_t)>;

inline ProcessResult spawnProcess(const std::string& command, size_t maxOutputBytes = 1048576, int timeoutMs = 30000)
{
        ProcessResult result{ };
        int pipefd[2];
        if (pipe(pipefd) < 0)
        {
                result.status = -2;
                return result;
        }
        std::vector<std::string> envStrs;
        std::vector<char*> envp;
        {
                std::string envBlob;
                FILE* envFile = fopen("/proc/self/environ", "rb");
                if (envFile)
                {
                        char tmp[4096];
                        size_t n;
                        while ((n = fread(tmp, 1, sizeof(tmp), envFile)) > 0)
                                envBlob.append(tmp, n);
                        fclose(envFile);
                }
                size_t pos = 0;
                while (pos < envBlob.size())
                {
                        size_t end = envBlob.find('\0', pos);
                        if (end == std::string::npos)
                                end = envBlob.size();
                        std::string_view entry(envBlob.data() + pos, end - pos);
                        if (!entry.empty() && entry.substr(0, 16) != "LD_LIBRARY_PATH=")
                                envStrs.emplace_back(entry);
                        pos = end + 1;
                }
        }
        for (auto& s : envStrs)
                envp.push_back(s.data());
        envp.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addclose(&actions, pipefd[0]);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipefd[1]);
        const char* argv[] = { "sh", "-c", command.c_str(), nullptr };
        pid_t pid = 0;
        int spawnErr = posix_spawn(&pid, "/bin/sh", &actions, nullptr, const_cast<char**>(argv), envp.data());
        posix_spawn_file_actions_destroy(&actions);
        if (spawnErr != 0)
        {
                close(pipefd[0]);
                close(pipefd[1]);
                result.status = -2;
                return result;
        }
        close(pipefd[1]);
        int flags = fcntl(pipefd[0], F_GETFL, 0);
        if (flags >= 0)
                fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
        char buf[4096];
        bool timedOut = false;
        bool outputCapped = false;
        struct pollfd pfd{ };
        pfd.fd = pipefd[0];
        pfd.events = POLLIN;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (true)
        {
                auto now = std::chrono::steady_clock::now();
                int remainingMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
                if (remainingMs <= 0)
                {
                        timedOut = true;
                        break;
                }
                int ret = poll(&pfd, 1, remainingMs > 100 ? 100 : remainingMs);
                if (ret < 0 && errno == EINTR)
                        continue;
                if (ret <= 0)
                {
                        int wstatus = 0;
                        if (waitpid(pid, &wstatus, WNOHANG) != 0)
                                break;
                        if (ret == 0)
                                continue;
                        break;
                }
                ssize_t n = read(pipefd[0], buf, sizeof(buf));
                if (n <= 0)
                        break;
                size_t avail = maxOutputBytes > result.output.size() ? maxOutputBytes - result.output.size() : 0;
                if (avail == 0)
                {
                        outputCapped = true;
                        break;
                }
                size_t toAppend = static_cast<size_t>(n) < avail ? static_cast<size_t>(n) : avail;
                result.output.append(buf, toAppend);
                if (toAppend < static_cast<size_t>(n))
                {
                        outputCapped = true;
                        break;
                }
        }
        close(pipefd[0]);
        int wstatus = 0;
        if (waitpid(pid, &wstatus, WNOHANG) == 0)
        {
                if (timedOut || outputCapped)
                {
                        kill(pid, SIGKILL);
                        waitpid(pid, &wstatus, 0);
                }
                else
                {
                        waitpid(pid, &wstatus, 0);
                }
        }
        while (!result.output.empty() && result.output.back() == '\n')
                result.output.pop_back();
        if (timedOut)
        {
                result.status = -3;
                return result;
        }
        result.status = static_cast<int32_t>(result.output.size());
        return result;
}

}

FX_DEFINE_GUID(IID_fxIStream, 0x82EC2441, 0xDBB4, 0x4512, 0x81, 0xE9, 0x3A, 0x98, 0xCE, 0x9F, 0xFC, 0xAB);

class fxIStream : public fxIBase
{
    public:
        NS_DECLARE_STATIC_IID_ACCESSOR(IID_fxIStream)
};

FX_DEFINE_GUID(IID_IScriptBuffer, 0xAD1B9D69, 0xB984, 0x4D30, 0x8D, 0x33, 0xBB, 0x1E, 0x6C, 0xF9, 0xE1, 0xBA);

class IScriptBuffer : public fxIBase
{
    public:
        NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptBuffer)
        NS_IMETHOD_(char*)
        GetBytes() = 0;
        NS_IMETHOD_(uint32_t)
        GetLength() = 0;
};

class ScriptBuffer final : public fx::OMClass<ScriptBuffer, IScriptBuffer>
{
        std::vector<char> m_data;

    public:
        ScriptBuffer() = default;
        explicit ScriptBuffer(std::vector<char> data) : m_data(std::move(data))
        {
        }
        char* OM_DECL GetBytes() override
        {
                return m_data.data();
        }
        uint32_t OM_DECL GetLength() override
        {
                return static_cast<uint32_t>(m_data.size());
        }
};

FX_DEFINE_GUID(IID_IScriptRuntime, 0x67B28AF1, 0xAAF9, 0x4368, 0x82, 0x96, 0xF9, 0x3A, 0xFC, 0x7B, 0xDE, 0x96);

class IScriptHost;

class IScriptRuntime : public fxIBase
{
    public:
        NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptRuntime)
        NS_IMETHOD Create(IScriptHost* scriptHost) = 0;
        NS_IMETHOD Destroy() = 0;
        NS_IMETHOD_(void*)
        GetParentObject() = 0;
        NS_IMETHOD_(void)
        SetParentObject(void*) = 0;
        NS_IMETHOD_(int32_t)
        GetInstanceId() = 0;
};

FX_DEFINE_GUID(IID_IScriptHost, 0x8FFDC384, 0x4767, 0x4EA2, 0xA9, 0x35, 0x3B, 0xFC, 0xAD, 0x1D, 0xB7, 0xBF);

class fxIStream;
class IScriptBuffer;

class IScriptHost : public fxIBase
{
    public:
        NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptHost)
        NS_IMETHOD InvokeNative(fxNativeContext& context) = 0;
        NS_IMETHOD OpenSystemFile(char* fileName, fxIStream** stream) = 0;
        NS_IMETHOD OpenHostFile(char* fileName, fxIStream** stream) = 0;
        NS_IMETHOD CanonicalizeRef(int32_t localRef, int32_t instanceId, char** refString) = 0;
        NS_IMETHOD ScriptTrace(char* message) = 0;
        NS_IMETHOD SubmitBoundaryStart(char* boundaryData, int32_t boundarySize) = 0;
        NS_IMETHOD SubmitBoundaryEnd(char* boundaryData, int32_t boundarySize) = 0;
        NS_IMETHOD GetLastErrorText(char** errorString) = 0;
        NS_IMETHOD InvokeFunctionReference(char* refId, char* argsSerialized, uint32_t argsSize, IScriptBuffer** ret) = 0;
};

FX_DEFINE_GUID(IID_IScriptHostWithResourceData, 0x9568DF2D, 0x27C8, 0x4B9E, 0xB2, 0x9D, 0x48, 0x27, 0x2C, 0x31, 0x70, 0x84);

class IScriptHostWithResourceData : public fxIBase
{
    public:
        NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptHostWithResourceData)
        NS_IMETHOD GetResourceName(char** resourceName) = 0;
        NS_IMETHOD GetNumResourceMetaData(char* fieldName, int32_t* numFields) = 0;
        NS_IMETHOD GetResourceMetaData(char* fieldName, int32_t fieldIndex, char** fieldValue) = 0;
};

FX_DEFINE_GUID(IID_IScriptHostWithManifest, 0x5E212027, 0x3AAD, 0x46D1, 0x97, 0xE0, 0xB8, 0xBC, 0x5E, 0xF8, 0x9E, 0x18);

class IScriptHostWithManifest : public fxIBase
{
    public:
        NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptHostWithManifest)
        NS_IMETHOD IsManifestVersionBetween(const guid_t& lower, const guid_t& upper, bool* retval) = 0;
        NS_IMETHOD IsManifestVersionV2Between(char* lower, char* upper, bool* retval) = 0;
};

FX_DEFINE_GUID(IID_IScriptTickRuntime, 0x91B203C7, 0xF95A, 0x4902, 0xB4, 0x63, 0x72, 0x2D, 0x55, 0x09, 0x83, 0x66);

class IScriptTickRuntime : public fxIBase
{
    public:
        NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptTickRuntime)
        NS_IMETHOD Tick() = 0;
};

FX_DEFINE_GUID(IID_IScriptTickRuntimeWithBookmarks, 0x195FB3BD, 0x1A64, 0x4EBD, 0xA1, 0xCC, 0x80, 0x52, 0xED, 0x7E, 0xB0, 0xBD);

class IScriptTickRuntimeWithBookmarks : public fxIBase
{
    public:
        NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptTickRuntimeWithBookmarks)
        NS_IMETHOD TickBookmarks(uint64_t* bookmarks, int32_t numBookmarks) = 0;
};

FX_DEFINE_GUID(IID_IScriptEventRuntime, 0x637140DB, 0x24E5, 0x46BF, 0xA8, 0xBD, 0x08, 0xF2, 0xDB, 0xAC, 0x51, 0x9A);

class IScriptEventRuntime : public fxIBase
{
    public:
        NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptEventRuntime)
        NS_IMETHOD TriggerEvent(char* eventName, char* argsSerialized, uint32_t serializedSize, char* sourceId) = 0;
};

FX_DEFINE_GUID(IID_IScriptRefRuntime, 0xA2F1B24B, 0xA29F, 0x4121, 0x81, 0x62, 0x86, 0x90, 0x1E, 0xCA, 0x80, 0x97);

class IScriptRefRuntime : public fxIBase
{
    public:
        NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptRefRuntime)
        NS_IMETHOD CallRef(int32_t refIdx, char* argsSerialized, uint32_t argsSize, IScriptBuffer** retval) = 0;
        NS_IMETHOD DuplicateRef(int32_t refIdx, int32_t* newRefIdx) = 0;
        NS_IMETHOD RemoveRef(int32_t refIdx) = 0;
};

class IScriptHostWithResourceData;

FX_DEFINE_GUID(IID_IScriptFileHandlingRuntime, 0x567634C6, 0x3BDD, 0x4D0E, 0xAF, 0x39, 0x74, 0x72, 0xAE, 0xD4, 0x79, 0xB7);

class IScriptFileHandlingRuntime : public fxIBase
{
    public:
        NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptFileHandlingRuntime)
        NS_IMETHOD_(int32_t)
        HandlesFile(char* scriptFile, IScriptHostWithResourceData* metadata) = 0;
        NS_IMETHOD LoadFile(char* scriptFile) = 0;
};

class IScriptTickRuntimeWithBookmarks;

FX_DEFINE_GUID(IID_IScriptHostWithBookmarks, 0x2A7E092D, 0x6CE9, 0x4B9D, 0xAC, 0x4F, 0x8D, 0xA8, 0x18, 0xBD, 0x0D, 0xA4);

class IScriptHostWithBookmarks : public fxIBase
{
    public:
        NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptHostWithBookmarks)
        NS_IMETHOD ScheduleBookmark(IScriptTickRuntimeWithBookmarks* runtime, uint64_t bookmark, int64_t deadline) = 0;
        NS_IMETHOD RemoveBookmarks(IScriptTickRuntimeWithBookmarks* runtime) = 0;
        NS_IMETHOD CreateBookmarks(IScriptTickRuntimeWithBookmarks* runtime) = 0;
};

FX_DEFINE_GUID(IID_IScriptStackWalkVisitor, 0x182CAAF3, 0xE33D, 0x474B, 0xA6, 0xAF, 0x33, 0xD5, 0x9F, 0xF0, 0xE9, 0xED);

class IScriptStackWalkVisitor : public fxIBase
{
    public:
        NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptStackWalkVisitor)
        NS_IMETHOD SubmitStackFrame(char* frameBlob, uint32_t frameBlobSize) = 0;
};

FX_DEFINE_GUID(IID_IScriptStackWalkingRuntime, 0x567D2FDA, 0x610C, 0x4FA0, 0xAE, 0x3E, 0x4F, 0x70, 0x0A, 0xE5, 0xCE, 0x56);

class IScriptStackWalkingRuntime : public fxIBase
{
    public:
        NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptStackWalkingRuntime)
        NS_IMETHOD WalkStack(char* boundaryStart, uint32_t boundaryStartLength, char* boundaryEnd, uint32_t boundaryEndLength, IScriptStackWalkVisitor* visitor) = 0;
};

FX_DEFINE_GUID(IID_IScriptMemInfoRuntime, 0xD98A35CF, 0xD6EE, 0x4B51, 0xA1, 0xC3, 0x99, 0xB7, 0x0F, 0x4E, 0xC1, 0xE6);

class IScriptMemInfoRuntime : public fxIBase
{
    public:
        NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptMemInfoRuntime)
        NS_IMETHOD RequestMemoryUsage() = 0;
        NS_IMETHOD GetMemoryUsage(int64_t* memUsage) = 0;
};

FX_DEFINE_GUID(IID_IScriptWarningRuntime, 0xD72BE411, 0x5152, 0x4474, 0x91, 0x7C, 0x53, 0x61, 0xAC, 0x05, 0x11, 0x81);

class IScriptWarningRuntime : public fxIBase
{
    public:
        NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptWarningRuntime)
        NS_IMETHOD EmitWarning(char* channel, char* message) = 0;
};

FX_DEFINE_GUID(IID_IScriptProfiler, 0x782A4496, 0x2AE3, 0x4C70, 0xB5, 0x4A, 0xFA, 0xD8, 0xFD, 0x8A, 0xEE, 0xFD);

class IScriptProfiler : public fxIBase
{
    public:
        NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptProfiler)
        NS_IMETHOD_(void)
        SetupFxProfiler(void* obj, int32_t resourceId) = 0;
        NS_IMETHOD_(void)
        ShutdownFxProfiler() = 0;
};

FX_DEFINE_GUID(IID_IScriptRuntimeHandler, 0x4720A986, 0xEAA6, 0x4ECC, 0xA3, 0x1F, 0x2C, 0xE2, 0xBB, 0xF5, 0x69, 0xF7);
FX_DEFINE_GUID(CLSID_ScriptRuntimeHandler, 0xC41E7194, 0x7556, 0x4C02, 0xBA, 0x45, 0xA9, 0xC8, 0x4D, 0x18, 0xAD, 0x43);

class IScriptRuntimeHandler : public fxIBase
{
    public:
        NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptRuntimeHandler)
        NS_IMETHOD PushRuntime(IScriptRuntime* runtime) = 0;
        NS_IMETHOD GetCurrentRuntime(IScriptRuntime** runtime) = 0;
        NS_IMETHOD PopRuntime(IScriptRuntime* runtime) = 0;
        NS_IMETHOD GetInvokingRuntime(IScriptRuntime** runtime) = 0;
        NS_IMETHOD TryPushRuntime(IScriptRuntime* runtime) = 0;
};

namespace fx
{

inline fx::OMPtr<IScriptRuntimeHandler> GetRuntimeHandler()
{
        static fx::OMPtr<IScriptRuntimeHandler> h;
        static std::once_flag flag;
        std::call_once(flag, []
        {
                fx::MakeInterface(&h, CLSID_ScriptRuntimeHandler);
        });
        return h;
}

class PushEnvironment
{
        fx::OMPtr<IScriptRuntimeHandler> m_handler;
        fx::OMPtr<IScriptRuntime> m_runtime;

    public:
        PushEnvironment() = default;
        explicit PushEnvironment(IScriptRuntime* rt)
        {
                m_handler = GetRuntimeHandler();
                if (!m_handler.GetRef())
                        return;
                m_runtime = fx::OMPtr<IScriptRuntime>(rt);
                m_handler->PushRuntime(rt);
        }
        PushEnvironment(IScriptRuntimeHandler* handler, IScriptRuntime* rt)
        {
                if (!handler || !rt)
                        return;
                m_handler = fx::OMPtr<IScriptRuntimeHandler>(handler);
                m_runtime = fx::OMPtr<IScriptRuntime>(rt);
                m_handler->PushRuntime(rt);
        }
        ~PushEnvironment()
        {
                if (m_runtime.GetRef() && m_handler.GetRef())
                        m_handler->PopRuntime(m_runtime.GetRef());
        }
        PushEnvironment(const PushEnvironment&) = delete;
        PushEnvironment& operator=(const PushEnvironment&) = delete;
        PushEnvironment(PushEnvironment&& o) noexcept : m_handler(o.m_handler), m_runtime(o.m_runtime)
        {
                o.m_handler = { };
                o.m_runtime = { };
        }
};

inline result_t GetCurrentScriptRuntime(fx::OMPtr<IScriptRuntime>* out)
{
        auto h = GetRuntimeHandler();
        if (!h.GetRef())
                return FX_E_NOTIMPL;
        return h->GetCurrentRuntime(out->ReleaseAndGetAddressOf());
}

}

namespace fx::cpp
{

struct CppBoundary
{
        int64_t hint;
};

class BoundaryGuard
{
        IScriptHost* m_host;

    public:
        BoundaryGuard(IScriptHost* host, int64_t hint) : m_host(host)
        {
                if (m_host)
                {
                        CppBoundary b{ hint };
                        m_host->SubmitBoundaryStart(reinterpret_cast<char*>(&b), sizeof(b));
                }
        }
        ~BoundaryGuard()
        {
                if (m_host)
                        m_host->SubmitBoundaryEnd(nullptr, 0);
        }
        BoundaryGuard(const BoundaryGuard&) = delete;
        BoundaryGuard& operator=(const BoundaryGuard&) = delete;
};

struct WasmNativeCtx
{
        uint64_t hash;
        uint32_t numArgs;
        uint32_t numResults;
        uint64_t args[32];
        uint32_t ptrMask;
        uint32_t resultPtrMask;
};
static_assert(sizeof(WasmNativeCtx) == 280, "WasmNativeCtx layout mismatch");

// {00F3A7B9-241D-5E4C-8A93-2FA1B2C3D4E5}
FX_DEFINE_GUID(CLSID_CppScriptRuntime, 0xF3A7B9, 0x241D, 0x5E4C, 0x8A, 0x93, 0x2F, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5);

class CppScriptRuntime final : public fx::OMClass<CppScriptRuntime, IScriptRuntime, IScriptTickRuntime, IScriptEventRuntime, IScriptRefRuntime, IScriptFileHandlingRuntime, IScriptTickRuntimeWithBookmarks, IScriptStackWalkingRuntime, IScriptMemInfoRuntime, IScriptWarningRuntime, IScriptProfiler>
{
    public:
        CppScriptRuntime();
        ~CppScriptRuntime();
        result_t OM_DECL Create(IScriptHost* host) override;
        result_t OM_DECL Destroy() override;
        void* OM_DECL GetParentObject() override
        {
                return m_parentObject;
        }
        void OM_DECL SetParentObject(void*) override;
        int32_t OM_DECL GetInstanceId() override
        {
                return m_instanceId;
        }
        result_t OM_DECL Tick() override;
        result_t OM_DECL TickBookmarks(uint64_t* bookmarks, int32_t numBookmarks) override;
        result_t OM_DECL TriggerEvent(char* eventName, char* argsSerialized, uint32_t serializedSize, char* sourceId) override;
        result_t OM_DECL CallRef(int32_t refIdx, char* argsSerialized, uint32_t argsSize, IScriptBuffer** retval) override;
        result_t OM_DECL DuplicateRef(int32_t refIdx, int32_t* newRefIdx) override;
        result_t OM_DECL RemoveRef(int32_t refIdx) override;
        int32_t OM_DECL HandlesFile(char* scriptFile, IScriptHostWithResourceData* metadata) override;
        result_t OM_DECL LoadFile(char* scriptFile) override;
        result_t OM_DECL WalkStack(char* boundaryStart, uint32_t boundaryStartLength, char* boundaryEnd, uint32_t boundaryEndLength, IScriptStackWalkVisitor* visitor) override;
        result_t OM_DECL RequestMemoryUsage() override;
        result_t OM_DECL GetMemoryUsage(int64_t* memUsage) override;
        result_t OM_DECL EmitWarning(char* channel, char* message) override;
        void OM_DECL SetupFxProfiler(void* obj, int32_t resourceId) override;
        void OM_DECL ShutdownFxProfiler() override;
        int32_t AddFuncRef(fx::RefCallback cb);
        IScriptHost* host() const
        {
                return m_host.GetRef();
        }
        const std::string& resourceName() const
        {
                return m_resourceName;
        }

        uint8_t* wasmBase();
        size_t wasmMemSize();
        uint32_t wasmAlloc(uint32_t size);
        void wasmFree(uint32_t ptr, uint32_t size);
        bool callVoid(const wasmtime_func_t& fn);
        bool callEvent(uint32_t namePtr, uint32_t nameLen, uint32_t argsPtr, uint32_t argsLen, uint32_t srcPtr, uint32_t srcLen);
        bool callInvokeRef(uint32_t callbackId, const char* argsSerialized, uint32_t argsSize, std::vector<char>& result);
        void wasmMapRef(int32_t refIdx, int32_t callbackId);
        void wasmDuplicateRef(int32_t callbackId);
        void wasmRemoveRef(int32_t callbackId);
        struct WorkerState
        {
                enum Status
                {
                        Running,
                        Done,
                        Error
                };
                std::thread thread;
                std::mutex mutex;
                Status status = Running;
                std::vector<char> result;
        };
        static constexpr int32_t MAX_WORKERS_PER_RESOURCE = 8;
        std::unordered_map<int32_t, std::shared_ptr<WorkerState>> m_workers;
        int32_t m_nextWorkerId = 1;
        wasmtime_module_t* wasmModule() const
        {
                return m_module;
        }
        void scheduleWasmBookmark(int32_t wasmId, int32_t deadlineMs);
        void refuelWasm();
        static wasm_engine_t* engine();

    private:
        fx::OMPtr<IScriptHost> m_host;
        fx::OMPtr<IScriptHostWithBookmarks> m_bookmarkHost;
        fx::OMPtr<IScriptHostWithResourceData> m_metadataHost;
        void* m_parentObject = nullptr;
        int32_t m_instanceId = 0;
        std::string m_resourceName;
        std::unordered_map<int32_t, fx::RefCallback> m_refs;
        uint32_t m_nextRefIdx = 1;
        int32_t allocRefIdx();
        uint64_t m_nextBoundaryId = 1;
        uint64_t nextBoundaryId();

        wasmtime_store_t* m_store = nullptr;
        wasmtime_module_t* m_module = nullptr;
        wasmtime_linker_t* m_linker = nullptr;
        wasmtime_instance_t m_instance{ };
        wasmtime_memory_t m_memory{ };
        bool m_hasMemory = false;
        wasmtime_func_t m_fnTick{ };
        wasmtime_func_t m_fnEvent{ };
        wasmtime_func_t m_fnStop{ };
        wasmtime_func_t m_fnAlloc{ };
        wasmtime_func_t m_fnFree{ };
        wasmtime_func_t m_fnInvokeRef{ };
        wasmtime_func_t m_fnDuplicateRef{ };
        wasmtime_func_t m_fnRemoveRef{ };
        wasmtime_func_t m_fnHasPendingWork{ };
        bool m_hasTickFn = false;
        bool m_hasEventFn = false;
        bool m_hasStopFn = false;
        bool m_hasAllocFn = false;
        bool m_hasFreeFn = false;
        bool m_hasInvokeRefFn = false;
        bool m_hasDuplicateRefFn = false;
        bool m_hasRemoveRefFn = false;
        bool m_hasHasPendingWorkFn = false;
        std::unordered_map<int32_t, int32_t> m_refToCallbackId;
    public:
        bool m_eventCanceled = false;
        bool m_hasValidNativeResult = false;
        fxNativeContext m_lastNativeCtx{ };
        uint32_t m_lastResultPtrMask = 0;
    private:
        wasmtime_func_t m_fnTickBookmarks{ };
        bool m_hasTickBookmarksFn = false;
        std::unordered_map<int32_t, uint64_t> m_wasmToHostBookmark;
        std::unordered_map<uint64_t, int32_t> m_hostToWasmBookmark;
        uint64_t m_nextWasmHostBookmarkId = 1;
        void defineImports();
        bool resolveExports();
        void destroyWasm();
        std::string wasmErrMsg(wasmtime_error_t* err, wasm_trap_t* trap);
        result_t loadWasm(const std::string& resolvedPath);
};

}

#define EXPORTED_TYPE
#ifndef DLL_EXPORT
#define DLL_EXPORT __attribute__((visibility("default")))
#endif

class OMComponentBaseImpl
{
public:
        static OMComponentBaseImpl* ms_instance;
};

class OMComponent
{
public:
        virtual result_t CreateObjectInstance(const guid_t& guid, const guid_t& iid, void** outRef) = 0;
        virtual std::vector<guid_t> GetImplementedClasses(const guid_t& iid) = 0;
};

class Component : public fwRefCountable
{
public:
        virtual bool Initialize() { return true; }
        virtual void SetCommandLine(int, char*[]) { }
        virtual bool SetUserData(const std::string&) { return true; }
        virtual bool Shutdown() { return true; }
        virtual bool DoGameLoad(void*) { return true; }
        virtual bool IsA(uint32_t) { return false; }
        virtual void* As(uint32_t) { return nullptr; }
};

template<typename T>
class OMComponentBase : public Component, public OMComponent
{
public:
        bool IsA(uint32_t type) override
        {
                return type == HashString("OMComponent") || Component::IsA(type);
        }

        void* As(uint32_t type) override
        {
                if (type == HashString("OMComponent"))
                        return static_cast<OMComponent*>(this);
                return Component::As(type);
        }

        result_t CreateObjectInstance(const guid_t& guid, const guid_t& iid, void** outRef) override
        {
                guid_t match = fx::IsNullGuid(guid) ? iid : guid;
                for (auto* f = OMFactoryDef::s_factories; f; f = f->next)
                {
                        if (f->guid == match)
                        {
                                fxIBase* base = f->factory();
                                result_t res = base->QueryInterface(iid, outRef);
                                base->Release();
                                if (res != FX_E_NOINTERFACE)
                                        return res;
                        }
                }
                return FX_E_NOINTERFACE;
        }

        std::vector<guid_t> GetImplementedClasses(const guid_t& iid) override
        {
                std::vector<guid_t> out;
                for (auto* e = OMImplementsDef::s_impls; e; e = e->next)
                        if (e->iid == iid)
                                out.push_back(e->clsid);
                return out;
        }
};

class InitFunctionBase
{
public:
        static void RunAll() { }
};

class HookFunction
{
public:
        static void RunAll() { }
};

#endif
