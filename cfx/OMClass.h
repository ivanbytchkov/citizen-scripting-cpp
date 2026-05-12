#pragma once

#include "fxBase.h"
#include "fxIBase.h"
#include "OMPtr.h"

#include <atomic>
#include <type_traits>

namespace fx
{

template<typename TClass, typename... TInterfaces>
class OMClass : public TInterfaces...
{
    std::atomic<int32_t> m_refCount { 0 };

protected:
    OMClass() = default;
    virtual ~OMClass() = default;
    void* operator new(size_t) noexcept { return nullptr; }
    void* operator new(size_t, void* p) { return p; }

public:
    template<typename TNew, typename... TArg>
    friend OMPtr<TNew> MakeNew(TArg...);
    template<typename TNew, typename... TArg>
    friend fxIBase* MakeNewBase(TArg...);

    virtual result_t OM_DECL QueryInterface(const guid_t& riid, void** outObject) override
    {
        result_t res = FX_E_NOINTERFACE;
        ([&] {
            if (res == FX_E_NOINTERFACE && riid == TInterfaces::GetIID())
            {
                res = FX_S_OK;
                *outObject = static_cast<TInterfaces*>(this);
                AddRef();
            }
        }(), ...);
        if (res == FX_E_NOINTERFACE)
        {
            constexpr guid_t iunknown = { 0, 0, 0, { 0xc0,0,0,0,0,0,0,0x46 } };
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
        auto c = m_refCount.fetch_sub(1) - 1;
        if (c == 0)
        {
            this->~OMClass();
            fwFree(this);
            return 1;
        }
        return 0;
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
    TClass* inst = new(fwAlloc(sizeof(TClass))) TClass(args...);
    inst->AddRef();
    *ret.GetAddressOf() = inst;
    return ret;
}

template<typename TClass, typename... TArg>
inline fxIBase* MakeNewBase(TArg... args)
{
    TClass* inst = new(fwAlloc(sizeof(TClass))) TClass(args...);
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
