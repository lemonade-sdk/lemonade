#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace lnsdk {

/// Lifecycle of one pooled endpoint connection, keyed by endpoint identifier.
enum class ConnState { Requested, Connecting, Established, Failed };

struct PooledConnection {
    std::string identifier;
    ConnState   state{ConnState::Requested};
    uint64_t    last_used{0};   ///< logical timestamp for LRU
};

/// Bounds the client's auto-connected endpoint set: at most `max_endpoints`
/// total and `max_in_flight` concurrent setups. When full, the least-recently-
/// used Established connection is evicted to admit a new request. Pure logic
/// (caller supplies a monotonic `now`), so it is trivially unit-testable and
/// independent of the network/dataplane.
class RoutingConnectionPool {
public:
    struct Config {
        std::size_t max_endpoints{200};
        std::size_t max_in_flight{16};
    };

    RoutingConnectionPool() = default;
    explicit RoutingConnectionPool(Config cfg) : cfg_(cfg) {}

    /// Admit a new request for `identifier`. Returns true if it may proceed.
    /// Idempotent: an already-pooled identifier is touched and accepted.
    /// Fails (false) when the in-flight cap is reached, or when at the total cap
    /// with no Established connection available to evict. On eviction, the
    /// removed identifier is returned via `evicted`.
    [[nodiscard]] bool try_begin(const std::string& identifier, uint64_t now,
                                 std::string& evicted);

    void mark_connecting(const std::string& id);
    void mark_established(const std::string& id, uint64_t now);
    void mark_failed(const std::string& id);
    void touch(const std::string& id, uint64_t now);
    void remove(const std::string& id);

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t in_flight() const;   ///< Requested + Connecting
    [[nodiscard]] std::optional<ConnState> state_of(const std::string& id) const;

private:
    [[nodiscard]] std::size_t in_flight_locked() const;

    Config cfg_;
    mutable std::mutex mtx_;
    std::unordered_map<std::string, PooledConnection> conns_;
};

} // namespace lnsdk
