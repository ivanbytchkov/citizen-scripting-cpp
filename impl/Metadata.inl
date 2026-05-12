namespace fx
{

inline std::string ResourceContext::getResourceMetadata(const std::string& key, int index)
{
    fx::OMPtr<IScriptHostWithResourceData> md;
    fx::OMPtr<IScriptHost> h(m_host);
    if (FX_FAILED(h.As(&md)) || !md.GetRef()) return {};
    char* value = nullptr;
    if (FX_FAILED(md->GetResourceMetaData(const_cast<char*>(key.c_str()), index, &value)) || !value)
        return {};
    return std::string(value);
}

inline int ResourceContext::getNumResourceMetadata(const std::string& key)
{
    fx::OMPtr<IScriptHostWithResourceData> md;
    fx::OMPtr<IScriptHost> h(m_host);
    if (FX_FAILED(h.As(&md)) || !md.GetRef()) return 0;
    int32_t count = 0;
    md->GetNumResourceMetaData(const_cast<char*>(key.c_str()), &count);
    return count;
}

}
