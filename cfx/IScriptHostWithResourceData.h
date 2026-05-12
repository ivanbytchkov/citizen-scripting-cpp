#pragma once

#include "core.h"

FX_DEFINE_GUID(IID_IScriptHostWithResourceData, 0x9568DF2D, 0x27C8, 0x4B9E, 0xB2, 0x9D, 0x48, 0x27, 0x2C, 0x31, 0x70, 0x84);

class IScriptHostWithResourceData : public fxIBase
{
public:
    NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptHostWithResourceData)
    NS_IMETHOD GetResourceName(char** resourceName) = 0;
    NS_IMETHOD GetNumResourceMetaData(char* fieldName, int32_t* numFields) = 0;
    NS_IMETHOD GetResourceMetaData(char* fieldName, int32_t fieldIndex, char** fieldValue) = 0;
};
