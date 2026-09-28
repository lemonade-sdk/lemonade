#pragma once

/// Fully in-process boringtun dataplane.
///
/// One UDP socket + one boringtun Noise session per peer + a userspace
/// cryptokey router. No TUN device, no kernel interface, no elevated
/// privileges: tunnel keys and plaintext never leave this process.
///
/// Decrypted packets are routed in userspace:
///   - dst == one of our virtual IPs  -> inbound handler (in-process netstack)
///   - dst in another peer's allowed IPs -> re-encrypted and sent (hairpin)
///   - anything else -> dropped by cryptokey routing
///
/// Incoming UDP datagrams are demultiplexed O(1): boringtun tags each local
/// session receiver index with the tunnel index we assign per peer
/// (receiver_idx >> 8), so transport packets never trial-decrypt. Handshake
/// initiations (rare) are identified with a single anonymous parse.
///
/// Plain class (not a CRTP service) so tests can run several instances in
/// one process. BoringtunService owns one and delegates to it.

#include <LemonadeNexusSDK/Transport/IBoringtunProvider.hpp>
#include <LemonadeNexusSDK/Transport/IpRouter.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct Tunn;  // boringtun_ffi.h

namespace nexus::boringtun {

class UserspaceDataplane {
public:
    struct Config {
        std::string private_key_b64;   ///< Our Curve25519 private key
        std::string public_key_b64;    ///< Our Curve25519 public key
        uint16_t    listen_port{51940};
        int         rx_threads{2};
        /// Maximum plaintext IP packet we will emit/accept on the local seam.
        uint32_t    mtu{1420};
    };

    /// Receives decrypted IP packets addressed to one of our virtual IPs.
    /// Called from dataplane rx threads — must not block.
    using InboundIpHandler = std::function<void(std::span<const uint8_t>)>;

    UserspaceDataplane() = default;
    ~UserspaceDataplane();

    UserspaceDataplane(const UserspaceDataplane&)            = delete;
    UserspaceDataplane& operator=(const UserspaceDataplane&) = delete;

    /// Bind the UDP socket and start rx/timer threads.
    [[nodiscard]] bool start(const Config& config);
    void stop();
    [[nodiscard]] bool is_running() const { return running_.load(std::memory_order_relaxed); }

    /// Port the socket actually bound (useful when config.listen_port == 0).
    [[nodiscard]] uint16_t bound_port() const { return bound_port_; }

    // ---- control plane -----------------------------------------------------

    /// Register a virtual local address (host order) the router answers for.
    void add_local_ip(uint32_t ip_host_order);
    void remove_local_ip(uint32_t ip_host_order);

    [[nodiscard]] bool add_peer(const std::string& pubkey_b64,
                                const std::string& allowed_ips,
                                const std::string& endpoint,
                                uint16_t persistent_keepalive = 25);

    /// Add an identity-keyed E2E session for the routing layer: keyed purely by
    /// the peer's Noise static (which the caller MUST take from a verified,
    /// root-signed IdentityBinding), with NO virtual-IP routing — it is not
    /// added to the cryptokey router. Decrypted frames are delivered to `sink`.
    /// This is the add-time authorization gate: only an authorized peer is ever
    /// added, and revocation is `remove_peer`. The HELLO handshake and payload
    /// gate are an app-layer protocol carried over this session (the dataplane
    /// only moves encrypted IP frames).
    [[nodiscard]] bool add_identity_session(const std::string& pubkey_b64,
                                            const std::string& conn_id,
                                            const std::string& endpoint,
                                            bool is_initiator,
                                            InboundIpHandler sink,
                                            uint16_t persistent_keepalive = 25);

    /// Encrypt and send an IP packet over an existing identity session.
    [[nodiscard]] bool send_on_session(const std::string& pubkey_b64,
                                       std::span<const uint8_t> ip_packet);

    [[nodiscard]] bool remove_peer(const std::string& pubkey_b64);
    [[nodiscard]] bool has_peer(const std::string& pubkey_b64) const;
    [[nodiscard]] bool update_endpoint(const std::string& pubkey_b64,
                                       const std::string& endpoint);
    [[nodiscard]] std::vector<BoringtunPeer> snapshot_peers() const;
    [[nodiscard]] size_t peer_count() const;

    // ---- termination-layer seam ---------------------------------------------

    void set_inbound_handler(InboundIpHandler handler);

    /// Route + encrypt a locally-originated IP packet. Returns false if the
    /// destination has no route or no usable endpoint.
    bool send_outbound_ip_packet(std::span<const uint8_t> ip_packet);

private:
    struct Peer {
        Tunn*            tunn{nullptr};
        uint32_t         index{0};        ///< 24-bit tunnel index (>=1)
        std::string      pubkey_b64;
        std::string      allowed_ips;
        std::vector<Cidr> cidrs;
        /// Packed endpoint: (ipv4_host << 16) | port. 0 = unknown (NATed
        /// client not yet heard from). Atomic so the hot path never takes
        /// the table lock to roam.
        std::atomic<uint64_t> endpoint{0};
        uint16_t         keepalive{25};

        // Identity-keyed routing-layer session: not in the IpRouter; decrypted
        // frames go straight to `sink` (set once at creation, never reassigned).
        bool             identity_keyed{false};
        std::string      conn_id;
        InboundIpHandler sink;

        ~Peer();
    };
    using PeerPtr = std::shared_ptr<Peer>;

    void rx_loop();
    void timer_loop();

    /// Process one received datagram.
    void handle_datagram(std::span<const uint8_t> pkt, uint64_t from_packed,
                         std::vector<uint8_t>& scratch_a,
                         std::vector<uint8_t>& scratch_b);

    /// Route a decrypted IPv4 packet from `src` (already authenticated).
    void route_decrypted(std::span<const uint8_t> ip_pkt, const PeerPtr& src,
                         std::vector<uint8_t>& scratch);

    /// Encrypt `ip_pkt` for `dst` and send it to its current endpoint.
    bool encrypt_and_send(const PeerPtr& dst, std::span<const uint8_t> ip_pkt,
                          std::vector<uint8_t>& scratch);

    /// Drain queued follow-up output from a Tunn after a read/handshake.
    void drain_followups(const PeerPtr& peer, uint64_t to_packed,
                         std::vector<uint8_t>& scratch_a,
                         std::vector<uint8_t>& scratch_b);

    void send_udp(const uint8_t* data, size_t len, uint64_t to_packed);

    [[nodiscard]] PeerPtr peer_by_index(uint32_t index) const;
    [[nodiscard]] PeerPtr peer_by_pubkey(const std::string& pubkey_b64) const;

    Config config_;
    std::atomic<bool> running_{false};
    uint16_t bound_port_{0};
    intptr_t socket_{-1};

    mutable std::shared_mutex tables_mtx_;
    std::unordered_map<std::string, PeerPtr> by_pubkey_;
    std::unordered_map<uint32_t, PeerPtr>    by_index_;
    IpRouter<PeerPtr>                        router_;
    uint32_t                                 next_index_{1};
    std::vector<uint32_t>                    free_indices_;

    mutable std::shared_mutex inbound_mtx_;
    InboundIpHandler          inbound_;

    std::vector<std::thread> rx_workers_;
    std::thread              timer_thread_;
};

} // namespace nexus::boringtun
