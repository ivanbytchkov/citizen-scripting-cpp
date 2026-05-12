#pragma once

#include "core.h"

class IScriptHostWithResourceData;

FX_DEFINE_GUID(IID_IScriptFileHandlingRuntime, 0x567634C6, 0x3BDD, 0x4D0E, 0xAF, 0x39, 0x74, 0x72, 0xAE, 0xD4, 0x79, 0xB7);

class IScriptFileHandlingRuntime : public fxIBase
{
public:
    NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptFileHandlingRuntime)
    NS_IMETHOD_(int32_t) HandlesFile(char* scriptFile, IScriptHostWithResourceData* metadata) = 0;
    NS_IMETHOD LoadFile(char* scriptFile) = 0;
};
