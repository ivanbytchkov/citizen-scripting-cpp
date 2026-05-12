#pragma once

#include "core.h"
#include "IScriptRuntime.h"

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

class PushEnvironment
{
    fx::OMPtr<IScriptRuntimeHandler> m_handler;
    fx::OMPtr<IScriptRuntime> m_runtime;
    static fx::OMPtr<IScriptRuntimeHandler> GetHandler()
    {
        static fx::OMPtr<IScriptRuntimeHandler> h;
        if (!h.GetRef())
            fx::MakeInterface(&h, CLSID_ScriptRuntimeHandler);
        return h;
    }

public:
    PushEnvironment() = default;
    explicit PushEnvironment(IScriptRuntime* rt)
    {
        m_handler = GetHandler();
        if (!m_handler.GetRef()) return;
        m_runtime = fx::OMPtr<IScriptRuntime>(rt);
        m_handler->PushRuntime(rt);
    }
    PushEnvironment(IScriptRuntimeHandler* handler, IScriptRuntime* rt)
    {
        if (!handler || !rt) return;
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
        o.m_handler = {};
        o.m_runtime = {};
    }
};

inline result_t GetCurrentScriptRuntime(fx::OMPtr<IScriptRuntime>* out)
{
    static fx::OMPtr<IScriptRuntimeHandler> h;
    if (!h.GetRef())
        fx::MakeInterface(&h, CLSID_ScriptRuntimeHandler);
    if (!h.GetRef()) return FX_E_NOTIMPL;
    return h->GetCurrentRuntime(out->ReleaseAndGetAddressOf());
}

}
