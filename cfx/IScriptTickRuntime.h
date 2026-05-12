#pragma once

#include "core.h"

FX_DEFINE_GUID(IID_IScriptTickRuntime, 0x91B203C7, 0xF95A, 0x4902, 0xB4, 0x63, 0x72, 0x2D, 0x55, 0x09, 0x83, 0x66);

class IScriptTickRuntime : public fxIBase
{
public:
    NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptTickRuntime)
    NS_IMETHOD Tick() = 0;
};
