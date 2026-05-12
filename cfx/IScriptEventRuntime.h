#pragma once

#include "core.h"

FX_DEFINE_GUID(IID_IScriptEventRuntime, 0x637140DB, 0x24E5, 0x46BF, 0xA8, 0xBD, 0x08, 0xF2, 0xDB, 0xAC, 0x51, 0x9A);

class IScriptEventRuntime : public fxIBase
{
public:
    NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptEventRuntime)
    NS_IMETHOD TriggerEvent(char* eventName, char* argsSerialized, uint32_t serializedSize, char* sourceId) = 0;
};
