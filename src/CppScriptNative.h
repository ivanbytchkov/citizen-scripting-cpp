#pragma once

#include "../include/fxScripting.h"

#include <chrono>
#include <coroutine>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

extern "C" {
extern char** environ;
}

namespace fx::json
{

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
                return set(key, static_cast<int64_t>(v));
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

struct Value
{
        enum class Kind
        {
                String,
                Number,
                Bool,
                Null,
                Array,
                Object,
                FuncRef
        };
        Kind kind = Kind::Null;
        std::string scalar;
        std::vector<Value> children;
        std::unordered_map<std::string, Value> fields;
        Value() = default;
        Value(Value&&) noexcept = default;
        Value& operator=(Value&&) noexcept = default;
        Value(const Value&) = default;
        Value& operator=(const Value&) = default;
        Value(int v) : kind(Kind::Number), scalar(std::to_string(v))
        {
        }
        Value(int64_t v) : kind(Kind::Number), scalar(std::to_string(v))
        {
        }
        Value(double v) : kind(Kind::Number)
        {
                char buf[32];
                snprintf(buf, sizeof(buf), "%g", v);
                scalar = buf;
        }
        Value(float v) : Value(static_cast<double>(v))
        {
        }
        Value(bool b) : kind(Kind::Bool), scalar(b ? "true" : "false")
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
                return kind == Kind::Number ? std::strtod(scalar.c_str(), nullptr) : def;
        }
        int asInt(int def = 0) const
        {
                return static_cast<int>(asNum(def));
        }
        float asFloat(float def = 0.f) const
        {
                return static_cast<float>(asNum(def));
        }
        bool asBool(bool def = false) const
        {
                return kind == Kind::Bool ? scalar == "true" : def;
        }
        bool isNull() const
        {
                return kind == Kind::Null;
        }
        bool has(const std::string& key) const
        {
                return fields.count(key) > 0;
        }
        const Value& operator[](const std::string& key) const
        {
                static const Value nil{ };
                auto it = fields.find(key);
                return it != fields.end() ? it->second : nil;
        }
        size_t size() const
        {
                return children.size();
        }
        const Value& at(size_t i) const
        {
                static const Value nil{ };
                return i < children.size() ? children[i] : nil;
        }
};

namespace detail
{

        struct Parser
        {
                std::string_view src;
                size_t pos = 0;
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
                void expect(char c)
                {
                        skipWs();
                        if (consume() != c)
                                throw std::runtime_error("json: unexpected character");
                }
                std::string parseString()
                {
                        expect('"');
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
                                                        if (cp < 0x80)
                                                                out += static_cast<char>(cp);
                                                        else if (cp < 0x800)
                                                        {
                                                                out += static_cast<char>(0xC0 | (cp >> 6));
                                                                out += static_cast<char>(0x80 | (cp & 0x3F));
                                                        }
                                                        else
                                                        {
                                                                out += static_cast<char>(0xE0 | (cp >> 12));
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
                        throw std::runtime_error("json: unterminated string");
                }
                static constexpr int MAX_DEPTH = 128;
                Value parseValue(int depth = 0)
                {
                        if (depth > MAX_DEPTH)
                                throw std::runtime_error("json: nesting depth exceeds 128");
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
                                while (true)
                                {
                                        skipWs();
                                        std::string key = parseString();
                                        skipWs();
                                        expect(':');
                                        v.fields[key] = parseValue(depth + 1);
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
                                                throw std::runtime_error("json: expected , or }");
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
                                while (true)
                                {
                                        v.children.push_back(parseValue(depth + 1));
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
                                                throw std::runtime_error("json: expected , or ]");
                                }
                        }
                        else if (c == 't')
                        {
                                if (pos + 4 > src.size() || src.compare(pos, 4, "true") != 0)
                                        throw std::runtime_error("json: unexpected");
                                v.kind = Value::Kind::Bool;
                                v.scalar = "true";
                                pos += 4;
                        }
                        else if (c == 'f')
                        {
                                if (pos + 5 > src.size() || src.compare(pos, 5, "false") != 0)
                                        throw std::runtime_error("json: unexpected");
                                v.kind = Value::Kind::Bool;
                                v.scalar = "false";
                                pos += 5;
                        }
                        else if (c == 'n')
                        {
                                if (pos + 4 > src.size() || src.compare(pos, 4, "null") != 0)
                                        throw std::runtime_error("json: unexpected");
                                v.kind = Value::Kind::Null;
                                pos += 4;
                        }
                        else
                        {
                                v.kind = Value::Kind::Number;
                                size_t start = pos;
                                if (peek() == '-')
                                        ++pos;
                                while (pos < src.size() && std::isdigit(static_cast<unsigned char>(src[pos])))
                                        ++pos;
                                if (pos < src.size() && src[pos] == '.')
                                {
                                        ++pos;
                                        while (pos < src.size() && std::isdigit(static_cast<unsigned char>(src[pos])))
                                                ++pos;
                                }
                                if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E'))
                                {
                                        ++pos;
                                        if (pos < src.size() && (src[pos] == '+' || src[pos] == '-'))
                                                ++pos;
                                        while (pos < src.size() && std::isdigit(static_cast<unsigned char>(src[pos])))
                                                ++pos;
                                }
                                v.scalar = std::string(src.data() + start, pos - start);
                        }
                        return v;
                }
        };

}

inline Value parse(std::string_view json)
{
        detail::Parser p{ json, 0 };
        return p.parseValue();
}

inline Value makeNull()
{
        return { };
}

inline void ensureArray(Value& v)
{
        if (v.kind != Value::Kind::Array)
        {
                Value wrapper;
                wrapper.kind = Value::Kind::Array;
                wrapper.children.push_back(std::move(v));
                v = std::move(wrapper);
        }
}

}

