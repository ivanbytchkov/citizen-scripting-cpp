#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>
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
#define EXPORTED_TYPE __attribute__((visibility("default")))

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
        OMPtr& operator=(const OMPtr& o)
        {
                if (o.m_ref)
                        o.m_ref->AddRef();
                if (m_ref)
                        m_ref->Release();
                m_ref = o.m_ref;
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
        OMPtr<TClass> ret;
        TClass* inst = new (fwAlloc(sizeof(TClass))) TClass(args...);
        inst->AddRef();
        *ret.GetAddressOf() = inst;
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
