#pragma once

#include "core.h"

FX_DEFINE_GUID(IID_fxIStream, 0x82EC2441, 0xDBB4, 0x4512, 0x81, 0xE9, 0x3A, 0x98, 0xCE, 0x9F, 0xFC, 0xAB);

class fxIStream : public fxIBase
{
public:
    NS_DECLARE_STATIC_IID_ACCESSOR(IID_fxIStream)
};
