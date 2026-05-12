namespace fx
{

inline void ResourceContext::onStop(StopHandler h)
{
    m_stopHandlers.push_back(std::move(h));
}

inline void ResourceContext::dispatchStop()
{
    for (auto& h : m_stopHandlers)
    {
        try { h(); }
        catch (...) {}
    }
    m_stopHandlers.clear();
}

}
