namespace fx
{

inline void ResourceContext::trace(const char* fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m_host->ScriptTrace(buf);
    fprintf(stderr, "[script:%s] %s", m_name.c_str(), buf);
}

inline void ResourceContext::emit(const std::string& event, const std::vector<std::string>& rawArgs)
{
    auto payload = fx::msgpack::encodeArgs(rawArgs);
    invokeNative(HashString("TRIGGER_EVENT_INTERNAL"), reinterpret_cast<uintptr_t>(event.c_str()), reinterpret_cast<uintptr_t>(payload.data()), payload.size());
}

inline void ResourceContext::emitNet(const std::string& event, int target, const std::vector<std::string>& rawArgs)
{
    auto payload = fx::msgpack::encodeArgs(rawArgs);
    invokeNative(HashString("TRIGGER_CLIENT_EVENT_INTERNAL"), reinterpret_cast<uintptr_t>(event.c_str()), static_cast<uintptr_t>(target), reinterpret_cast<uintptr_t>(payload.data()), payload.size());
}

}
