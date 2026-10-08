#include <LemonadeNexusSDK/RoutingConnectionPool.hpp>

namespace lnsdk {

std::size_t RoutingConnectionPool::in_flight_locked() const {
    std::size_t n = 0;
    for (const auto& [id, c] : conns_)
        if (c.state == ConnState::Requested || c.state == ConnState::Connecting) ++n;
    return n;
}

bool RoutingConnectionPool::try_begin(const std::string& identifier, uint64_t now,
                                      std::string& evicted) {
    evicted.clear();
    std::lock_guard lock(mtx_);

    if (auto it = conns_.find(identifier); it != conns_.end()) {
        it->second.last_used = now;             // idempotent reuse
        return true;
    }

    if (in_flight_locked() >= cfg_.max_in_flight) return false;

    if (conns_.size() >= cfg_.max_endpoints) {
        // Evict the least-recently-used Established connection.
        auto lru = conns_.end();
        for (auto it = conns_.begin(); it != conns_.end(); ++it) {
            if (it->second.state != ConnState::Established) continue;
            if (lru == conns_.end() || it->second.last_used < lru->second.last_used) lru = it;
        }
        if (lru == conns_.end()) return false;  // all in-flight — cannot admit
        evicted = lru->first;
        conns_.erase(lru);
    }

    conns_.emplace(identifier, PooledConnection{identifier, ConnState::Requested, now});
    return true;
}

void RoutingConnectionPool::mark_connecting(const std::string& id) {
    std::lock_guard lock(mtx_);
    if (auto it = conns_.find(id); it != conns_.end()) it->second.state = ConnState::Connecting;
}

void RoutingConnectionPool::mark_established(const std::string& id, uint64_t now) {
    std::lock_guard lock(mtx_);
    if (auto it = conns_.find(id); it != conns_.end()) {
        it->second.state = ConnState::Established;
        it->second.last_used = now;
    }
}

void RoutingConnectionPool::mark_failed(const std::string& id) {
    std::lock_guard lock(mtx_);
    if (auto it = conns_.find(id); it != conns_.end()) it->second.state = ConnState::Failed;
}

void RoutingConnectionPool::touch(const std::string& id, uint64_t now) {
    std::lock_guard lock(mtx_);
    if (auto it = conns_.find(id); it != conns_.end()) it->second.last_used = now;
}

void RoutingConnectionPool::remove(const std::string& id) {
    std::lock_guard lock(mtx_);
    conns_.erase(id);
}

std::size_t RoutingConnectionPool::size() const {
    std::lock_guard lock(mtx_);
    return conns_.size();
}

std::size_t RoutingConnectionPool::in_flight() const {
    std::lock_guard lock(mtx_);
    return in_flight_locked();
}

std::optional<ConnState> RoutingConnectionPool::state_of(const std::string& id) const {
    std::lock_guard lock(mtx_);
    if (auto it = conns_.find(id); it != conns_.end()) return it->second.state;
    return std::nullopt;
}

} // namespace lnsdk
