namespace fx
{

inline void ResourceContext::setStateBagValue(const std::string& bagName, const std::string& key, const json::Value& value, bool replicated)
{
    auto encoded = msgpack::encode(value);
    PushEnvironment env(m_handler.GetRef(), m_runtime);
    fxNativeContext ctx{};
    ctx.nativeIdentifier = HashString("SET_STATE_BAG_VALUE");
    ctx.arguments[0] = reinterpret_cast<uintptr_t>(bagName.c_str());
    ctx.arguments[1] = reinterpret_cast<uintptr_t>(key.c_str());
    ctx.arguments[2] = reinterpret_cast<uintptr_t>(encoded.data());
    ctx.arguments[3] = static_cast<uintptr_t>(encoded.size());
    ctx.arguments[4] = replicated ? 1u : 0u;
    ctx.numArguments = 5;
    m_host->InvokeNative(ctx);
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
