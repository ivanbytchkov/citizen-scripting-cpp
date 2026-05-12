namespace fx
{

inline int32_t ResourceContext::setTimeout(uint32_t ms, std::function<void()> cb)
{
    int32_t id = m_nextTimerId++;
    auto fire = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    m_timers[id] = { id, fire, 0, std::move(cb) };
    return id;
}

inline int32_t ResourceContext::setInterval(uint32_t ms, std::function<void()> cb)
{
    int32_t id = m_nextTimerId++;
    auto fire = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    m_timers[id] = { id, fire, ms, std::move(cb) };
    return id;
}

inline void ResourceContext::clearTimer(int32_t id)
{
    m_timers.erase(id);
}

inline void ResourceContext::dispatchTick()
{
    auto now = std::chrono::steady_clock::now();
    std::vector<int32_t> expired;
    for (auto& [id, t] : m_timers)
    {
        if (now >= t.nextFire)
            expired.push_back(id);
    }
    for (auto id : expired)
    {
        auto it = m_timers.find(id);
        if (it == m_timers.end()) continue;
        auto cb = it->second.callback;
        if (it->second.intervalMs > 0)
            it->second.nextFire = now + std::chrono::milliseconds(it->second.intervalMs);
        else
            m_timers.erase(it);
        cb();
    }

    for (auto& h : m_tickHandlers) h();
}

}
