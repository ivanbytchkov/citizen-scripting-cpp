namespace fx
{

inline void ResourceContext::onStop(StopHandler h)
{
    m_stopHandlers.push_back(std::move(h));
}

inline void ResourceContext::dispatchStop()
{
    auto handlers = std::move(m_stopHandlers);
    m_stopHandlers.clear();
    for (auto& h : handlers)
    {
        try { h(); }
        catch (const std::exception& e) { trace("Unhandled exception in stop handler: %s\n", e.what()); }
        catch (...) { trace("Unhandled non-standard exception in stop handler\n"); }
    }
}

}