namespace fx::msgpack
{

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
        void pu64(uint64_t v)
        {
                pu32(static_cast<uint32_t>(v >> 32));
                pu32(static_cast<uint32_t>(v));
        }
        void str(const std::string& s)
        {
                uint32_t n = static_cast<uint32_t>(s.size());
                if (n <= 31)
                        pu8(0xA0 | static_cast<uint8_t>(n));
                else if (n <= 255)
                {
                        pu8(0xD9);
                        pu8(static_cast<uint8_t>(n));
                }
                else if (n <= 65535)
                {
                        pu8(0xDA);
                        pu16(static_cast<uint16_t>(n));
                }
                else
                {
                        pu8(0xDB);
                        pu32(n);
                }
                buf.insert(buf.end(), s.begin(), s.end());
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
                        pu8(static_cast<uint8_t>(v));
                else if (v < 0 && v >= -32)
                        pu8(static_cast<uint8_t>(static_cast<int8_t>(v)));
                else if (v >= -128 && v <= 127)
                {
                        pu8(0xD0);
                        pu8(static_cast<uint8_t>(static_cast<int8_t>(v)));
                }
                else if (v >= -32768 && v <= 32767)
                {
                        pu8(0xD1);
                        pu16(static_cast<uint16_t>(static_cast<int16_t>(v)));
                }
                else if (v >= -2147483648LL && v <= 2147483647LL)
                {
                        pu8(0xD2);
                        pu32(static_cast<uint32_t>(static_cast<int32_t>(v)));
                }
                else
                {
                        pu8(0xD3);
                        pu64(static_cast<uint64_t>(v));
                }
        }
        void encDouble(double d)
        {
                uint64_t bits;
                memcpy(&bits, &d, 8);
                pu8(0xCB);
                for (int i = 7; i >= 0; --i)
                        pu8(static_cast<uint8_t>((bits >> (i * 8)) & 0xFF));
        }
        void encValue(const json::Value& v)
        {
                switch (v.kind)
                {
                        case json::Value::Kind::Null:
                                encNull();
                                break;
                        case json::Value::Kind::Bool:
                                encBool(v.scalar == "true");
                                break;
                        case json::Value::Kind::Number:
                        {
                                char* end = nullptr;
                                long long iv = strtoll(v.scalar.c_str(), &end, 10);
                                if (end && *end == '\0')
                                        encInt(iv);
                                else
                                        encDouble(strtod(v.scalar.c_str(), nullptr));
                                break;
                        }
                        case json::Value::Kind::String:
                                str(v.scalar);
                                break;
                        case json::Value::Kind::Array:
                        {
                                uint32_t n = static_cast<uint32_t>(v.children.size());
                                if (n <= 15)
                                        pu8(0x90 | static_cast<uint8_t>(n));
                                else if (n <= 65535)
                                {
                                        pu8(0xDC);
                                        pu16(static_cast<uint16_t>(n));
                                }
                                else
                                {
                                        pu8(0xDD);
                                        pu32(n);
                                }
                                for (auto& c : v.children)
                                        encValue(c);
                                break;
                        }
                        case json::Value::Kind::Object:
                        {
                                uint32_t n = static_cast<uint32_t>(v.fields.size());
                                if (n <= 15)
                                        pu8(0x80 | static_cast<uint8_t>(n));
                                else if (n <= 65535)
                                {
                                        pu8(0xDE);
                                        pu16(static_cast<uint16_t>(n));
                                }
                                else
                                {
                                        pu8(0xDF);
                                        pu32(n);
                                }
                                for (auto& [key, val] : v.fields)
                                {
                                        str(key);
                                        encValue(val);
                                }
                                break;
                        }
                        case json::Value::Kind::FuncRef:
                        {
                                uint32_t n = static_cast<uint32_t>(v.scalar.size());
                                if (n <= 255)
                                {
                                        pu8(0xC7);
                                        pu8(static_cast<uint8_t>(n));
                                }
                                else if (n <= 65535)
                                {
                                        pu8(0xC8);
                                        pu16(static_cast<uint16_t>(n));
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
                        default:
                                encNull();
                                break;
                }
        }
};

inline std::vector<uint8_t> encode(const json::Value& v)
{
        Writer w;
        w.encValue(v);
        return std::move(w.buf);
}

namespace detail
{
        struct Reader
        {
                const uint8_t* p;
                const uint8_t* end;
                uint8_t u8()
                {
                        if (p >= end)
                                throw std::runtime_error("msgpack: truncated");
                        return *p++;
                }
                uint16_t u16()
                {
                        uint8_t a = u8(), b = u8();
                        return static_cast<uint16_t>((a << 8) | b);
                }
                uint32_t u32()
                {
                        uint16_t a = u16(), b = u16();
                        return (static_cast<uint32_t>(a) << 16) | b;
                }
                uint64_t u64()
                {
                        uint32_t a = u32(), b = u32();
                        return (static_cast<uint64_t>(a) << 32) | b;
                }
                std::string str(size_t n)
                {
                        if (p + n > end)
                                throw std::runtime_error("msgpack: truncated");
                        std::string s(reinterpret_cast<const char*>(p), n);
                        p += n;
                        return s;
                }
                static constexpr int MAX_DEPTH = 128;
                void validate(uint32_t n) const
                {
                        if (n > (1 << 20) || n > static_cast<uint32_t>(end - p))
                                throw std::runtime_error("msgpack: count exceeds data");
                }
                json::Value readArray(uint32_t n, int d)
                {
                        if (d > MAX_DEPTH)
                                throw std::runtime_error("msgpack: too deep");
                        validate(n);
                        json::Value v;
                        v.kind = json::Value::Kind::Array;
                        v.children.reserve(n);
                        for (uint32_t i = 0; i < n; ++i)
                                v.children.push_back(read(d + 1));
                        return v;
                }
                json::Value readMap(uint32_t n, int d)
                {
                        if (d > MAX_DEPTH)
                                throw std::runtime_error("msgpack: too deep");
                        validate(n);
                        json::Value v;
                        v.kind = json::Value::Kind::Object;
                        for (uint32_t i = 0; i < n; ++i)
                        {
                                auto k = read(d + 1);
                                v.fields[k.kind == json::Value::Kind::String ? k.scalar : std::to_string(i)] = read(d + 1);
                        }
                        return v;
                }
                json::Value readStr(uint32_t n)
                {
                        json::Value v;
                        v.kind = json::Value::Kind::String;
                        v.scalar = str(n);
                        return v;
                }
                json::Value readExt(uint32_t n)
                {
                        int8_t t = static_cast<int8_t>(u8());
                        if (t == 10)
                        {
                                json::Value v;
                                v.kind = json::Value::Kind::FuncRef;
                                v.scalar = str(n);
                                return v;
                        }
                        if (p + n > end)
                                throw std::runtime_error("msgpack: truncated");
                        p += n;
                        return { };
                }
                json::Value read(int d = 0)
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
                        if ((b & 0xE0) == 0xA0)
                                return readStr(b & 0x1F);
                        if ((b & 0xF0) == 0x80)
                                return readMap(b & 0x0F, d);
                        if ((b & 0xF0) == 0x90)
                                return readArray(b & 0x0F, d);
                        switch (b)
                        {
                                case 0xC0:
                                        return { };
                                case 0xC2:
                                        v.kind = json::Value::Kind::Bool;
                                        v.scalar = "false";
                                        return v;
                                case 0xC3:
                                        v.kind = json::Value::Kind::Bool;
                                        v.scalar = "true";
                                        return v;
                                case 0xC4:
                                        return readStr(u8());
                                case 0xC5:
                                        return readStr(u16());
                                case 0xC6:
                                        return readStr(u32());
                                case 0xCA:
                                {
                                        uint32_t bits = u32();
                                        float f;
                                        memcpy(&f, &bits, 4);
                                        char buf[32];
                                        snprintf(buf, sizeof(buf), "%g", static_cast<double>(f));
                                        v.kind = json::Value::Kind::Number;
                                        v.scalar = buf;
                                        return v;
                                }
                                case 0xCB:
                                {
                                        uint64_t bits = u64();
                                        double d_;
                                        memcpy(&d_, &bits, 8);
                                        char buf[32];
                                        snprintf(buf, sizeof(buf), "%g", d_);
                                        v.kind = json::Value::Kind::Number;
                                        v.scalar = buf;
                                        return v;
                                }
                                case 0xCC:
                                        v.kind = json::Value::Kind::Number;
                                        v.scalar = std::to_string(u8());
                                        return v;
                                case 0xCD:
                                        v.kind = json::Value::Kind::Number;
                                        v.scalar = std::to_string(u16());
                                        return v;
                                case 0xCE:
                                        v.kind = json::Value::Kind::Number;
                                        v.scalar = std::to_string(u32());
                                        return v;
                                case 0xCF:
                                        v.kind = json::Value::Kind::Number;
                                        v.scalar = std::to_string(u64());
                                        return v;
                                case 0xD0:
                                        v.kind = json::Value::Kind::Number;
                                        v.scalar = std::to_string(static_cast<int8_t>(u8()));
                                        return v;
                                case 0xD1:
                                        v.kind = json::Value::Kind::Number;
                                        v.scalar = std::to_string(static_cast<int16_t>(u16()));
                                        return v;
                                case 0xD2:
                                        v.kind = json::Value::Kind::Number;
                                        v.scalar = std::to_string(static_cast<int32_t>(u32()));
                                        return v;
                                case 0xD3:
                                        v.kind = json::Value::Kind::Number;
                                        v.scalar = std::to_string(static_cast<int64_t>(u64()));
                                        return v;
                                case 0xD9:
                                        return readStr(u8());
                                case 0xDA:
                                        return readStr(u16());
                                case 0xDB:
                                        return readStr(u32());
                                case 0xDC:
                                        return readArray(u16(), d);
                                case 0xDD:
                                        return readArray(u32(), d);
                                case 0xDE:
                                        return readMap(u16(), d);
                                case 0xDF:
                                        return readMap(u32(), d);
                                case 0xC7:
                                        return readExt(u8());
                                case 0xC8:
                                        return readExt(u16());
                                case 0xC9:
                                        return readExt(u32());
                                case 0xD4:
                                        return readExt(1);
                                case 0xD5:
                                        return readExt(2);
                                case 0xD6:
                                        return readExt(4);
                                case 0xD7:
                                        return readExt(8);
                                case 0xD8:
                                        return readExt(16);
                                default:
                                        return { };
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
                return { };
        }
}

}

namespace fx
{

struct Vector3
{
        float x = 0.f, y = 0.f, z = 0.f;
};

class EventArgs
{
    public:
        explicit EventArgs(json::Value arr) : m_arr(std::move(arr))
        {
        }
        size_t size() const
        {
                return m_arr.size();
        }
        template<typename T>
        T get(size_t i) const;
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
                return v.kind == json::Value::Kind::FuncRef ? v.scalar : std::string{ };
        }

    private:
        json::Value m_arr;
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
        return static_cast<float>(number(i));
}
template<>
inline bool EventArgs::get<bool>(size_t i) const
{
        return boolean(i);
}

using EventHandler = std::function<void(const std::string& source, const EventArgs&)>;
using TickHandler = std::function<void()>;
using CommandHandler = std::function<void(const std::string& source, const std::vector<std::string>& args)>;
using ExportHandler = std::function<json::Value(const EventArgs&)>;
using StopHandler = std::function<void()>;
using StateBagChangeHandler = std::function<void(const std::string& bagName, const std::string& key, const json::Value& value, int source, bool replicated)>;
using HttpCallback = std::function<void(int statusCode, const std::string& body, const std::string& headers)>;

struct BookmarkPromise;
using BookmarkHandle = std::coroutine_handle<BookmarkPromise>;

struct TimerEntry
{
        int32_t id;
        std::chrono::steady_clock::time_point nextFire;
        uint32_t intervalMs;
        std::function<void()> callback;
};

}

namespace fx::detail
{

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
void invoke_typed_handler(F& fn, const std::string& source, const EventArgs& args, std::index_sequence<Is...>)
{
        fn(source, args.get<std::decay_t<std::tuple_element_t<Is + 1, ArgsTuple>>>(Is)...);
}

template<typename F>
EventHandler wrap_typed_handler(F&& fn)
{
        using args_tuple = typename fn_traits<std::decay_t<F>>::tuple_type;
        constexpr size_t n = std::tuple_size_v<args_tuple>;
        static_assert(n >= 1, "Handler must accept at least source parameter");
        return [f = std::forward<F>(fn)](const std::string& source, const EventArgs& args) mutable
        {
                invoke_typed_handler<decltype(f), args_tuple>(f, source, args, std::make_index_sequence<n - 1>{ });
        };
}

}

#define FXCPP_RESOURCE_EXPORT __attribute__((visibility("default")))

namespace fx
{

class ResourceContext
{
    public:
        ResourceContext(IScriptHost* host, IScriptRuntime* runtime, std::string name, IScriptRuntimeHandler* handler = nullptr, AddRefFn addRefFn = nullptr, RemoveRefFn removeRefFn = nullptr, ScheduleBookmarkFn scheduleBookmarkFn = nullptr) : m_host(host), m_runtime(runtime), m_name(std::move(name)), m_handler(fx::OMPtr<IScriptRuntimeHandler>(handler)), m_addRef(std::move(addRefFn)), m_removeRef(std::move(removeRefFn)), m_scheduleBookmark(std::move(scheduleBookmarkFn))
        {
                m_host.As(&m_metadataHost);
                m_host.As(&m_manifestHost);
        }
        int32_t on(const std::string& event, EventHandler h)
        {
                auto& handlers = m_eventHandlers[event];
                if (handlers.size() >= 1024)
                {
                        trace("Handler limit reached for event '%s'\n", event.c_str());
                        return -1;
                }
                bool first = handlers.empty();
                int32_t token = m_nextEventHandlerId++;
                handlers.push_back({ token, std::move(h) });
                m_handlerEventMap[token] = event;
                if (first)
                        invokeNative(HashString("REGISTER_RESOURCE_AS_EVENT_HANDLER"), reinterpret_cast<uintptr_t>(event.c_str()));
                return token;
        }
        int32_t onNet(const std::string& event, EventHandler h)
        {
                m_netSafeEvents.insert(event);
                return on(event, std::move(h));
        }
        void registerNetEvent(const std::string& event)
        {
                m_netSafeEvents.insert(event);
        }
        void removeEventHandler(int32_t token)
        {
                auto mapIt = m_handlerEventMap.find(token);
                if (mapIt == m_handlerEventMap.end())
                        return;
                std::string event = mapIt->second;
                m_handlerEventMap.erase(mapIt);
                auto it = m_eventHandlers.find(event);
                if (it == m_eventHandlers.end())
                        return;
                auto& vec = it->second;
                vec.erase(std::remove_if(vec.begin(), vec.end(), [token](const EventHandlerEntry& e)
                          {
                                  return e.id == token;
                          }),
                vec.end());
        }
        void onTick(TickHandler h)
        {
                if (m_tickHandlers.size() >= 256)
                {
                        trace("Tick handler limit reached\n");
                        return;
                }
                m_tickHandlers.push_back(std::move(h));
        }
        void onCommand(const std::string& command, CommandHandler h)
        {
                m_commandHandlers[command].push_back(std::move(h));
                if (!m_addRef)
                {
                        fprintf(stderr, "[citizen-scripting-cpp] onCommand('%s'): no ref support\n", command.c_str());
                        return;
                }
                int32_t refIdx = m_addRef([this, command](const char* argsSerialized, uint32_t argsSize) -> std::vector<char>
                {
                        json::Value args = msgpack::decode(argsSerialized, argsSize);
                        if (args.kind != json::Value::Kind::Array)
                                return { };
                        std::string source = args.size() > 0 ? std::to_string(args.at(0).asInt()) : "0";
                        std::vector<std::string> cmdArgs;
                        if (args.size() > 1 && args.at(1).kind == json::Value::Kind::Array)
                                for (size_t i = 0; i < args.at(1).size(); ++i)
                                        cmdArgs.push_back(args.at(1).at(i).asStr());
                        dispatchCommand(command, source, cmdArgs);
                        return { static_cast<char>(0x90) };
                });
                char* refString = nullptr;
                m_host->CanonicalizeRef(refIdx, m_runtime->GetInstanceId(), &refString);
                if (!refString)
                {
                        if (m_removeRef)
                                m_removeRef(refIdx);
                        return;
                }
                invokeNative(HashString("REGISTER_COMMAND"), reinterpret_cast<uintptr_t>(command.c_str()), reinterpret_cast<uintptr_t>(refString), uintptr_t(0));
                fwFree(refString);
        }
        void dispatchEvent(const std::string& name, const json::Value& args, const std::string& source)
        {
                if (source.size() >= 4 && source.compare(0, 4, "net:") == 0)
                        if (m_netSafeEvents.find(name) == m_netSafeEvents.end())
                                return;
                auto it = m_eventHandlers.find(name);
                if (it == m_eventHandlers.end())
                        return;
                auto handlers = it->second;
                EventArgs ea(args);
                for (auto& entry : handlers)
                {
                        try
                        {
                                entry.handler(source, ea);
                        }
                        catch (const std::exception& e)
                        {
                                trace("Unhandled exception in event '%s': %s\n", name.c_str(), e.what());
                        }
                        catch (...)
                        {
                                trace("Unhandled non-standard exception in event '%s'\n", name.c_str());
                        }
                        if (wasEventCanceled())
                                break;
                }
        }
        void dispatchCommand(const std::string& command, const std::string& source, const std::vector<std::string>& args)
        {
                auto it = m_commandHandlers.find(command);
                if (it == m_commandHandlers.end())
                        return;
                for (auto& h : it->second)
                {
                        try
                        {
                                h(source, args);
                        }
                        catch (const std::exception& e)
                        {
                                trace("Unhandled exception in command '%s': %s\n", command.c_str(), e.what());
                        }
                        catch (...)
                        {
                                trace("Unhandled non-standard exception in command '%s'\n", command.c_str());
                        }
                }
        }
        void onStop(StopHandler h)
        {
                if (m_stopHandlers.size() >= 256)
                {
                        trace("Stop handler limit reached\n");
                        return;
                }
                m_stopHandlers.push_back(std::move(h));
        }
        void dispatchStop()
        {
                auto handlers = std::move(m_stopHandlers);
                m_stopHandlers.clear();
                for (auto& h : handlers)
                {
                        try
                        {
                                h();
                        }
                        catch (const std::exception& e)
                        {
                                trace("Unhandled exception in stop handler: %s\n", e.what());
                        }
                        catch (...)
                        {
                                trace("Unhandled non-standard exception in stop handler\n");
                        }
                }
        }
        int32_t setTimeout(uint32_t ms, std::function<void()> cb)
        {
                return addTimer(ms, 0, std::move(cb));
        }
        int32_t setInterval(uint32_t ms, std::function<void()> cb)
        {
                return addTimer(ms, ms, std::move(cb));
        }
        void clearTimer(int32_t id)
        {
                m_timers.erase(id);
        }
        void dispatchTick()
        {
                auto now = std::chrono::steady_clock::now();
                std::vector<int32_t> expired;
                for (auto& [id, t] : m_timers)
                        if (now >= t.nextFire)
                                expired.push_back(id);
                for (auto id : expired)
                {
                        auto it = m_timers.find(id);
                        if (it == m_timers.end())
                                continue;
                        auto cb = it->second.callback;
                        if (it->second.intervalMs > 0)
                        {
                                auto next = it->second.nextFire + std::chrono::milliseconds(it->second.intervalMs);
                                it->second.nextFire = (next > now) ? next : now;
                        }
                        else
                                m_timers.erase(it);
                        try
                        {
                                cb();
                        }
                        catch (const std::exception& e)
                        {
                                trace("Unhandled exception in timer %d: %s\n", id, e.what());
                        }
                        catch (...)
                        {
                                trace("Unhandled non-standard exception in timer %d\n", id);
                        }
                }
                auto tickHandlers = m_tickHandlers;
                for (auto& h : tickHandlers)
                {
                        try
                        {
                                h();
                        }
                        catch (const std::exception& e)
                        {
                                trace("Unhandled exception in tick handler: %s\n", e.what());
                        }
                        catch (...)
                        {
                                trace("Unhandled non-standard exception in tick handler\n");
                        }
                }
        }
        void addExport(const std::string& name, ExportHandler handler)
        {
                if (!m_addRef)
                        return;
                int32_t refIdx = m_addRef([handler](const char* argsSerialized, uint32_t argsSize) -> std::vector<char>
                {
                        json::Value args = msgpack::decode(argsSerialized, argsSize);
                        json::ensureArray(args);
                        json::Value result = handler(EventArgs(args));
                        json::Value arr;
                        arr.kind = json::Value::Kind::Array;
                        arr.children.push_back(std::move(result));
                        auto encoded = msgpack::encode(arr);
                        return std::vector<char>(encoded.begin(), encoded.end());
                });
                char* refString = nullptr;
                m_host->CanonicalizeRef(refIdx, m_runtime->GetInstanceId(), &refString);
                if (!refString)
                {
                        if (m_removeRef)
                                m_removeRef(refIdx);
                        return;
                }
                std::string exportRef = refString;
                fwFree(refString);
                std::string eventName = "__cfx_export_" + m_name + "_" + name;
                on(eventName, [this, exportRef](const std::string&, EventArgs args)
                {
                        if (args.size() == 0)
                                return;
                        std::string setterRef = args.funcRef(0);
                        if (setterRef.empty())
                                return;
                        json::Value refVal;
                        refVal.kind = json::Value::Kind::FuncRef;
                        refVal.scalar = exportRef;
                        json::Value arr;
                        arr.kind = json::Value::Kind::Array;
                        arr.children.push_back(std::move(refVal));
                        auto payload = msgpack::encode(arr);
                        PushEnvironment env(m_handler.GetRef(), m_runtime);
                        fx::OMPtr<IScriptBuffer> retBuf;
                        m_host->InvokeFunctionReference(const_cast<char*>(setterRef.c_str()), reinterpret_cast<char*>(payload.data()), static_cast<uint32_t>(payload.size()), retBuf.ReleaseAndGetAddressOf());
                });
        }
        json::Value callExport(const std::string& resource, const std::string& name, std::initializer_list<json::Value> args)
        {
                if (!m_addRef)
                        return { };
                auto capturedRef = std::make_shared<std::string>();
                int32_t setterIdx = m_addRef([capturedRef](const char* argsSerialized, uint32_t argsSize) -> std::vector<char>
                {
                        json::Value decoded = msgpack::decode(argsSerialized, argsSize);
                        if (decoded.kind == json::Value::Kind::FuncRef)
                                *capturedRef = decoded.scalar;
                        else if (decoded.kind == json::Value::Kind::Array && decoded.size() > 0 && decoded.at(0).kind == json::Value::Kind::FuncRef)
                                *capturedRef = decoded.at(0).scalar;
                        return { static_cast<char>(0x90) };
                });
                char* setterRefStr = nullptr;
                m_host->CanonicalizeRef(setterIdx, m_runtime->GetInstanceId(), &setterRefStr);
                if (!setterRefStr)
                        return { };
                json::Value setterVal;
                setterVal.kind = json::Value::Kind::FuncRef;
                setterVal.scalar = setterRefStr;
                fwFree(setterRefStr);
                json::Value setterArr;
                setterArr.kind = json::Value::Kind::Array;
                setterArr.children.push_back(std::move(setterVal));
                auto setterPayload = msgpack::encode(setterArr);
                std::string eventName = "__cfx_export_" + resource + "_" + name;
                try
                {
                        invokeNative(HashString("TRIGGER_EVENT_INTERNAL"), reinterpret_cast<uintptr_t>(eventName.c_str()), reinterpret_cast<uintptr_t>(setterPayload.data()), setterPayload.size());
                }
                catch (...)
                {
                        if (m_removeRef)
                                m_removeRef(setterIdx);
                        trace("callExport: exception resolving '%s' from '%s'\n", name.c_str(), resource.c_str());
                        return { };
                }
                if (m_removeRef)
                        m_removeRef(setterIdx);
                if (capturedRef->empty())
                        return { };
                json::Value argArr;
                argArr.kind = json::Value::Kind::Array;
                argArr.children.assign(args.begin(), args.end());
                auto userPayload = msgpack::encode(argArr);
                fx::OMPtr<IScriptBuffer> retBuf;
                try
                {
                        PushEnvironment env(m_handler.GetRef(), m_runtime);
                        m_host->InvokeFunctionReference(const_cast<char*>(capturedRef->c_str()), reinterpret_cast<char*>(userPayload.data()), static_cast<uint32_t>(userPayload.size()), retBuf.ReleaseAndGetAddressOf());
                }
                catch (...)
                {
                        trace("callExport: exception invoking '%s' from '%s'\n", name.c_str(), resource.c_str());
                        return { };
                }
                if (!retBuf.GetRef())
                        return { };
                json::Value result = msgpack::decode(retBuf->GetBytes(), retBuf->GetLength());
                return (result.kind == json::Value::Kind::Array && result.size() > 0) ? result.at(0) : result;
        }
        void trace(const char* fmt, ...)
        {
                va_list ap;
                va_start(ap, fmt);
                va_list ap2;
                va_copy(ap2, ap);
                int len = vsnprintf(nullptr, 0, fmt, ap);
                va_end(ap);
                if (len <= 0)
                {
                        va_end(ap2);
                        return;
                }
                std::string buf(static_cast<size_t>(len), '\0');
                vsnprintf(buf.data(), buf.size() + 1, fmt, ap2);
                va_end(ap2);
                m_host->ScriptTrace(buf.data());
                fprintf(stderr, "[script:%s] %s", m_name.c_str(), buf.c_str());
        }
        void traceStr(const std::string& msg)
        {
                if (msg.empty())
                        return;
                m_host->ScriptTrace(const_cast<char*>(msg.c_str()));
                fprintf(stderr, "[script:%s] %s", m_name.c_str(), msg.c_str());
        }
        std::vector<uint8_t> encodeArgs(std::initializer_list<json::Value> args)
        {
                json::Value arr;
                arr.kind = json::Value::Kind::Array;
                arr.children.assign(args.begin(), args.end());
                return msgpack::encode(arr);
        }
        void emit(const std::string& event, std::initializer_list<json::Value> args = { })
        {
                auto payload = encodeArgs(args);
                invokeNative(HashString("TRIGGER_EVENT_INTERNAL"), reinterpret_cast<uintptr_t>(event.c_str()), reinterpret_cast<uintptr_t>(payload.data()), payload.size());
        }
        void emitNet(const std::string& event, int target, std::initializer_list<json::Value> args = { })
        {
                auto payload = encodeArgs(args);
                std::string targetStr = std::to_string(target);
                invokeNative(HashString("TRIGGER_CLIENT_EVENT_INTERNAL"), reinterpret_cast<uintptr_t>(event.c_str()), reinterpret_cast<uintptr_t>(targetStr.c_str()), reinterpret_cast<uintptr_t>(payload.data()), payload.size());
        }
        void emitLatent(const std::string& event, int target, int bps, std::initializer_list<json::Value> args = { })
        {
                auto payload = encodeArgs(args);
                std::string targetStr = std::to_string(target);
                invokeNative(HashString("TRIGGER_LATENT_CLIENT_EVENT_INTERNAL"), reinterpret_cast<uintptr_t>(event.c_str()), reinterpret_cast<uintptr_t>(targetStr.c_str()), reinterpret_cast<uintptr_t>(payload.data()), payload.size(), static_cast<uintptr_t>(bps));
        }
        void cancelEvent()
        {
                invokeNative(HashString("CANCEL_EVENT"));
        }
        bool wasEventCanceled()
        {
                return static_cast<bool>(invokeNativeResult(HashString("WAS_EVENT_CANCELED")).arguments[0]);
        }
        void performHttpRequest(const std::string& url, HttpCallback cb, const std::string& method = "GET", const std::string& data = "", const std::string& headers = "{}")
        {
                json::JsonObj req;
                req.set("url", url).set("method", method).set("data", data).setRaw("headers", headers);
                std::string payload = req.build();
                auto ctx = invokeNativeResult(HashString("PERFORM_HTTP_REQUEST_INTERNAL"), reinterpret_cast<uintptr_t>(payload.c_str()), static_cast<uintptr_t>(payload.size()));
                int32_t token = static_cast<int32_t>(ctx.arguments[0]);
                if (token == -1)
                        return;
                m_httpCallbacks[token] = std::move(cb);
                if (!m_httpEventRegistered)
                {
                        m_httpEventRegistered = true;
                        on("__cfx_internal:httpResponse", [this](const std::string&, const EventArgs& args)
                        {
                                if (args.size() < 4)
                                        return;
                                int32_t tok = args.get<int>(0);
                                auto it = m_httpCallbacks.find(tok);
                                if (it == m_httpCallbacks.end())
                                        return;
                                auto userCb = std::move(it->second);
                                m_httpCallbacks.erase(it);
                                try
                                {
                                        userCb(args.get<int>(1), args.str(2), args.str(3));
                                }
                                catch (const std::exception& e)
                                {
                                        trace("Unhandled exception in HTTP callback: %s\n", e.what());
                                }
                                catch (...)
                                {
                                        trace("Unhandled exception in HTTP callback\n");
                                }
                        });
                }
        }
        void setStateBagValue(const std::string& bagName, const std::string& key, const json::Value& value, bool replicated = true)
        {
                auto encoded = msgpack::encode(value);
                invokeNative(HashString("SET_STATE_BAG_VALUE"), reinterpret_cast<uintptr_t>(bagName.c_str()), reinterpret_cast<uintptr_t>(key.c_str()), reinterpret_cast<uintptr_t>(encoded.data()), encoded.size(), uintptr_t(replicated ? 1u : 0u));
        }
        void setPlayerState(int id, const std::string& key, const json::Value& v, bool r = true)
        {
                setStateBagValue("player:" + std::to_string(id), key, v, r);
        }
        void setEntityState(int id, const std::string& key, const json::Value& v, bool r = true)
        {
                setStateBagValue("entity:" + std::to_string(id), key, v, r);
        }
        void setGlobalState(const std::string& key, const json::Value& v, bool r = true)
        {
                setStateBagValue("global", key, v, r);
        }
        json::Value getStateBagValue(const std::string& bagName, const std::string& key)
        {
                auto ctx = invokeNativeResult(HashString("GET_STATE_BAG_VALUE"), reinterpret_cast<uintptr_t>(bagName.c_str()), reinterpret_cast<uintptr_t>(key.c_str()));
                const char* data = reinterpret_cast<const char*>(ctx.arguments[0]);
                size_t size = static_cast<size_t>(ctx.arguments[1]);
                return (data && size > 0) ? msgpack::decode(data, static_cast<uint32_t>(size)) : json::makeNull();
        }
        json::Value getPlayerState(int id, const std::string& key)
        {
                return getStateBagValue("player:" + std::to_string(id), key);
        }
        json::Value getEntityState(int id, const std::string& key)
        {
                return getStateBagValue("entity:" + std::to_string(id), key);
        }
        json::Value getGlobalState(const std::string& key)
        {
                return getStateBagValue("global", key);
        }
        bool stateBagHasKey(const std::string& bagName, const std::string& key)
        {
                return static_cast<bool>(invokeNativeResult(HashString("STATE_BAG_HAS_KEY"), reinterpret_cast<uintptr_t>(bagName.c_str()), reinterpret_cast<uintptr_t>(key.c_str())).arguments[0]);
        }
        std::vector<std::string> getStateBagKeys(const std::string& bagName)
        {
                auto ctx = invokeNativeResult(HashString("GET_STATE_BAG_KEYS"), reinterpret_cast<uintptr_t>(bagName.c_str()));
                const char* data = reinterpret_cast<const char*>(ctx.arguments[0]);
                size_t size = static_cast<size_t>(ctx.arguments[1]);
                if (!data || size == 0)
                        return { };
                json::Value arr = msgpack::decode(data, static_cast<uint32_t>(size));
                std::vector<std::string> keys;
                for (size_t i = 0; i < arr.size(); i++)
                        keys.push_back(arr.at(i).asStr());
                return keys;
        }
        int32_t addStateBagChangeHandler(const std::string& keyFilter, const std::string& bagFilter, StateBagChangeHandler handler)
        {
                if (!m_addRef)
                        return -1;
                int32_t refIdx = m_addRef([handler](const char* argsSerialized, uint32_t argsSize) -> std::vector<char>
                {
                        json::Value args = msgpack::decode(argsSerialized, argsSize);
                        json::ensureArray(args);
                        if (args.size() >= 5)
                                handler(args.at(0).asStr(), args.at(1).asStr(), args.at(2), args.at(3).asInt(), args.at(4).asBool());
                        return { static_cast<char>(0x90) };
                });
                char* refString = nullptr;
                m_host->CanonicalizeRef(refIdx, m_runtime->GetInstanceId(), &refString);
                if (!refString)
                {
                        if (m_removeRef)
                                m_removeRef(refIdx);
                        return -1;
                }
                std::string cbRef = refString;
                fwFree(refString);
                const char* keyArg = keyFilter.empty() ? nullptr : keyFilter.c_str();
                const char* bagArg = bagFilter.empty() ? nullptr : bagFilter.c_str();
                auto ctx = invokeNativeResult(HashString("ADD_STATE_BAG_CHANGE_HANDLER"), reinterpret_cast<uintptr_t>(keyArg), reinterpret_cast<uintptr_t>(bagArg), reinterpret_cast<uintptr_t>(cbRef.c_str()));
                int32_t cookie = static_cast<int32_t>(ctx.arguments[0]);
                m_stateBagHandlerRefs[cookie] = refIdx;
                return cookie;
        }
        void removeStateBagChangeHandler(int32_t cookie)
        {
                invokeNative(HashString("REMOVE_STATE_BAG_CHANGE_HANDLER"), static_cast<uintptr_t>(cookie));
                auto it = m_stateBagHandlerRefs.find(cookie);
                if (it != m_stateBagHandlerRefs.end())
                {
                        if (m_removeRef)
                                m_removeRef(it->second);
                        m_stateBagHandlerRefs.erase(it);
                }
        }
        void cleanupStateBagHandlers()
        {
                for (auto& [cookie, refIdx] : m_stateBagHandlerRefs)
                {
                        invokeNative(HashString("REMOVE_STATE_BAG_CHANGE_HANDLER"), static_cast<uintptr_t>(cookie));
                        if (m_removeRef)
                                m_removeRef(refIdx);
                }
                m_stateBagHandlerRefs.clear();
        }
        std::string getResourceMetadata(const std::string& key, int index = 0)
        {
                if (!m_metadataHost.GetRef())
                        return { };
                char* value = nullptr;
                return (FX_SUCCEEDED(m_metadataHost->GetResourceMetaData(const_cast<char*>(key.c_str()), index, &value)) && value) ? std::string(value) : std::string{ };
        }
        int getNumResourceMetadata(const std::string& key)
        {
                if (!m_metadataHost.GetRef())
                        return 0;
                int32_t count = 0;
                m_metadataHost->GetNumResourceMetaData(const_cast<char*>(key.c_str()), &count);
                return count;
        }
        bool isManifestVersionBetween(const guid_t& lower, const guid_t& upper)
        {
                if (!m_manifestHost.GetRef())
                        return false;
                bool result = false;
                return FX_SUCCEEDED(m_manifestHost->IsManifestVersionBetween(lower, upper, &result)) && result;
        }
        bool isManifestVersionV2Between(const std::string& lower, const std::string& upper)
        {
                if (!m_manifestHost.GetRef())
                        return false;
                bool result = false;
                return FX_SUCCEEDED(m_manifestHost->IsManifestVersionV2Between(const_cast<char*>(lower.c_str()), const_cast<char*>(upper.c_str()), &result)) && result;
        }
        std::string getCurrentResourceName()
        {
                auto ctx = invokeNativeResult(HashString("GET_CURRENT_RESOURCE_NAME"));
                const char* s = reinterpret_cast<const char*>(ctx.arguments[0]);
                return s ? std::string(s) : m_name;
        }
        std::string getInvokingResource()
        {
                auto ctx = invokeNativeResult(HashString("GET_INVOKING_RESOURCE"));
                const char* s = reinterpret_cast<const char*>(ctx.arguments[0]);
                return s ? std::string(s) : std::string{ };
        }
        std::string getResourceState(const std::string& resource)
        {
                auto ctx = invokeNativeResult(HashString("GET_RESOURCE_STATE"), reinterpret_cast<uintptr_t>(resource.c_str()));
                const char* s = reinterpret_cast<const char*>(ctx.arguments[0]);
                return s ? std::string(s) : std::string{ "unknown" };
        }
        std::string getResourcePath(const std::string& resource)
        {
                auto ctx = invokeNativeResult(HashString("GET_RESOURCE_PATH"), reinterpret_cast<uintptr_t>(resource.c_str()));
                const char* s = reinterpret_cast<const char*>(ctx.arguments[0]);
                return s ? std::string(s) : std::string{ };
        }
        int getNumResources()
        {
                return static_cast<int>(invokeNativeResult(HashString("GET_NUM_RESOURCES")).arguments[0]);
        }
        std::string getResourceByIndex(int index)
        {
                auto ctx = invokeNativeResult(HashString("GET_RESOURCE_BY_FIND_INDEX"), static_cast<uintptr_t>(index));
                const char* s = reinterpret_cast<const char*>(ctx.arguments[0]);
                return s ? std::string(s) : std::string{ };
        }

