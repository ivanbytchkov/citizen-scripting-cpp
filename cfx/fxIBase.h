#pragma once

#include "fxBase.h"

class fxIBase
{
public:
    NS_IMETHOD_(result_t) QueryInterface(const guid_t& riid, void** outObject) = 0;
    NS_IMETHOD_(uint32_t) AddRef() = 0;
    NS_IMETHOD_(uint32_t) Release() = 0;
};
