namespace fx
{

inline void ResourceContext::addExport(const std::string& name, ExportHandler handler)
{
    if (!m_addRef) return;

    int32_t refIdx = m_addRef([handler](const char* argsSerialized, uint32_t argsSize) -> std::vector<char> {
        json::Value args = msgpack::decode(argsSerialized, argsSize);
        if (args.kind != json::Value::Kind::Array)
        {
            json::Value wrapper;
            wrapper.kind = json::Value::Kind::Array;
            wrapper.children.push_back(std::move(args));
            args = std::move(wrapper);
        }
        EventArgs ea(args);
        json::Value result = handler(ea);
        json::Value arr;
        arr.kind = json::Value::Kind::Array;
        arr.children.push_back(std::move(result));
        auto encoded = msgpack::encode(arr);
        return std::vector<char>(encoded.begin(), encoded.end());
    });

    char* refString = nullptr;
    m_host->CanonicalizeRef(refIdx, m_runtime->GetInstanceId(), &refString);
    if (!refString) return;
    std::string exportRef = refString;
    fwFree(refString);

    std::string eventName = "__cfx_export_" + m_name + "_" + name;
    on(eventName, [this, exportRef](const std::string& /*source*/, EventArgs args) {
        if (args.size() == 0) return;
        std::string setterRef = args.funcRef(0);
        if (setterRef.empty()) return;

        json::Value refVal;
        refVal.kind = json::Value::Kind::FuncRef;
        refVal.scalar = exportRef;
        json::Value arr;
        arr.kind = json::Value::Kind::Array;
        arr.children.push_back(std::move(refVal));
        auto payload = msgpack::encode(arr);

        PushEnvironment env(m_handler.GetRef(), m_runtime);
        fx::OMPtr<IScriptBuffer> retBuf;
        m_host->InvokeFunctionReference(
            const_cast<char*>(setterRef.c_str()),
            reinterpret_cast<char*>(payload.data()),
            static_cast<uint32_t>(payload.size()),
            retBuf.ReleaseAndGetAddressOf()
        );
    });
}

inline json::Value ResourceContext::callExport(const std::string& resource, const std::string& name, const std::vector<std::string>& args)
{
    if (!m_addRef) return {};

    auto capturedRef = std::make_shared<std::string>();
    int32_t setterIdx = m_addRef([capturedRef](const char* argsSerialized, uint32_t argsSize) -> std::vector<char> {
        json::Value decoded = msgpack::decode(argsSerialized, argsSize);
        if (decoded.kind == json::Value::Kind::FuncRef)
            *capturedRef = decoded.scalar;
        else if (decoded.kind == json::Value::Kind::Array && decoded.size() > 0 && decoded.at(0).kind == json::Value::Kind::FuncRef)
            *capturedRef = decoded.at(0).scalar;
        return { static_cast<char>(0xC0) };
    });

    char* setterRefStr = nullptr;
    m_host->CanonicalizeRef(setterIdx, m_runtime->GetInstanceId(), &setterRefStr);
    if (!setterRefStr) return {};

    json::Value setterVal;
    setterVal.kind = json::Value::Kind::FuncRef;
    setterVal.scalar = setterRefStr;
    fwFree(setterRefStr);

    json::Value setterArr;
    setterArr.kind = json::Value::Kind::Array;
    setterArr.children.push_back(std::move(setterVal));
    auto setterPayload = msgpack::encode(setterArr);

    std::string eventName = "__cfx_export_" + resource + "_" + name;
    {
        PushEnvironment env(m_handler.GetRef(), m_runtime);
        fxNativeContext nctx{};
        nctx.nativeIdentifier = HashString("TRIGGER_EVENT_INTERNAL");
        nctx.arguments[0] = reinterpret_cast<uintptr_t>(eventName.c_str());
        nctx.arguments[1] = reinterpret_cast<uintptr_t>(setterPayload.data());
        nctx.arguments[2] = static_cast<uintptr_t>(setterPayload.size());
        nctx.numArguments = 3;
        m_host->InvokeNative(nctx);
    }

    if (capturedRef->empty()) return {};
    auto userPayload = msgpack::encodeArgs(args);
    fx::OMPtr<IScriptBuffer> retBuf;
    {
        PushEnvironment env(m_handler.GetRef(), m_runtime);
        m_host->InvokeFunctionReference(
            const_cast<char*>(capturedRef->c_str()),
            reinterpret_cast<char*>(userPayload.data()),
            static_cast<uint32_t>(userPayload.size()),
            retBuf.ReleaseAndGetAddressOf()
        );
    }
    if (!retBuf.GetRef()) return {};
    json::Value result = msgpack::decode(retBuf->GetBytes(), retBuf->GetLength());
    if (result.kind == json::Value::Kind::Array && result.size() > 0)
        return result.at(0);
    return result;
}

}
