namespace fx
{

inline void ResourceContext::setStateBagValue(const std::string& bagName, const std::string& key, const json::Value& value, bool replicated)
{
    auto encoded = msgpack::encode(value);
    invokeNative(HashString("SET_STATE_BAG_VALUE"), reinterpret_cast<uintptr_t>(bagName.c_str()), reinterpret_cast<uintptr_t>(key.c_str()), reinterpret_cast<uintptr_t>(encoded.data()), encoded.size(), uintptr_t(replicated ? 1u : 0u));
}

inline void ResourceContext::setPlayerState(int serverId, const std::string& key, const json::Value& value, bool replicated)
{
    setStateBagValue("player:" + std::to_string(serverId), key, value, replicated);
}

inline void ResourceContext::setEntityState(int netId, const std::string& key, const json::Value& value, bool replicated)
{
    setStateBagValue("entity:" + std::to_string(netId), key, value, replicated);
}

inline void ResourceContext::setGlobalState(const std::string& key, const json::Value& value, bool replicated)
{
    setStateBagValue("global", key, value, replicated);
}

}
