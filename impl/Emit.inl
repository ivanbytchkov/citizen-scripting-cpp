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
    PushEnvironment env(m_handler.GetRef(), m_runtime);
    fxNativeContext ctx{};
    ctx.nativeIdentifier = HashString("TRIGGER_EVENT_INTERNAL");
    ctx.arguments[0] = reinterpret_cast<uintptr_t>(event.c_str());
    ctx.arguments[1] = reinterpret_cast<uintptr_t>(payload.data());
    ctx.arguments[2] = static_cast<uintptr_t>(payload.size());
    ctx.numArguments = 3;
    m_host->InvokeNative(ctx);
}

inline void ResourceContext::emitNet(const std::string& event, int target, const std::vector<std::string>& rawArgs)
{
    auto payload = fx::msgpack::encodeArgs(rawArgs);
    PushEnvironment env(m_handler.GetRef(), m_runtime);
    fxNativeContext ctx{};
    ctx.nativeIdentifier = HashString("TRIGGER_CLIENT_EVENT_INTERNAL");
    ctx.arguments[0] = reinterpret_cast<uintptr_t>(event.c_str());
    ctx.arguments[1] = static_cast<uintptr_t>(target);
    ctx.arguments[2] = reinterpret_cast<uintptr_t>(payload.data());
    ctx.arguments[3] = static_cast<uintptr_t>(payload.size());
    ctx.numArguments = 4;
    m_host->InvokeNative(ctx);
}

}
