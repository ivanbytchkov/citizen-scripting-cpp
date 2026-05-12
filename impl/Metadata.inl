namespace fx
{

inline std::string ResourceContext::getResourceMetadata(const std::string& key, int index)
{
    if (!m_metadataHost.GetRef()) return {};
    char* value = nullptr;
    if (FX_FAILED(m_metadataHost->GetResourceMetaData(const_cast<char*>(key.c_str()), index, &value)) || !value)
        return {};
    return std::string(value);
}

inline int ResourceContext::getNumResourceMetadata(const std::string& key)
{
    if (!m_metadataHost.GetRef()) return 0;
    int32_t count = 0;
    m_metadataHost->GetNumResourceMetaData(const_cast<char*>(key.c_str()), &count);
    return count;
}

}