        std::string getConvar(const std::string& name, const std::string& defaultValue = "")
        {
                auto ctx = invokeNativeResult(HashString("GET_CONVAR"), reinterpret_cast<uintptr_t>(name.c_str()), reinterpret_cast<uintptr_t>(defaultValue.c_str()));
                const char* s = reinterpret_cast<const char*>(ctx.arguments[0]);
                return s ? std::string(s) : defaultValue;
        }
        int getConvarInt(const std::string& name, int defaultValue = 0)
        {
                return static_cast<int>(invokeNativeResult(HashString("GET_CONVAR_INT"), reinterpret_cast<uintptr_t>(name.c_str()), static_cast<uintptr_t>(defaultValue)).arguments[0]);
        }
        void setConvar(const std::string& name, const std::string& value)
        {
                invokeNative(HashString("SET_CONVAR"), reinterpret_cast<uintptr_t>(name.c_str()), reinterpret_cast<uintptr_t>(value.c_str()));
        }
        void setConvarReplicated(const std::string& name, const std::string& value)
        {
                invokeNative(HashString("SET_CONVAR_REPLICATED"), reinterpret_cast<uintptr_t>(name.c_str()), reinterpret_cast<uintptr_t>(value.c_str()));
        }
        void createThread(BookmarkHandle handle, std::shared_ptr<void> prevent_destruct = { });
        void resumeBookmarks(uint64_t* bookmarks, int32_t numBookmarks);
        void cleanupBookmarks();
        bool hasPendingWork() const
        {
                return !m_tickHandlers.empty() || !m_timers.empty();
        }
        IScriptHost* getHost()
        {
                return m_host.GetRef();
        }
        IScriptRuntime* getRuntime()
        {
                return m_runtime;
        }
        IScriptRuntimeHandler* getRuntimeHandler()
        {
                return m_handler.GetRef();
        }
        IScriptHostWithResourceData* getMetadataHost()
        {
                return m_metadataHost.GetRef();
        }
        const std::string& resourceName() const
        {
                return m_name;
        }
        void traceNativeError()
        {
                char* err = nullptr;
                if (FX_SUCCEEDED(m_host->GetLastErrorText(&err)) && err && err[0])
                        trace("Native error: %s\n", err);
        }
        template<typename... Args>
        void invokeNative(uint64_t hash, Args... args)
        {
                static_assert(sizeof...(args) <= 32);
                fxNativeContext ctx{ };
                ctx.nativeIdentifier = hash;
                size_t idx = 0;
                ((ctx.arguments[idx++] = static_cast<uintptr_t>(args)), ...);
                ctx.numArguments = static_cast<int>(idx);
                if (FX_FAILED(m_host->InvokeNative(ctx)))
                        traceNativeError();
        }
        template<typename... Args>
        fxNativeContext invokeNativeResult(uint64_t hash, Args... args)
        {
                static_assert(sizeof...(args) <= 32);
                fxNativeContext ctx{ };
                ctx.nativeIdentifier = hash;
                ctx.numResults = 3;
                size_t idx = 0;
                ((ctx.arguments[idx++] = static_cast<uintptr_t>(args)), ...);
                ctx.numArguments = static_cast<int>(idx);
                if (FX_FAILED(m_host->InvokeNative(ctx)))
                        traceNativeError();
                return ctx;
        }

