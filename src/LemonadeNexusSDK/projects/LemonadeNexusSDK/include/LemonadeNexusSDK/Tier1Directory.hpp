#pragma once

#include <LemonadeNexusSDK/Error.hpp>
#include <LemonadeNexusSDK/Types.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace lnsdk {

/// One HTTP response, as a transport hands it back.
struct HttpReply {
    int         status{0};
    std::string body;
};

/// A GET through the caller's verified-TLS transport, with no session token.
/// nullopt when no response arrived (connect, TLS, or read failure).
using Tier1DiscoveryGet =
    std::function<std::optional<HttpReply>(const ServerEndpoint& server, const std::string& path)>;

/// One attempt of a tree request against `server`. nullopt when no response
/// arrived.
using Tier1Attempt = std::function<std::optional<HttpReply>(const ServerEndpoint& server)>;

/// A Tier 1 server the client verified: a member of the verified epoch that
/// signed an endpoint proof for exactly this host and port.
struct Tier1Server {
    std::string node_id;  ///< base64 Ed25519 node identity
    std::string host;     ///< certificate FQDN the proof names
    uint16_t    port{0};
};

/// The verified Tier 1 membership and the servers proven reachable.
struct Tier1Membership {
    uint64_t                 epoch{0};
    std::vector<std::string> members;  ///< base64 node ids, in Tier 1 set order
    std::vector<Tier1Server> servers;
};

/// A Genesis public key that first-connect discovery found and pinned.
struct GenesisKeyDiscovery {
    std::string              genesis_pubkey;  ///< canonical standard base64 (32 bytes)
    std::string              network_id;      ///< lowercase hex of the derived network id
    std::string              dns_base_domain; ///< the network DNS domain the hosts reported
    std::vector<std::string> hosts;           ///< the agreeing hosts inside that domain
};

/// Distinct hosts inside the network DNS domain that must report the same
/// Genesis key before first-connect discovery pins it. A network with fewer
/// reachable servers needs an explicit set_genesis_key().
inline constexpr std::size_t kGenesisDiscoveryMinHosts = 3;

/// Finds and verifies the Tier 1 servers that every tree read and write goes
/// to. Only Tier 1 servers hold the tree; a Tier 2 server refuses every tree
/// route.
///
/// Trust comes from one pinned value, the network's Genesis public key. The
/// directory walks the authority chain from it (GET /api/tier1/authority on
/// any server), verifying the Genesis certificate and every finalized handoff
/// with its commit proof. It then asks each candidate server for a signed
/// endpoint proof over a fresh nonce (GET /api/tier1/proof) and keeps only a
/// member of the verified epoch whose proof names the host and port it
/// reached, in that same epoch. A forged chain, a chain for another network,
/// a proof for an older or newer epoch, and a proof relayed from another host
/// are all refused. Without a pinned key there is no Tier 1 server and no
/// tree request leaves the client.
///
/// Staleness: the verified epoch never decreases, and a refresh that cannot
/// reach `min_epoch` fails. A proof that names a later epoch than the
/// verified one makes the directory walk the chain again before it trusts
/// that server. A set of servers that all withhold newer handoffs can hold a
/// client with no floor at an old epoch; the floor (`min_epoch`, for example
/// the last epoch the application saw) closes that across restarts.
///
/// Thread-safe. refresh() runs one at a time; reads never wait for network
/// I/O.
class Tier1Directory {
public:
    explicit Tier1Directory(Tier1DiscoveryGet get);
    ~Tier1Directory();

    Tier1Directory(const Tier1Directory&) = delete;
    Tier1Directory& operator=(const Tier1Directory&) = delete;

    /// Pin the Genesis public key (base64, 32 bytes) and the lowest epoch to
    /// accept. Clears any verified state when the key changes. Returns false
    /// and pins nothing when the key is malformed.
    bool set_genesis_key(const std::string& genesis_pubkey_b64, uint64_t min_epoch = 0);

    [[nodiscard]] bool has_genesis_key() const;

    /// First-connect discovery: find the Genesis key from the servers
    /// themselves and pin it. Runs only when no key is pinned; it never
    /// replaces a pinned key, even one pinned while it runs.
    ///
    /// Asks each host in `hosts` (at most 16, in order, duplicates removed)
    /// for GET /api/onboard/info through the verified-TLS transport. A host
    /// that does not answer 200, or answers without a key, is not counted.
    /// Every host that reports a key must report the same key, network id,
    /// and DNS domain, and the network id must derive from the key under the
    /// compiled ruleset versions. At least kGenesisDiscoveryMinHosts distinct
    /// agreeing hosts must sit inside the reported DNS domain. A disagreement,
    /// a malformed key or network id, a network id that does not derive from
    /// the key, or no DNS domain fails closed and pins nothing.
    ///
    /// Trust limit: this is trust on first use. It rests on verified-HTTPS
    /// ownership of the network's DNS domain and on agreement of several
    /// hosts, not on a signature. After it succeeds the key is pinned like
    /// one set with set_genesis_key().
    ///
    /// Errors: http_status 409 when a key is already pinned, 403 when the
    /// answers are refused, 0 when too few hosts agreed.
    Result<GenesisKeyDiscovery> discover_genesis_key(const std::vector<ServerEndpoint>& hosts);

    /// Walk the chain from `sources`, then probe candidate servers: each
    /// source, and each server those sources list in GET /api/servers whose
    /// node id is a member. On success the verified membership replaces the
    /// previous one. On failure the previous servers are dropped (fail
    /// closed), the error says why, and http_status is 0 when nothing was
    /// reachable or 403 when the key is missing or what the servers returned
    /// is refused (a forged chain, a stale epoch).
    Result<Tier1Membership> refresh(const std::vector<ServerEndpoint>& sources);

    /// The last verified membership; empty servers when none is verified.
    [[nodiscard]] Tier1Membership membership() const;

    /// True when a refresh is due: nothing verified, marked stale, or older
    /// than `max_age_ms`.
    [[nodiscard]] bool needs_refresh(int64_t max_age_ms) const;

    /// True when `node_id` (base64) is a member of the verified epoch and has
    /// a verified server.
    [[nodiscard]] bool is_verified_member(const std::string& node_id) const;

    /// Force the next needs_refresh() to report true (a verified server
    /// refused a tree route, so the membership may have changed).
    void mark_stale();

    /// Send one tree request to verified Tier 1 servers only. Starts with the
    /// server that answered last, and fails over to the next verified server
    /// when no response arrives or a server answers 421 (it holds no tree).
    /// With `pin_node` set, only that member's server is tried: a session is
    /// bound to the server that issued it. Without a pin, `avoid_nodes`
    /// names members (base64 node ids) whose servers are not tried: the
    /// server a session moves away from while it is still verified, and the
    /// members that refused a join's invite as unknown. Returns the first
    /// other response, and sets `answered` to the server that sent it.
    /// nullopt with `error` set when no verified server answered, also when
    /// every verified server is avoided.
    std::optional<HttpReply> send(const Tier1Attempt& attempt, const std::string& pin_node,
                                  Tier1Server* answered, std::string& error,
                                  const std::set<std::string>& avoid_nodes = {});

private:
    Result<Tier1Membership> refresh_verified(const std::vector<ServerEndpoint>& sources);

    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace lnsdk
