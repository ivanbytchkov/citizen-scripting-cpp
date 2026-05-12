#pragma once

#include "core.h"

#include <vector>

FX_DEFINE_GUID(IID_IScriptBuffer, 0xAD1B9D69, 0xB984, 0x4D30, 0x8D, 0x33, 0xBB, 0x1E, 0x6C, 0xF9, 0xE1, 0xBA);

class IScriptBuffer : public fxIBase
{
public:
    NS_DECLARE_STATIC_IID_ACCESSOR(IID_IScriptBuffer)
    NS_IMETHOD_(char*) GetBytes() = 0;
    NS_IMETHOD_(uint32_t) GetLength() = 0;
};

class ScriptBuffer final : public fx::OMClass<ScriptBuffer, IScriptBuffer>
{
    std::vector<char> m_data;
public:
    ScriptBuffer() = default;
    explicit ScriptBuffer(std::vector<char> data) : m_data(std::move(data)) {}
    char* OM_DECL GetBytes() override { return m_data.data(); }
    uint32_t OM_DECL GetLength() override { return static_cast<uint32_t>(m_data.size()); }
};