    private:
        int32_t addTimer(uint32_t ms, uint32_t intervalMs, std::function<void()> cb)
        {
                if (m_timers.size() >= 8192)
                {
                        trace("Timer limit reached\n");
                        return -1;
                }
                uint32_t id = m_nextTimerId;
                if (++m_nextTimerId == 0)
                        m_nextTimerId = 1;
                m_timers[static_cast<int32_t>(id)] = { static_cast<int32_t>(id), std::chrono::steady_clock::now() + std::chrono::milliseconds(ms), intervalMs, std::move(cb) };
                return static_cast<int32_t>(id);
        }
        fx::OMPtr<IScriptHost> m_host;
        IScriptRuntime* m_runtime = nullptr;
        fx::OMPtr<IScriptRuntimeHandler> m_handler;
        AddRefFn m_addRef;
        RemoveRefFn m_removeRef;
        ScheduleBookmarkFn m_scheduleBookmark;
        fx::OMPtr<IScriptHostWithResourceData> m_metadataHost;
        fx::OMPtr<IScriptHostWithManifest> m_manifestHost;
        std::string m_name;
        struct EventHandlerEntry
        {
                int32_t id;
                EventHandler handler;
        };
        std::unordered_map<std::string, std::vector<EventHandlerEntry>> m_eventHandlers;
        std::unordered_map<int32_t, std::string> m_handlerEventMap;
        int32_t m_nextEventHandlerId = 1;
        std::unordered_map<std::string, std::vector<CommandHandler>> m_commandHandlers;
        std::vector<TickHandler> m_tickHandlers;
        std::unordered_map<int32_t, TimerEntry> m_timers;
        uint32_t m_nextTimerId = 1;
        std::unordered_set<std::string> m_netSafeEvents;
        std::vector<StopHandler> m_stopHandlers;
        std::unordered_map<uint64_t, std::pair<BookmarkHandle, std::shared_ptr<void>>> m_bookmarks;
        uint64_t m_nextBookmarkId = 1;
        std::unordered_map<int32_t, int32_t> m_stateBagHandlerRefs;
        std::unordered_map<int32_t, HttpCallback> m_httpCallbacks;
        bool m_httpEventRegistered = false;
};

namespace detail
{
        inline ResourceContext* g_ctx = nullptr;
        inline void* g_libHandle = nullptr;
}
inline ResourceContext* GetContext()
{
        return detail::g_ctx;
}

}

#define _FXCPP_ENTRY                                                                                  \
        static void _fxcpp_resource_body(fx::ResourceContext&);                                       \
        extern "C" FXCPP_RESOURCE_EXPORT void fxcpp_init(fx::ResourceContext* _ctx, void* _libHandle) \
        {                                                                                             \
                fx::detail::g_ctx = _ctx;                                                             \
                fx::detail::g_libHandle = _libHandle;                                                 \
                _fxcpp_resource_body(*_ctx);                                                          \
        }                                                                                             \
        static void _fxcpp_resource_body([[maybe_unused]] fx::ResourceContext& ctx)
#define Server _FXCPP_ENTRY

namespace fx
{

template<typename F>
inline int32_t on(const std::string& event, F&& handler)
{
        if (auto* ctx = detail::g_ctx)
        {
                if constexpr (std::is_convertible_v<std::decay_t<F>, EventHandler>)
                        return ctx->on(event, EventHandler(std::forward<F>(handler)));
                else
                        return ctx->on(event, detail::wrap_typed_handler(std::forward<F>(handler)));
        }
        return -1;
}
template<typename F>
inline int32_t onNet(const std::string& event, F&& handler)
{
        if (auto* ctx = detail::g_ctx)
        {
                if constexpr (std::is_convertible_v<std::decay_t<F>, EventHandler>)
                        return ctx->onNet(event, EventHandler(std::forward<F>(handler)));
                else
                        return ctx->onNet(event, detail::wrap_typed_handler(std::forward<F>(handler)));
        }
        return -1;
}

inline void registerNetEvent(const std::string& event)
{
        if (auto* ctx = detail::g_ctx)
                ctx->registerNetEvent(event);
}

inline void removeEventHandler(int32_t token)
{
        if (auto* ctx = detail::g_ctx)
                ctx->removeEventHandler(token);
}

inline void onTick(TickHandler h)
{
        if (auto* ctx = detail::g_ctx)
                ctx->onTick(std::move(h));
}

inline void onCommand(const std::string& cmd, CommandHandler h)
{
        if (auto* ctx = detail::g_ctx)
                ctx->onCommand(cmd, std::move(h));
}

inline void onStop(StopHandler h)
{
        if (auto* ctx = detail::g_ctx)
                ctx->onStop(std::move(h));
}

template<typename... T>
inline void trace(const char* fmt, T&&... args)
{
        if (auto* ctx = detail::g_ctx)
                ctx->trace(fmt, std::forward<T>(args)...);
}

inline void traceStr(const std::string& msg)
{
        if (auto* ctx = detail::g_ctx)
                ctx->traceStr(msg);
}

template<typename... T>
inline void emit(const std::string& event, T&&... args)
{
        if (auto* ctx = detail::g_ctx)
                ctx->emit(event, { json::Value(std::forward<T>(args))... });
}

template<typename... T>
inline void emitNet(const std::string& event, int target, T&&... args)
{
        if (auto* ctx = detail::g_ctx)
                ctx->emitNet(event, target, { json::Value(std::forward<T>(args))... });
}

template<typename... T>
inline void emitLatent(const std::string& event, int target, int bps, T&&... args)
{
        if (auto* ctx = detail::g_ctx)
                ctx->emitLatent(event, target, bps, { json::Value(std::forward<T>(args))... });
}

inline void cancelEvent()
{
        if (auto* ctx = detail::g_ctx)
                ctx->cancelEvent();
}

inline bool wasEventCanceled()
{
        return detail::g_ctx ? detail::g_ctx->wasEventCanceled() : false;
}

template<typename F>
inline int32_t onResourceStart(F&& h)
{
        return on("onResourceStart", [h = std::forward<F>(h)](const std::string&, const EventArgs& a)
        {
                if (a.size() > 0)
                        h(a.str(0));
        });
}

template<typename F>
inline int32_t onResourceStop(F&& h)
{
        return on("onResourceStop", [h = std::forward<F>(h)](const std::string&, const EventArgs& a)
        {
                if (a.size() > 0)
                        h(a.str(0));
        });
}

inline void addExport(const std::string& name, ExportHandler h)
{
        if (auto* ctx = detail::g_ctx)
                ctx->addExport(name, std::move(h));
}

inline json::Value callExport(const std::string& resource, const std::string& name, std::initializer_list<json::Value> args = { })
{
        return detail::g_ctx ? detail::g_ctx->callExport(resource, name, args) : json::Value{ };
}

inline int32_t setTimeout(uint32_t ms, std::function<void()> cb)
{
        return detail::g_ctx ? detail::g_ctx->setTimeout(ms, std::move(cb)) : -1;
}

inline int32_t setInterval(uint32_t ms, std::function<void()> cb)
{
        return detail::g_ctx ? detail::g_ctx->setInterval(ms, std::move(cb)) : -1;
}

inline void clearTimer(int32_t id)
{
        if (auto* ctx = detail::g_ctx)
                ctx->clearTimer(id);
}

inline void setStateBagValue(const std::string& bag, const std::string& key, const json::Value& v, bool r = true)
{
        if (auto* ctx = detail::g_ctx)
                ctx->setStateBagValue(bag, key, v, r);
}

inline void setPlayerState(int id, const std::string& key, const json::Value& v, bool r = true)
{
        setStateBagValue("player:" + std::to_string(id), key, v, r);
}

inline void setEntityState(int id, const std::string& key, const json::Value& v, bool r = true)
{
        setStateBagValue("entity:" + std::to_string(id), key, v, r);
}

inline void setGlobalState(const std::string& key, const json::Value& v, bool r = true)
{
        setStateBagValue("global", key, v, r);
}

inline json::Value getStateBagValue(const std::string& bag, const std::string& key)
{
        return detail::g_ctx ? detail::g_ctx->getStateBagValue(bag, key) : json::makeNull();
}

inline json::Value getPlayerState(int id, const std::string& key)
{
        return getStateBagValue("player:" + std::to_string(id), key);
}

inline json::Value getEntityState(int id, const std::string& key)
{
        return getStateBagValue("entity:" + std::to_string(id), key);
}

inline json::Value getGlobalState(const std::string& key)
{
        return getStateBagValue("global", key);
}

inline bool stateBagHasKey(const std::string& bag, const std::string& key)
{
        return detail::g_ctx ? detail::g_ctx->stateBagHasKey(bag, key) : false;
}

inline std::vector<std::string> getStateBagKeys(const std::string& bag)
{
        return detail::g_ctx ? detail::g_ctx->getStateBagKeys(bag) : std::vector<std::string>{ };
}

inline int32_t addStateBagChangeHandler(const std::string& kf, const std::string& bf, StateBagChangeHandler h)
{
        return detail::g_ctx ? detail::g_ctx->addStateBagChangeHandler(kf, bf, std::move(h)) : -1;
}

inline void removeStateBagChangeHandler(int32_t cookie)
{
        if (auto* ctx = detail::g_ctx)
                ctx->removeStateBagChangeHandler(cookie);
}

inline std::string getResourceMetadata(const std::string& key, int index = 0)
{
        return detail::g_ctx ? detail::g_ctx->getResourceMetadata(key, index) : std::string{ };
}

inline int getNumResourceMetadata(const std::string& key)
{
        return detail::g_ctx ? detail::g_ctx->getNumResourceMetadata(key) : 0;
}

inline bool isManifestVersionBetween(const guid_t& l, const guid_t& u)
{
        return detail::g_ctx ? detail::g_ctx->isManifestVersionBetween(l, u) : false;
}

inline bool isManifestVersionV2Between(const std::string& l, const std::string& u)
{
        return detail::g_ctx ? detail::g_ctx->isManifestVersionV2Between(l, u) : false;
}

inline std::string getResourcePath(const std::string& r)
{
        return detail::g_ctx ? detail::g_ctx->getResourcePath(r) : std::string{ };
}

inline std::string getCurrentResourceName()
{
        return detail::g_ctx ? detail::g_ctx->getCurrentResourceName() : std::string{ };
}

inline std::string getInvokingResource()
{
        return detail::g_ctx ? detail::g_ctx->getInvokingResource() : std::string{ };
}

inline std::string getResourceState(const std::string& r)
{
        return detail::g_ctx ? detail::g_ctx->getResourceState(r) : "unknown";
}

inline int getNumResources()
{
        return detail::g_ctx ? detail::g_ctx->getNumResources() : 0;
}

inline std::string getResourceByIndex(int i)
{
        return detail::g_ctx ? detail::g_ctx->getResourceByIndex(i) : std::string{ };
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

inline void performHttpRequest(const std::string& url, HttpCallback cb, const std::string& method = "GET", const std::string& data = "", const std::string& headers = "{}")
{
        if (auto* ctx = detail::g_ctx)
                ctx->performHttpRequest(url, std::move(cb), method, data, headers);
}

inline std::string getConvar(const std::string& name, const std::string& def = "")
{
        return detail::g_ctx ? detail::g_ctx->getConvar(name, def) : def;
}

inline int getConvarInt(const std::string& name, int def = 0)
{
        return detail::g_ctx ? detail::g_ctx->getConvarInt(name, def) : def;
}

inline void setConvar(const std::string& name, const std::string& value)
{
        if (auto* ctx = detail::g_ctx)
                ctx->setConvar(name, value);
}

inline void setConvarReplicated(const std::string& name, const std::string& value)
{
        if (auto* ctx = detail::g_ctx)
                ctx->setConvarReplicated(name, value);
}

inline void setKvp(const std::string& key, const std::string& value)
{
        if (auto* ctx = detail::g_ctx)
                ctx->invokeNative(HashString("SET_RESOURCE_KVP"), reinterpret_cast<uintptr_t>(key.c_str()), reinterpret_cast<uintptr_t>(value.c_str()));
}

inline void setKvpInt(const std::string& key, int value)
{
        if (auto* ctx = detail::g_ctx)
                ctx->invokeNative(HashString("SET_RESOURCE_KVP_INT"), reinterpret_cast<uintptr_t>(key.c_str()), value);
}

inline void setKvpFloat(const std::string& key, float value)
{
        if (auto* ctx = detail::g_ctx)
                ctx->invokeNative(HashString("SET_RESOURCE_KVP_FLOAT"), reinterpret_cast<uintptr_t>(key.c_str()), value);
}

inline std::string getKvpString(const std::string& key)
{
        if (!detail::g_ctx)
                return { };
        auto ctx = detail::g_ctx->invokeNativeResult(HashString("GET_RESOURCE_KVP_STRING"), reinterpret_cast<uintptr_t>(key.c_str()));
        const char* r = reinterpret_cast<const char*>(ctx.arguments[0]);
        return r ? std::string(r) : std::string{ };
}

inline int getKvpInt(const std::string& key)
{
        if (!detail::g_ctx)
                return 0;
        auto ctx = detail::g_ctx->invokeNativeResult(HashString("GET_RESOURCE_KVP_INT"), reinterpret_cast<uintptr_t>(key.c_str()));
        return static_cast<int>(ctx.arguments[0]);
}

inline float getKvpFloat(const std::string& key)
{
        if (!detail::g_ctx)
                return 0.f;
        auto ctx = detail::g_ctx->invokeNativeResult(HashString("GET_RESOURCE_KVP_FLOAT"), reinterpret_cast<uintptr_t>(key.c_str()));
        uint32_t bits = static_cast<uint32_t>(ctx.arguments[0]);
        float f;
        memcpy(&f, &bits, sizeof(f));
        return f;
}

inline void deleteKvp(const std::string& key)
{
        if (auto* ctx = detail::g_ctx)
                ctx->invokeNative(HashString("DELETE_RESOURCE_KVP"), reinterpret_cast<uintptr_t>(key.c_str()));
}

inline void flushKvp()
{
        if (auto* ctx = detail::g_ctx)
                ctx->invokeNative(HashString("FLUSH_RESOURCE_KVP"));
}

inline std::vector<std::string> findKvp(const std::string& prefix)
{
        std::vector<std::string> keys;
        if (!detail::g_ctx)
                return keys;
        auto startCtx = detail::g_ctx->invokeNativeResult(HashString("START_FIND_KVP"), reinterpret_cast<uintptr_t>(prefix.c_str()));
        int handle = static_cast<int>(startCtx.arguments[0]);
        if (handle == -1)
                return keys;
        while (true)
        {
                auto findCtx = detail::g_ctx->invokeNativeResult(HashString("FIND_KVP"), handle);
                const char* r = reinterpret_cast<const char*>(findCtx.arguments[0]);
                if (!r || r[0] == '\0')
                        break;
                keys.push_back(r);
        }
        detail::g_ctx->invokeNative(HashString("END_FIND_KVP"), handle);
        return keys;
}

}

namespace fx
{

struct BookmarkPromise
{
        int64_t waitMs = 0;
        std::exception_ptr exception;
        auto get_return_object();
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
        void unhandled_exception()
        {
                exception = std::current_exception();
        }
};

struct ScriptTask
{
        using promise_type = BookmarkPromise;
        using handle_type = BookmarkHandle;
        handle_type handle;
};

inline auto BookmarkPromise::get_return_object()
{
        return ScriptTask{ BookmarkHandle::from_promise(*this) };
}

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
        void await_suspend(BookmarkHandle h) const noexcept
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
        if (auto* ctx = detail::g_ctx)
        {
                auto stored = std::make_shared<std::decay_t<F>>(std::forward<F>(fn));
                ctx->createThread((*stored)().handle, std::move(stored));
        }
}

inline void ResourceContext::createThread(BookmarkHandle handle, std::shared_ptr<void> prevent_destruct)
{
        if (!m_scheduleBookmark || !handle)
        {
                if (handle)
                        handle.destroy();
                trace("createThread: bookmark scheduling not available\n");
                return;
        }
        if (m_bookmarks.size() >= 4096)
        {
                handle.destroy();
                trace("createThread: bookmark limit reached\n");
                return;
        }
        uint64_t id = m_nextBookmarkId++;
        try
        {
                m_bookmarks[id] = { handle, std::move(prevent_destruct) };
                m_scheduleBookmark(id, 0);
        }
        catch (...)
        {
                m_bookmarks.erase(id);
                handle.destroy();
                trace("createThread: failed to register bookmark\n");
        }
}

inline void ResourceContext::resumeBookmarks(uint64_t* bookmarks, int32_t numBookmarks)
{
        for (int32_t i = 0; i < numBookmarks; i++)
        {
                auto it = m_bookmarks.find(bookmarks[i]);
                if (it == m_bookmarks.end())
                        continue;
                auto handle = it->second.first;
                handle.promise().waitMs = 0;
                try
                {
                        handle.resume();
                }
                catch (...)
                {
                        trace("Exception escaped coroutine resume\n");
                        handle.destroy();
                        m_bookmarks.erase(it);
                        continue;
                }
                if (handle.done())
                {
                        if (handle.promise().exception)
                        {
                                try
                                {
                                        std::rethrow_exception(handle.promise().exception);
                                }
                                catch (const std::exception& e)
                                {
                                        trace("Unhandled exception in thread: %s\n", e.what());
                                }
                                catch (...)
                                {
                                        trace("Unhandled non-standard exception in thread\n");
                                }
                        }
                        handle.destroy();
                        m_bookmarks.erase(it);
                }
                else
                {
                        int64_t waitMs = handle.promise().waitMs;
                        if (m_scheduleBookmark)
                                m_scheduleBookmark(bookmarks[i], -waitMs);
                }
        }
}

inline void ResourceContext::cleanupBookmarks()
{
        for (auto& [id, entry] : m_bookmarks)
                if (entry.first)
                        entry.first.destroy();
        m_bookmarks.clear();
}

}

namespace fx
{

}

#define FXCPP_WORKER(name)                                                                                               \
        static int32_t name##_impl(const char* input, int32_t input_len, char* result, int32_t result_max);              \
        extern "C" __attribute__((visibility("default"))) int32_t name(intptr_t ip, int32_t il, intptr_t rp, int32_t rm) \
        {                                                                                                                \
                return name##_impl(reinterpret_cast<const char*>(ip), il, reinterpret_cast<char*>(rp), rm);              \
        }                                                                                                                \
        static int32_t name##_impl(const char* input, int32_t input_len, char* result, int32_t result_max)

namespace fx
{

struct WorkerResult
{
        int32_t status;
        std::string output;
};

namespace detail
{
        using RawWorkerFn = int32_t (*)(intptr_t, int32_t, intptr_t, int32_t);
        struct NativeWorkerState
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
                std::string result;
        };
        inline std::unordered_map<int32_t, std::unique_ptr<NativeWorkerState>>& workerMap()
        {
                static std::unordered_map<int32_t, std::unique_ptr<NativeWorkerState>> s;
                return s;
        }
        inline int32_t& nextWorkerId()
        {
                static int32_t s = 1;
                return s;
        }
}

static constexpr int32_t MAX_WORKERS_PER_RESOURCE = 16;

inline int32_t createWorker(const std::string& fnName, const std::string& input = "", int32_t resultBufferSize = 65536)
{
        if (static_cast<int32_t>(detail::workerMap().size()) >= MAX_WORKERS_PER_RESOURCE)
                return -3;
        void* sym = dlsym(detail::g_libHandle ? detail::g_libHandle : RTLD_DEFAULT, fnName.c_str());
        if (!sym)
                return -2;
        auto fn = reinterpret_cast<detail::RawWorkerFn>(sym);
        int32_t id = detail::nextWorkerId()++;
        auto state = std::make_unique<detail::NativeWorkerState>();
        auto* sp = state.get();
        detail::workerMap()[id] = std::move(state);
        sp->thread = std::thread([sp, fn, input, resultBufferSize]()
        {
                std::vector<char> buf(resultBufferSize, '\0');
                int32_t written = fn(reinterpret_cast<intptr_t>(input.data()), static_cast<int32_t>(input.size()), reinterpret_cast<intptr_t>(buf.data()), resultBufferSize);
                std::lock_guard<std::mutex> lk(sp->mutex);
                if (written > 0 && written < resultBufferSize)
                        sp->result.assign(buf.data(), written);
                sp->status = detail::NativeWorkerState::Done;
        });
        return id;
}

inline WorkerResult pollWorker(int32_t workerId, int32_t = 0)
{
        WorkerResult result{ };
        auto& map = detail::workerMap();
        auto it = map.find(workerId);
        if (it == map.end())
        {
                result.status = -2;
                return result;
        }
        auto& state = it->second;
        std::lock_guard<std::mutex> lk(state->mutex);
        if (state->status == detail::NativeWorkerState::Running)
        {
                result.status = 0;
                return result;
        }
        if (state->status == detail::NativeWorkerState::Error)
        {
                if (state->thread.joinable())
                        state->thread.join();
                map.erase(it);
                result.status = -1;
                return result;
        }
        result.output = std::move(state->result);
        result.status = result.output.empty() ? 1 : static_cast<int32_t>(result.output.size());
        if (state->thread.joinable())
                state->thread.join();
        map.erase(it);
        return result;
}

}
