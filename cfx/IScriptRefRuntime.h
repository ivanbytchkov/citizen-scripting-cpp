#pragma once

#include "core.h"

class IScriptBuffer;

FX_DEFINE_GUID(IID_IScriptRefRuntime, 0xA2F1B24B, 0xA29F, 0x4121, 0x81, 0x62, 0x86, 0x90, 0x1E, 0xCA, 0x80, 0x97);

class IScriptRefRuntime : public fxIBase
{
public:
    NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptRefRuntime)
    NS_IMETHOD CallRef(int32_t refIdx, char* argsSerialized, uint32_t argsSize, IScriptBuffer** retval) = 0;
    NS_IMETHOD DuplicateRef(int32_t refIdx, int32_t* newRefIdx) = 0;
    NS_IMETHOD RemoveRef(int32_t refIdx) = 0;
};
