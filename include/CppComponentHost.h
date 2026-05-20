#pragma once

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
                if (this != &o)
                {
                        if (m_ref)
                                m_ref->Release();
                        m_ref = o.m_ref;
                        o.m_ref = nullptr;
                }
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
                assert(!m_ref && "GetAddressOf() on non-empty OMPtr leaks the existing ref");
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
                if (!ptr)
                        return FX_E_INVALIDARG;
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
        friend OMPtr<TNew> MakeNew(TArg&&...);
        template<typename TNew, typename... TArg>
        friend fxIBase* MakeNewBase(TArg&&...);
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
                return static_cast<uint32_t>(m_refCount.fetch_add(1, std::memory_order_relaxed) + 1);
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
inline OMPtr<TClass> MakeNew(TArg&&... args)
{
        TClass* inst = new (fwAlloc(sizeof(TClass))) TClass(std::forward<TArg>(args)...);
        OMPtr<TClass> ret(inst);
        return ret;
}

template<typename TClass, typename... TArg>
inline fxIBase* MakeNewBase(TArg&&... args)
{
        TClass* inst = new (fwAlloc(sizeof(TClass))) TClass(std::forward<TArg>(args)...);
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

inline const std::vector<std::string>& cachedFilteredEnv()
{
        static const std::vector<std::string> s_env = []
        {
                static constexpr std::string_view kAllowedPrefixes[] = {
                        "PATH=", "HOME=", "USER=", "LOGNAME=", "LANG=", "LANGUAGE=", "LC_", "TERM=", "TZ=", "TMPDIR=", "TEMP=", "TMP=",
                };
                std::vector<std::string> out;
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
                        bool allowed = false;
                        for (auto prefix : kAllowedPrefixes)
                        {
                                if (entry.size() >= prefix.size() && entry.substr(0, prefix.size()) == prefix)
                                {
                                        allowed = true;
                                        break;
                                }
                        }
                        if (allowed)
                                out.emplace_back(entry);
                        pos = end + 1;
                }
                return out;
        }();
        return s_env;
}

inline ProcessResult spawnProcess(const std::string& command, size_t maxOutputBytes = 1048576, int timeoutMs = 30000)
{
        ProcessResult result{ };
        int pipefd[2];
        if (pipe(pipefd) < 0)
        {
                result.status = -2;
                return result;
        }
        const auto& envStrs = cachedFilteredEnv();
        std::vector<char*> envp;
        for (const auto& s : envStrs)
                envp.push_back(const_cast<char*>(s.data()));
        envp.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
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
        pid_t wp = waitpid(pid, &wstatus, WNOHANG);
        if (wp == 0)
        {
                if (timedOut || outputCapped)
                        kill(pid, SIGKILL);
                waitpid(pid, &wstatus, 0);
        }
        else if (wp < 0)
        {
                wstatus = 0;
        }
        while (!result.output.empty() && result.output.back() == '\n')
                result.output.pop_back();
        if (timedOut)
        {
                result.status = -3;
                return result;
        }
        result.status = static_cast<int32_t>(result.output.size());
        if (WIFEXITED(wstatus))
                result.exitCode = WEXITSTATUS(wstatus);
        else if (WIFSIGNALED(wstatus))
                result.exitCode = 128 + WTERMSIG(wstatus);
        else
                result.exitCode = -1;
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

FX_DEFINE_GUID(IID_IScriptFileHandlingRuntime, 0x567634C6, 0x3BDD, 0x4D0E, 0xAF, 0x39, 0x74, 0x72, 0xAE, 0xD4, 0x79, 0xB7);

class IScriptFileHandlingRuntime : public fxIBase
{
public:
        NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptFileHandlingRuntime)
        NS_IMETHOD_(int32_t)
        HandlesFile(char* scriptFile, IScriptHostWithResourceData* metadata) = 0;
        NS_IMETHOD LoadFile(char* scriptFile) = 0;
};

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

inline const fx::OMPtr<IScriptRuntimeHandler>& GetRuntimeHandler()
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
        PushEnvironment(PushEnvironment&& o) noexcept : m_handler(std::move(o.m_handler)), m_runtime(std::move(o.m_runtime))
        {
        }
};

inline result_t GetCurrentScriptRuntime(fx::OMPtr<IScriptRuntime>* out)
{
        const auto& h = GetRuntimeHandler();
        if (!h.GetRef())
                return FX_E_NOTIMPL;
        return h->GetCurrentRuntime(out->ReleaseAndGetAddressOf());
}

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
