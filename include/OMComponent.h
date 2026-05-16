#pragma once

#include "Local.h"

struct OMFactoryDef
{
        guid_t guid;
        fxIBase* (*factory)();
        OMFactoryDef* next = nullptr;
        OMFactoryDef(const guid_t& g, fxIBase* (*f)()) : guid(g), factory(f)
        {
                next = s_factories;
                s_factories = this;
        }
        static OMFactoryDef* s_factories;
};

struct OMImplementsDef
{
        guid_t iid;
        guid_t clsid;
        OMImplementsDef* next = nullptr;
        OMImplementsDef(const guid_t& c, const guid_t& i) : iid(i), clsid(c)
        {
                next = s_impls;
                s_impls = this;
        }
        static OMImplementsDef* s_impls;
};

#define FX_NEW_FACTORY(RuntimeClass)                                     \
        static OMFactoryDef _g_##RuntimeClass##_factory                  \
        {                                                                \
                CLSID_##RuntimeClass,                                    \
                []() -> fxIBase* {                                       \
                        return fx::MakeNewBase<fx::cpp::RuntimeClass>(); \
                }                                                        \
        }

#define FX_IMPLEMENTS(clsid, iface)            \
        static OMImplementsDef _g_impl_##iface \
        {                                      \
                clsid, iface::GetIID()         \
        }
