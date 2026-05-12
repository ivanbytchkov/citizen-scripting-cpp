#pragma once

#include "core.h"
#include "fxNativeContext.h"

FX_DEFINE_GUID(IID_IScriptHost, 0x8FFDC384, 0x4767, 0x4EA2, 0xA9, 0x35, 0x3B, 0xFC, 0xAD, 0x1D, 0xB7, 0xBF);

class fxIStream;
class IScriptBuffer;

class IScriptHost : public fxIBase
{
public:
    NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptHost)
    NS_IMETHOD InvokeNative(fxNativeContext& context) = 0;
    NS_IMETHOD OpenSystemFile(char* fileName, fxIStream** stream) = 0;
    NS_IMETHOD OpenHostFile(char* fileName, fxIStream** stream) = 0;
    NS_IMETHOD CanonicalizeRef(int32_t localRef, int32_t instanceId, char** refString) = 0;
    NS_IMETHOD ScriptTrace(char* message) = 0;
    NS_IMETHOD SubmitBoundaryStart(char* boundaryData, int32_t boundarySize) = 0;
    NS_IMETHOD SubmitBoundaryEnd(char* boundaryData, int32_t boundarySize) = 0;
    NS_IMETHOD GetLastErrorText(char** errorString) = 0;
    NS_IMETHOD InvokeFunctionReference(char* refId, char* argsSerialized, uint32_t argsSize, IScriptBuffer** ret) = 0;
};
