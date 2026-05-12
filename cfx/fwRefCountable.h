#pragma once

#include <atomic>

class fwRefCountable
{
    std::atomic<uint32_t> m_count { 0 };

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
    fwRefContainer(T* ref) : m_ref(ref) { if (m_ref) m_ref->AddRef(); }
    fwRefContainer(const fwRefContainer& o) : m_ref(o.m_ref) { if (m_ref) m_ref->AddRef(); }
    ~fwRefContainer() { if (m_ref) m_ref->Release(); }
    fwRefContainer& operator=(const fwRefContainer& o)
    {
        if (o.m_ref) o.m_ref->AddRef();
        if (m_ref) m_ref->Release();
        m_ref = o.m_ref;
        return *this;
    }
    T*  GetRef() const { return m_ref; }
    T*  operator->() const { return m_ref; }
    explicit operator bool() const { return m_ref != nullptr; }
};
