#pragma once

#include "core.h"

FX_DEFINE_GUID(IID_IScriptRuntime, 0x67B28AF1, 0xAAF9, 0x4368, 0x82, 0x96, 0xF9, 0x3A, 0xFC, 0x7B, 0xDE, 0x96);

class IScriptHost;

class IScriptRuntime : public fxIBase
{
public:
    NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptRuntime)
    NS_IMETHOD Create(IScriptHost* scriptHost) = 0;
    NS_IMETHOD Destroy() = 0;
    NS_IMETHOD_(void*) GetParentObject() = 0;
    NS_IMETHOD_(void) SetParentObject(void*) = 0;
    NS_IMETHOD_(int32_t) GetInstanceId() = 0;
};
