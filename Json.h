#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <stdexcept>
#include <cstdlib>
#include <cstdio>
#include <cstdint>

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
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                out += buf;
            }
            else out += c;
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
    JsonObj& set(std::string_view key, const std::string& value) { return set(key, std::string_view(value)) ; }
    JsonObj& set(std::string_view key, const char* value) { return set(key, std::string_view(value)) ; }

    JsonObj& set(std::string_view key, int64_t value)
    {
        append(key, std::to_string(value));
        return *this;
    }
    JsonObj& set(std::string_view key, int value) { return set(key, static_cast<int64_t>(value)) ; }
    JsonObj& set(std::string_view key, uint32_t v) { return set(key, static_cast<int64_t>(v)) ; }
    JsonObj& set(std::string_view key, uint64_t v) { return set(key, static_cast<int64_t>(static_cast<int64_t>(v))) ; }

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
        if (!m_body.empty()) m_body += ',';
        m_body += quote(key);
        m_body += ':';
        m_body += rawVal;
    }

    std::string m_body;
};

inline std::string arrayOf(const std::vector<std::string>& rawValues)
{
    std::string out = "[";
    for (size_t i = 0; i < rawValues.size(); ++i)
    {
        if (i) out += ',';
        out += rawValues[i];
    }
    out += ']';
    return out;
}

struct Value
{
    enum class Kind { String, Number, Bool, Null, Array, Object, FuncRef };

    Kind kind = Kind::Null;
    std::string scalar;
    std::vector<Value> children;
    std::unordered_map<std::string, Value> fields;
    std::string asStr(std::string_view def = "") const
    {
        return kind == Kind::String ? scalar : std::string(def);
    }
    double asNum(double def = 0.0) const
    {
        return kind == Kind::Number ? std::strtod(scalar.c_str(), nullptr) : def;
    }
    int asInt(int def = 0) const { return static_cast<int>(asNum(def)) ; }
    bool asBool(bool def = false) const
    {
        if (kind == Kind::Bool) return scalar == "true";
        return def;
    }
    bool isNull() const { return kind == Kind::Null ; }

    bool has(const std::string& key) const { return fields.count(key) > 0 ; }
    const Value& operator[](const std::string& key) const
    {
        static const Value nil{};
        auto it = fields.find(key);
        return it != fields.end() ? it->second : nil;
    }

    size_t size() const { return children.size() ; }
    const Value& at(size_t i) const
    {
        static const Value nil{};
        return i < children.size() ? children[i] : nil;
    }
};

namespace detail
{

struct Parser
{
    std::string_view src;
    size_t pos = 0;

    char peek() const { return pos < src.size() ? src[pos] : '\0' ; }
    char consume() { return pos < src.size() ? src[pos++] : '\0' ; }

    void skipWs()
    {
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n' || src[pos] == '\r'))
            ++pos;
    }

    void expect(char c)
    {
        skipWs();
        if (consume() != c)
            throw std::runtime_error("citizen-scripting-cpp::json: unexpected character");
    }

    std::string parseString()
    {
        expect('"');
        std::string out;
        while (pos < src.size())
        {
            char c = consume();
            if (c == '"') return out;
            if (c == '\\')
            {
                char e = consume();
                switch (e)
                {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    for (int i = 0; i < 4; ++i)
                    {
                        char h = consume();
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= h - '0';
                        else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                    }
                    if (cp < 0x80) out += static_cast<char>(cp);
                    else if (cp < 0x800) { out += static_cast<char>(0xC0|(cp>>6)); out += static_cast<char>(0x80|(cp&0x3F)); }
                    else { out += static_cast<char>(0xE0|(cp>>12)); out += static_cast<char>(0x80|((cp>>6)&0x3F)); out += static_cast<char>(0x80|(cp&0x3F)); }
                    break;
                }
                default: out += e;
                }
            }
            else out += c;
        }
        throw std::runtime_error("citizen-scripting-cpp::json: unterminated string");
    }

    Value parseValue()
    {
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
            if (peek() == '}') { consume(); return v ; }
            while (true)
            {
                skipWs();
                std::string key = parseString();
                skipWs();
                expect(':');
                v.fields[key] = parseValue();
                skipWs();
                char sep = peek();
                if (sep == ',') { consume(); }
                else if (sep == '}') { consume(); break; }
                else throw std::runtime_error("citizen-scripting-cpp::json: expected , or }");
            }
        }
        else if (c == '[')
        {
            v.kind = Value::Kind::Array;
            consume();
            skipWs();
            if (peek() == ']') { consume(); return v ; }
            while (true)
            {
                v.children.push_back(parseValue());
                skipWs();
                char sep = peek();
                if (sep == ',') { consume(); }
                else if (sep == ']') { consume(); break; }
                else throw std::runtime_error("citizen-scripting-cpp::json: expected , or ]");
            }
        }
        else if (c == 't')
        {
            if (pos + 4 > src.size() || src.substr(pos, 4) != "true")
                throw std::runtime_error("citizen-scripting-cpp::json: unexpected token");
            v.kind = Value::Kind::Bool; v.scalar = "true";
            pos += 4;
        }
        else if (c == 'f')
        {
            if (pos + 5 > src.size() || src.substr(pos, 5) != "false")
                throw std::runtime_error("citizen-scripting-cpp::json: unexpected token");
            v.kind = Value::Kind::Bool; v.scalar = "false";
            pos += 5;
        }
        else if (c == 'n')
        {
            if (pos + 4 > src.size() || src.substr(pos, 4) != "null")
                throw std::runtime_error("citizen-scripting-cpp::json: unexpected token");
            v.kind = Value::Kind::Null;
            pos += 4;
        }
        else
        {
            v.kind = Value::Kind::Number;
            size_t start = pos;
            if (peek() == '-') ++pos;
            while (pos < src.size() && std::isdigit(src[pos])) ++pos;
            if (pos < src.size() && src[pos] == '.')
            {
                ++pos;
                while (pos < src.size() && std::isdigit(src[pos])) ++pos;
            }
            if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E'))
            {
                ++pos;
                if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) ++pos;
                while (pos < src.size() && std::isdigit(src[pos])) ++pos;
            }
            v.scalar = std::string(src.substr(start, pos - start));
        }
        return v;
    }
};

}

inline Value parse(std::string_view json)
{
    detail::Parser p{json, 0};
    return p.parseValue();
}

inline Value makeString(const std::string& s) { Value v; v.kind = Value::Kind::String; v.scalar = s; return v; }
inline Value makeInt(int n) { Value v; v.kind = Value::Kind::Number; v.scalar = std::to_string(n); return v; }
inline Value makeNumber(double n) { Value v; v.kind = Value::Kind::Number; char buf[32]; snprintf(buf, sizeof(buf), "%g", n); v.scalar = buf; return v; }
inline Value makeBool(bool b) { Value v; v.kind = Value::Kind::Bool; v.scalar = b ? "true" : "false"; return v; }
inline Value makeNull() { return {}; }

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
