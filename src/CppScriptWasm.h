#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <coroutine>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <vector>
#include <functional>

namespace fxw_internal
{

struct Value
{
        enum class Kind
        {
                Null,
                Bool,
                Number,
                String,
                Array,
                FuncRef
        } kind = Kind::Null;
        std::string scalar;
        std::vector<Value> children;
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
        Value(float v) : kind(Kind::Number)
        {
                char buf[32];
                snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
                scalar = buf;
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
        size_t size() const
        {
                return children.size();
        }
        const Value& at(size_t i) const
        {
                static const Value nil;
                return i < children.size() ? children[i] : nil;
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
        Value read(int d = 0)
        {
                if (d > 64 || p >= end || error)
                        return { };
                uint8_t b = u8();
                Value v;
                if ((b & 0x80) == 0)
                {
                        v.kind = Value::Kind::Number;
                        v.scalar = std::to_string(b);
                        return v;
                }
                if ((b & 0xE0) == 0xE0)
                {
                        v.kind = Value::Kind::Number;
                        v.scalar = std::to_string((int8_t)b);
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
                {
                        uint32_t n = b & 0x0F;
                        if (n == 1)
                        {
                                auto key = read(d + 1);
                                auto val = read(d + 1);
                                if (key.kind == Value::Kind::String && key.scalar == "__cfx_functionReference")
                                {
                                        v.kind = Value::Kind::FuncRef;
                                        v.scalar = val.asStr();
                                        return v;
                                }
                                return { };
                        }
                        for (uint32_t i = 0; i < n; ++i)
                        {
                                read(d + 1);
                                read(d + 1);
                        }
                        return { };
                }
                switch (b)
                {
                        case 0xC0:
                                return { };
                        case 0xC2:
                                v.kind = Value::Kind::Bool;
                                v.scalar = "false";
                                return v;
                        case 0xC3:
                                v.kind = Value::Kind::Bool;
                                v.scalar = "true";
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
                        {
                                v.kind = Value::Kind::Number;
                                v.scalar = std::to_string(u8());
                                return v;
                        }
                        case 0xCD:
                        {
                                v.kind = Value::Kind::Number;
                                v.scalar = std::to_string(u16());
                                return v;
                        }
                        case 0xCE:
                        {
                                v.kind = Value::Kind::Number;
                                v.scalar = std::to_string(u32());
                                return v;
                        }
                        case 0xCF:
                        {
                                v.kind = Value::Kind::Number;
                                v.scalar = std::to_string(u64());
                                return v;
                        }
                        case 0xD0:
                        {
                                v.kind = Value::Kind::Number;
                                v.scalar = std::to_string((int8_t)u8());
                                return v;
                        }
                        case 0xD1:
                        {
                                v.kind = Value::Kind::Number;
                                v.scalar = std::to_string((int16_t)u16());
                                return v;
                        }
                        case 0xD2:
                        {
                                v.kind = Value::Kind::Number;
                                v.scalar = std::to_string((int32_t)u32());
                                return v;
                        }
                        case 0xD3:
                        {
                                v.kind = Value::Kind::Number;
                                v.scalar = std::to_string((int64_t)u64());
                                return v;
                        }
                        case 0xCA:
                        {
                                uint32_t bits = u32();
                                float f;
                                memcpy(&f, &bits, 4);
                                char buf[32];
                                snprintf(buf, sizeof(buf), "%g", (double)f);
                                v.kind = Value::Kind::Number;
                                v.scalar = buf;
                                return v;
                        }
                        case 0xCB:
                        {
                                uint64_t bits = u64();
                                double d;
                                memcpy(&d, &bits, 8);
                                char buf[32];
                                snprintf(buf, sizeof(buf), "%g", d);
                                v.kind = Value::Kind::Number;
                                v.scalar = buf;
                                return v;
                        }
                        case 0xDC:
                        {
                                v.kind = Value::Kind::Array;
                                uint32_t n = u16();
                                for (uint32_t i = 0; i < n; ++i)
                                        v.children.push_back(read(d + 1));
                                return v;
                        }
                        case 0xDD:
                        {
                                v.kind = Value::Kind::Array;
                                uint32_t n = u32();
                                for (uint32_t i = 0; i < n; ++i)
                                        v.children.push_back(read(d + 1));
                                return v;
                        }
                        // ext8/ext16/ext32
                        case 0xC7:
                        {
                                uint32_t n = u8();
                                uint8_t t = u8();
                                if (t == 10)
                                {
                                        v.kind = Value::Kind::FuncRef;
                                        v.scalar = str(n);
                                        return v;
                                }
                                p += n;
                                return { };
                        }
                        case 0xC8:
                        {
                                uint32_t n = u16();
                                uint8_t t = u8();
                                if (t == 10)
                                {
                                        v.kind = Value::Kind::FuncRef;
                                        v.scalar = str(n);
                                        return v;
                                }
                                p += n;
                                return { };
                        }
                        case 0xC9:
                        {
                                uint32_t n = u32();
                                uint8_t t = u8();
                                if (t == 10)
                                {
                                        v.kind = Value::Kind::FuncRef;
                                        v.scalar = str(n);
                                        return v;
                                }
                                p += n;
                                return { };
                        }
                        // map16/map32
                        case 0xDE:
                        {
                                uint32_t n = u16();
                                for (uint32_t i = 0; i < n; ++i)
                                {
                                        read(d + 1);
                                        read(d + 1);
                                }
                                return { };
                        }
                        case 0xDF:
                        {
                                uint32_t n = u32();
                                for (uint32_t i = 0; i < n; ++i)
                                {
                                        read(d + 1);
                                        read(d + 1);
                                }
                                return { };
                        }
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
                else
                {
                        pu8(0xDA);
                        pu16((uint16_t)n);
                }
                buf.insert(buf.end(), s.begin(), s.end());
        }
        void arrayHeader(uint32_t n)
        {
                if (n <= 15)
                        pu8(0x90 | (uint8_t)n);
                else
                {
                        pu8(0xDC);
                        pu16((uint16_t)n);
                }
        }
        void mapHeader(uint32_t n)
        {
                if (n <= 15)
                        pu8(0x80 | (uint8_t)n);
                else
                {
                        pu8(0xDE);
                        pu16((uint16_t)n);
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
                                encBool(v.scalar == "true");
                                break;
                        case Value::Kind::Number:
                        {
                                char* end = nullptr;
                                long long iv = strtoll(v.scalar.c_str(), &end, 10);
                                if (end && *end == '\0')
                                        encInt(iv);
                                else
                                        encDouble(strtod(v.scalar.c_str(), nullptr));
                                break;
                        }
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

extern "C" {
__attribute__((import_module("fxcpp"), import_name("trace"))) void __fxcpp_trace(const char* ptr, uint32_t len);

__attribute__((import_module("fxcpp"), import_name("invoke_native"))) void __fxcpp_invoke_native(uint32_t ctx_ptr);

__attribute__((import_module("fxcpp"), import_name("copy_string_result"))) int32_t __fxcpp_copy_string_result(uint32_t ctx_ptr, int32_t result_idx, char* buf, int32_t buf_max);

__attribute__((import_module("fxcpp"), import_name("emit_event"))) void __fxcpp_emit_event(const char* name, uint32_t name_len, const uint8_t* args, uint32_t args_len);

__attribute__((import_module("fxcpp"), import_name("emit_net_event"))) void __fxcpp_emit_net_event(const char* name, uint32_t name_len, int32_t target, const uint8_t* args, uint32_t args_len);

__attribute__((import_module("fxcpp"), import_name("cancel_event"))) void __fxcpp_cancel_event();

__attribute__((import_module("fxcpp"), import_name("was_event_canceled"))) int32_t __fxcpp_was_event_canceled();

__attribute__((import_module("fxcpp"), import_name("get_resource_metadata"))) int32_t __fxcpp_get_resource_metadata(const char* key, uint32_t key_len, int32_t index, char* buf, int32_t buf_max);

__attribute__((import_module("fxcpp"), import_name("get_num_resource_metadata"))) int32_t __fxcpp_get_num_resource_metadata(const char* key, uint32_t key_len);

__attribute__((import_module("fxcpp"), import_name("create_ref"))) int32_t __fxcpp_create_ref(int32_t callback_id);

__attribute__((import_module("fxcpp"), import_name("canonicalize_ref"))) int32_t __fxcpp_canonicalize_ref(int32_t ref_idx, char* buf, int32_t buf_max);

__attribute__((import_module("fxcpp"), import_name("remove_ref"))) void __fxcpp_remove_ref(int32_t ref_idx);

__attribute__((import_module("fxcpp"), import_name("invoke_function_reference"))) void __fxcpp_invoke_function_reference(const char* ref_str, uint32_t ref_len, const char* args, uint32_t args_len, void* out);

__attribute__((import_module("fxcpp"), import_name("get_instance_id"))) int32_t __fxcpp_get_instance_id();

__attribute__((import_module("fxcpp"), import_name("spawn_process"))) int32_t __fxcpp_spawn_process(const char* cmd, uint32_t cmd_len, char* out_buf, int32_t out_buf_max);

__attribute__((import_module("fxcpp"), import_name("create_worker"))) int32_t __fxcpp_create_worker(const char* fn_name, uint32_t fn_name_len, const char* input, uint32_t input_len);

__attribute__((import_module("fxcpp"), import_name("poll_worker"))) int32_t __fxcpp_poll_worker(int32_t worker_id, char* out_buf, int32_t out_buf_max);

__attribute__((import_module("fxcpp"), import_name("schedule_bookmark"))) void __fxcpp_schedule_bookmark(int32_t bookmark_id, int32_t deadline_ms);

__attribute__((import_module("fxcpp"), import_name("is_manifest_version_v2_between"))) int32_t __fxcpp_is_manifest_version_v2_between(const char* lower, uint32_t lower_len, const char* upper, uint32_t upper_len);
}

#if __cpp_exceptions
#endif

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
                        if (__fxcpp_was_event_canceled())
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
        int32_t id = fxw_internal::nextCallbackId()++;
        fxw_internal::refCallbacks()[id] = std::move(cb);
        fxw_internal::refCounts()[id] = 1;
        int32_t hostRef = __fxcpp_create_ref(id);
        return hostRef;
}

inline std::string canonicalizeRef(int32_t refIdx)
{
        int32_t len = __fxcpp_canonicalize_ref(refIdx, nullptr, 0);
        if (len <= 0)
                return { };
        std::string out(static_cast<size_t>(len), '\0');
        __fxcpp_canonicalize_ref(refIdx, out.data(), len + 1);
        return out;
}

inline void removeRef(int32_t refIdx)
{
        __fxcpp_remove_ref(refIdx);
}

inline std::vector<uint8_t> invokeFunctionReference(const std::string& ref, const uint8_t* args, uint32_t argsLen)
{
        struct
        {
                uint32_t ptr;
                uint32_t len;
        } out{ };
        __fxcpp_invoke_function_reference(ref.c_str(), static_cast<uint32_t>(ref.size()), reinterpret_cast<const char*>(args), argsLen, &out);
        if (!out.ptr || !out.len)
                return { };
        auto* data = reinterpret_cast<uint8_t*>(out.ptr);
        std::vector<uint8_t> result(data, data + out.len);
        free(data);
        return result;
}

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
        __fxcpp_invoke_native(reinterpret_cast<uint32_t>(&ctx));
        return ctx;
}

inline std::string getStringResult(NativeCtx& ctx, int32_t resultIdx)
{
        int32_t len = __fxcpp_copy_string_result(
        reinterpret_cast<uint32_t>(&ctx), resultIdx, nullptr, 0);
        if (len <= 0)
                return { };
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
                NativeCtx ctx{ };
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

namespace fx
{

template<typename F>
inline int32_t on(const std::string& event, F&& handler)
{
        EventHandler h;
        if constexpr (std::is_convertible_v<std::decay_t<F>, EventHandler>)
                h = EventHandler(std::forward<F>(handler));
        else
                h = detail::wrap_typed_handler(std::forward<F>(handler));
        if (auto* c = fxw_internal::currentContext())
        {
                bool first = c->events.find(event) == c->events.end() || c->events[event].empty();
                int32_t token = c->nextEventHandlerId++;
                c->events[event].push_back({ token, std::move(h) });
                c->handlerEventMap[token] = event;
                if (first)
                {
                        NativeCtx ctx{ };
                        ctx.hash = HashString("REGISTER_RESOURCE_AS_EVENT_HANDLER");
                        ctx.numArgs = 1;
                        ctx.args[0] = reinterpret_cast<uint64_t>(event.c_str());
                        ctx.ptrMask = 1;
                        __fxcpp_invoke_native(reinterpret_cast<uint32_t>(&ctx));
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
        __fxcpp_emit_event(event.c_str(), static_cast<uint32_t>(event.size()), w.buf.data(), static_cast<uint32_t>(w.buf.size()));
}

template<typename... TArgs>
inline void emitNet(const std::string& event, int target, TArgs&&... vals)
{
        fxw_internal::Writer w;
        w.arrayHeader(static_cast<uint32_t>(sizeof...(vals)));
        (fxw_internal::encodeOne(w, std::forward<TArgs>(vals)), ...);
        __fxcpp_emit_net_event(event.c_str(), static_cast<uint32_t>(event.size()), target, w.buf.data(), static_cast<uint32_t>(w.buf.size()));
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
        __fxcpp_cancel_event();
}

inline bool wasEventCanceled()
{
        return __fxcpp_was_event_canceled() != 0;
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

}

namespace fx
{

template<typename... TArgs>
inline void trace(const char* fmt, TArgs&&... args)
{
        if constexpr (sizeof...(args) == 0)
        {
                if (fmt && fmt[0])
                        __fxcpp_trace(fmt, static_cast<uint32_t>(strlen(fmt)));
        }
        else
        {
                int len = snprintf(nullptr, 0, fmt, std::forward<TArgs>(args)...);
                if (len <= 0)
                        return;
                std::string buf(static_cast<size_t>(len), '\0');
                snprintf(buf.data(), buf.size() + 1, fmt, std::forward<TArgs>(args)...);
                __fxcpp_trace(buf.data(), static_cast<uint32_t>(buf.size()));
        }
}

inline void traceStr(const std::string& msg)
{
        if (!msg.empty())
                __fxcpp_trace(msg.data(), static_cast<uint32_t>(msg.size()));
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

}

namespace fx
{

inline std::string getResourceMetadata(const std::string& key, int index = 0)
{
        int32_t len = __fxcpp_get_resource_metadata(key.c_str(), static_cast<uint32_t>(key.size()), index, nullptr, 0);
        if (len <= 0)
                return { };
        std::string out(static_cast<size_t>(len), '\0');
        __fxcpp_get_resource_metadata(key.c_str(), static_cast<uint32_t>(key.size()), index, out.data(), len + 1);
        return out;
}

inline int getNumResourceMetadata(const std::string& key)
{
        return __fxcpp_get_num_resource_metadata(key.c_str(), static_cast<uint32_t>(key.size()));
}

inline bool isManifestVersionV2Between(const std::string& lower, const std::string& upper)
{
        return __fxcpp_is_manifest_version_v2_between(lower.c_str(), static_cast<uint32_t>(lower.size()), upper.c_str(), static_cast<uint32_t>(upper.size())) != 0;
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

}

namespace fx
{

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
                return { 0x90 };
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

}

namespace fx
{

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
                return { 0x90 };
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
        __fxcpp_emit_event(eventName.c_str(), static_cast<uint32_t>(eventName.size()), setterPayload.data(), static_cast<uint32_t>(setterPayload.size()));
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

}

namespace fx
{

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
                return { 0x90 };
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

namespace detail
{
        inline std::string jsonEscape(const std::string& s)
        {
                std::string out;
                out.reserve(s.size());
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
                                        out += c;
                        }
                }
                return out;
        }
}

inline void performHttpRequest(const std::string& url, HttpCallback cb, const std::string& method = "GET", const std::string& data = "", const std::string& headers = "{}")
{
        std::string payload = "{\"url\":\"" + detail::jsonEscape(url) + "\",\"method\":\"" + detail::jsonEscape(method) + "\",\"data\":\"" + detail::jsonEscape(data) + "\",\"headers\":" + headers + "}";
        auto ctx = invokeNative(HashString("PERFORM_HTTP_REQUEST_INTERNAL"), { NativeArg::ptr(payload.c_str()), NativeArg(static_cast<int32_t>(payload.size())) }, 1);
        int32_t token = static_cast<int32_t>(ctx.args[0]);
        if (token == -1)
                return;
        auto* c = fxw_internal::currentContext();
        if (!c)
                return;
        static std::unordered_map<int32_t, HttpCallback> s_httpCallbacks;
        static bool s_registered = false;
        s_httpCallbacks[token] = std::move(cb);
        if (!s_registered)
        {
                s_registered = true;
                on("__cfx_internal:httpResponse", [](const std::string&, const EventArgs& args)
                {
                        if (args.size() < 4)
                                return;
                        int32_t tok = args.get<int>(0);
                        auto it = s_httpCallbacks.find(tok);
                        if (it == s_httpCallbacks.end())
                                return;
                        auto userCb = std::move(it->second);
                        s_httpCallbacks.erase(it);
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

}

namespace fx
{

inline int32_t setTimeout(uint32_t ms, std::function<void()> cb)
{
        auto* c = fxw_internal::currentContext();
        if (!c)
                return -1;
        if (c->timers.size() >= 8192)
                return -1;
        int32_t id = c->nextTimerId;
        if (++c->nextTimerId <= 0)
                c->nextTimerId = 1;
        auto fire = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        c->timers[id] = { id, fire, 0, std::move(cb) };
        return id;
}

inline int32_t setInterval(uint32_t ms, std::function<void()> cb)
{
        auto* c = fxw_internal::currentContext();
        if (!c)
                return -1;
        if (c->timers.size() >= 8192)
                return -1;
        int32_t id = c->nextTimerId;
        if (++c->nextTimerId <= 0)
                c->nextTimerId = 1;
        auto fire = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        c->timers[id] = { id, fire, ms, std::move(cb) };
        return id;
}

inline void clearTimer(int32_t id)
{
        auto* c = fxw_internal::currentContext();
        if (c)
                c->timers.erase(id);
}

}

namespace fx
{

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

#if __cpp_exceptions
#endif

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

inline std::unordered_map<uint64_t, CoroutineEntry>& coroutines()
{
        static std::unordered_map<uint64_t, CoroutineEntry> s_map;
        return s_map;
}

inline uint64_t& nextCoroutineId()
{
        static uint64_t s_id = 1;
        return s_id;
}

inline void resumeCoroutineById(uint64_t id)
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
                __fxcpp_schedule_bookmark(static_cast<int32_t>(id), static_cast<int32_t>(waitMs));
        }
}

inline void resumeCoroutines()
{
        auto& coros = coroutines();
        if (coros.empty())
                return;
        auto now = std::chrono::steady_clock::now();
        std::vector<uint64_t> ready;
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
        if (coros.size() >= 4096)
                return;
        auto stored = std::make_shared<std::decay_t<F>>(std::forward<F>(fn));
        auto task = (*stored)();
        uint64_t id = fxw_internal::nextCoroutineId()++;
        coros[id] = { task.handle, std::move(stored), std::chrono::steady_clock::now() };
        __fxcpp_schedule_bookmark(static_cast<int32_t>(id), 0);
}

}

namespace fx
{

struct ProcessResult
{
        int32_t status; // bytes written on success, -1 perission denied, -2 error
        std::string output;
};

inline ProcessResult spawnProcess(const std::string& command, int32_t maxOutput = 65536)
{
        ProcessResult result{ };
        std::string buf(static_cast<size_t>(maxOutput), '\0');
        result.status = __fxcpp_spawn_process(command.c_str(), static_cast<uint32_t>(command.size()), buf.data(), maxOutput);
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
        return __fxcpp_create_worker(fnName.c_str(), static_cast<uint32_t>(fnName.size()), input.c_str(), static_cast<uint32_t>(input.size()));
}

inline WorkerResult pollWorker(int32_t workerId, int32_t maxOutput = 65536)
{
        WorkerResult result{ };
        std::string buf(static_cast<size_t>(maxOutput), '\0');
        result.status = __fxcpp_poll_worker(workerId, buf.data(), maxOutput);
        if (result.status > 0)
                buf.resize(static_cast<size_t>(result.status));
        else
                buf.clear();
        result.output = std::move(buf);
        return result;
}

}

#define FXCPP_WASM_EXPORT(name) extern "C" __attribute__((export_name(#name)))

FXCPP_WASM_EXPORT(fxcpp_alloc)
void* fxcpp_alloc(uint32_t size)
{
        return malloc(size);
}

FXCPP_WASM_EXPORT(fxcpp_free)
void fxcpp_free(void* ptr, uint32_t)
{
        free(ptr);
}

FXCPP_WASM_EXPORT(fxcpp_has_pending_work)
int32_t fxcpp_has_pending_work()
{
        if (!fxw_internal::coroutines().empty())
                return 1;
        auto* c = fxw_internal::currentContext();
        if (!c)
                return 0;
        return (!c->ticks.empty() || !c->timers.empty()) ? 1 : 0;
}

FXCPP_WASM_EXPORT(fxcpp_tick)
void fxcpp_tick()
{
        fxw_internal::resumeCoroutines();
        if (auto* c = fxw_internal::currentContext())
                c->dispatchTick();
}

FXCPP_WASM_EXPORT(fxcpp_tick_bookmarks)
void fxcpp_tick_bookmarks(int32_t* bookmarks, int32_t count)
{
        for (int32_t i = 0; i < count; ++i)
                fxw_internal::resumeCoroutineById(static_cast<uint64_t>(bookmarks[i]));
}

FXCPP_WASM_EXPORT(fxcpp_on_event)
void fxcpp_on_event(const char* name, uint32_t nameLen, const uint8_t* args, uint32_t argsLen, const char* src, uint32_t srcLen)
{
        if (auto* c = fxw_internal::currentContext())
                c->dispatchEvent(name, nameLen, args, argsLen, src, srcLen);
}

FXCPP_WASM_EXPORT(fxcpp_on_stop)
void fxcpp_on_stop()
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
int32_t fxcpp_invoke_ref(int32_t callback_id, const uint8_t* args_ptr, uint32_t args_len, uint8_t* result_ptr, uint32_t result_max)
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
int32_t fxcpp_duplicate_ref(int32_t callback_id)
{
        auto& counts = fxw_internal::refCounts();
        auto it = counts.find(callback_id);
        if (it != counts.end())
                ++it->second;
        return callback_id;
}

FXCPP_WASM_EXPORT(fxcpp_remove_ref)
void fxcpp_remove_ref(int32_t callback_id)
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
                        int32_t len = __fxcpp_get_resource_metadata(k, sizeof(k) - 1, 0, nullptr, 0);                   \
                        if (len > 0)                                                                                    \
                        {                                                                                               \
                                s_ctx.resourceName.resize(static_cast<size_t>(len));                                    \
                                __fxcpp_get_resource_metadata(k, sizeof(k) - 1, 0, s_ctx.resourceName.data(), len + 1); \
                        }                                                                                               \
                }                                                                                                       \
                _fxcpp_wasm_body();                                                                                     \
        }                                                                                                               \
        static void _fxcpp_wasm_body()
#define Server FXCPP_WASM_ENTRY
