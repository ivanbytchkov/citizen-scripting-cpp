namespace fx
{

inline void ResourceContext::on(const std::string& event, EventHandler h)
{
    bool first = m_eventHandlers.find(event) == m_eventHandlers.end();
    m_eventHandlers[event].push_back(std::move(h));
    if (first)
        invokeNative(HashString("REGISTER_RESOURCE_AS_EVENT_HANDLER"), reinterpret_cast<uintptr_t>(event.c_str()));
}

inline void ResourceContext::registerNetEvent(const std::string& event)
{
    m_netSafeEvents.insert(event);
}

inline void ResourceContext::onNet(const std::string& event, EventHandler h)
{
    registerNetEvent(event);
    on(event, std::move(h));
}

inline void ResourceContext::onTick(TickHandler h)
{
    m_tickHandlers.push_back(std::move(h));
}

inline void ResourceContext::onCommand(const std::string& command, CommandHandler h)
{
    m_commandHandlers[command].push_back(std::move(h));

    if (!m_addRef)
    {
        fprintf(stderr, "[fx-cpp-sdk] onCommand('%s'): no ref support available\n", command.c_str());
        return;
    }

    int32_t refIdx = m_addRef([this, command](const char* argsSerialized, uint32_t argsSize) -> std::vector<char> {
        fx::json::Value args = fx::msgpack::decode(argsSerialized, argsSize);
        if (args.kind != fx::json::Value::Kind::Array) return {};
        std::string source = args.size() > 0 ? std::to_string(args.at(0).asInt()) : "0";
        std::vector<std::string> cmdArgs;
        if (args.size() > 1 && args.at(1).kind == fx::json::Value::Kind::Array)
            for (size_t i = 0; i < args.at(1).size(); ++i)
                cmdArgs.push_back(args.at(1).at(i).asStr());
        dispatchCommand(command, source, cmdArgs);
        return { static_cast<char>(0xC0) };
    });

    char* refString = nullptr;
    m_host->CanonicalizeRef(refIdx, m_runtime->GetInstanceId(), &refString);
    if (!refString) return;
    invokeNative(HashString("REGISTER_COMMAND"), reinterpret_cast<uintptr_t>(command.c_str()), reinterpret_cast<uintptr_t>(refString), uintptr_t(0));
    fwFree(refString);
}

inline void ResourceContext::dispatchEvent(const std::string& name, const json::Value& args, const std::string& source)
{
    if (source.size() > 4 && source.compare(0, 4, "net:") == 0)
    {
        if (m_netSafeEvents.find(name) == m_netSafeEvents.end())
            return;
    }
    auto it = m_eventHandlers.find(name);
    if (it == m_eventHandlers.end()) return;
    EventArgs ea(args);
    for (auto& h : it->second) h(source, ea);
}

inline void ResourceContext::dispatchCommand(const std::string& command, const std::string& source, const std::vector<std::string>& args)
{
    auto it = m_commandHandlers.find(command);
    if (it == m_commandHandlers.end()) return;
    for (auto& h : it->second) h(source, args);
}

}
