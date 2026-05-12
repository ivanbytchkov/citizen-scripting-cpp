#pragma once

#include "fxBase.h"
#include "fxIBase.h"

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
        if (m_ref && m_ref->Release()) m_ref = nullptr;
    }

    OMPtr(T* ref) : m_ref(ref) { if (m_ref) m_ref->AddRef(); }
    OMPtr(const OMPtr& o) : m_ref(o.m_ref) { if (m_ref) m_ref->AddRef(); }
    OMPtr& operator=(const OMPtr& o)
    {
        if (o.m_ref) o.m_ref->AddRef();
        if (m_ref) m_ref->Release();
        m_ref = o.m_ref;
        return *this;
    }

    T* GetRef() const { return m_ref; }
    T* operator->() const { return m_ref; }
    explicit operator bool() const { return m_ref != nullptr; }

    T** GetAddressOf() { return &m_ref; }
    T** ReleaseAndGetAddressOf()
    {
        if (m_ref) { if (m_ref->Release()) m_ref = nullptr; }
        return &m_ref;
    }

    result_t CopyTo(T** ptr) const
    {
        if (m_ref) m_ref->AddRef();
        *ptr = m_ref;
        return FX_S_OK;
    }

    template<class TOther>
    result_t As(OMPtr<TOther>* p)
    {
        if (!m_ref) return FX_E_INVALIDARG;
        return m_ref->QueryInterface(TOther::GetIID(), reinterpret_cast<void**>(p->ReleaseAndGetAddressOf()));
    }
};

template<typename TInterface>
inline result_t MakeInterface(OMPtr<TInterface>* ptr, const guid_t& clsid = GetNullGuid());

}
