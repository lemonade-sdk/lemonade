#include <LemonadeNexusSDK/Transport/UserspaceDataplane.hpp>

#include <LemonadeNexusSDK/Transport/WireProtocol.hpp>

#include <boringtun_ffi.h>
#include <sodium.h>
#include <spdlog/spdlog.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
static void close_socket(socket_t s) { closesocket(s); }
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
static void close_socket(socket_t s) { close(s); }
#endif

namespace nexus::boringtun {

namespace {

constexpr size_t kBufSize    = 65536;
constexpr int    kTimerSlots = 25;   // 25 slots x 10ms = every peer ticked per 250ms

/// Packed endpoint helpers: (ipv4_host << 16) | port, 0 = unknown.
uint64_t pack_endpoint(uint32_t ip_host, uint16_t port) {
    return (static_cast<uint64_t>(ip_host) << 16) | port;
}

sockaddr_in unpack_endpoint(uint64_t packed) {
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(static_cast<uint32_t>(packed >> 16));
    addr.sin_port        = htons(static_cast<uint16_t>(packed & 0xFFFF));
    return addr;
}

uint64_t pack_sockaddr(const sockaddr_in& addr) {
    return pack_endpoint(ntohl(addr.sin_addr.s_addr), ntohs(addr.sin_port));
}

std::string endpoint_to_string(uint64_t packed) {
    if (packed == 0) return {};
    uint32_t ip = static_cast<uint32_t>(packed >> 16);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u",
                  (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF,
                  static_cast<unsigned>(packed & 0xFFFF));
    return buf;
}

/// Parse "a.b.c.d:port" into a packed endpoint.
std::optional<uint64_t> parse_endpoint(const std::string& endpoint) {
    auto colon = endpoint.rfind(':');
    if (colon == std::string::npos || colon + 1 >= endpoint.size()) return std::nullopt;
    auto host = Cidr::parse(endpoint.substr(0, colon));
    if (!host || host->prefix_len != 32) return std::nullopt;
    int port = 0;
    try {
        port = std::stoi(endpoint.substr(colon + 1));
    } catch (...) {
        return std::nullopt;
    }
    if (port <= 0 || port > 65535) return std::nullopt;
    return pack_endpoint(host->network, static_cast<uint16_t>(port));
}

std::optional<std::array<uint8_t, 32>> decode_key(const std::string& b64) {
    std::array<uint8_t, 32> raw{};
    size_t len = 0;
    if (sodium_base642bin(raw.data(), raw.size(), b64.c_str(), b64.size(),
                          nullptr, &len, nullptr,
                          sodium_base64_VARIANT_ORIGINAL) != 0 || len != 32)
        return std::nullopt;
    return raw;
}

std::string encode_key(const uint8_t* raw32) {
    char b64[sodium_base64_ENCODED_LEN(32, sodium_base64_VARIANT_ORIGINAL)];
    sodium_bin2base64(b64, sizeof(b64), raw32, 32, sodium_base64_VARIANT_ORIGINAL);
    return b64;
}

} // namespace

UserspaceDataplane::Peer::~Peer() {
    if (tunn) tunnel_free(tunn);
}

UserspaceDataplane::~UserspaceDataplane() {
    stop();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool UserspaceDataplane::start(const Config& config) {
    if (running_) return false;
    if (!decode_key(config.private_key_b64) || !decode_key(config.public_key_b64)) {
        spdlog::error("[dataplane] start: invalid keypair");
        return false;
    }
    config_ = config;

#ifdef _WIN32
    static std::once_flag wsa_once;
    std::call_once(wsa_once, [] {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    });
#endif

    socket_t sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == kInvalidSocket) {
        spdlog::error("[dataplane] failed to create UDP socket");
        return false;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    int bufsize = 1 << 20;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&bufsize), sizeof(bufsize));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&bufsize), sizeof(bufsize));
    // Receive timeout so rx workers notice shutdown promptly.
#ifdef _WIN32
    DWORD timeout_ms = 250;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv{};
    tv.tv_usec = 250 * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    sockaddr_in bind_addr{};
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port        = htons(config.listen_port);
    if (bind(sock, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
        spdlog::error("[dataplane] failed to bind UDP port {}", config.listen_port);
        close_socket(sock);
        return false;
    }

    sockaddr_in actual{};
    socklen_t actual_len = sizeof(actual);
    getsockname(sock, reinterpret_cast<sockaddr*>(&actual), &actual_len);
    bound_port_ = ntohs(actual.sin_port);

    socket_  = static_cast<intptr_t>(sock);
    running_ = true;

    int workers = config.rx_threads > 0 ? config.rx_threads : 1;
    rx_workers_.reserve(workers);
    for (int i = 0; i < workers; ++i)
        rx_workers_.emplace_back([this] { rx_loop(); });
    timer_thread_ = std::thread([this] { timer_loop(); });

    spdlog::info("[dataplane] started on UDP :{} ({} rx workers, no kernel interface)",
                 bound_port_, workers);
    return true;
}

void UserspaceDataplane::stop() {
    if (!running_.exchange(false)) return;

    for (auto& t : rx_workers_)
        if (t.joinable()) t.join();
    rx_workers_.clear();
    if (timer_thread_.joinable()) timer_thread_.join();

    if (socket_ != static_cast<intptr_t>(kInvalidSocket)) {
        close_socket(static_cast<socket_t>(socket_));
        socket_ = static_cast<intptr_t>(kInvalidSocket);
    }

    std::unique_lock lock(tables_mtx_);
    for (auto& [pubkey, peer] : by_pubkey_)
        router_.remove_routes_for(peer);
    by_pubkey_.clear();
    by_index_.clear();
    spdlog::info("[dataplane] stopped");
}

// ---------------------------------------------------------------------------
// Control plane
// ---------------------------------------------------------------------------

void UserspaceDataplane::add_local_ip(uint32_t ip_host_order) {
    std::unique_lock lock(tables_mtx_);
    router_.add_local_ip(ip_host_order);
}

void UserspaceDataplane::remove_local_ip(uint32_t ip_host_order) {
    std::unique_lock lock(tables_mtx_);
    router_.remove_local_ip(ip_host_order);
}

bool UserspaceDataplane::add_peer(const std::string& pubkey_b64,
                                  const std::string& allowed_ips,
                                  const std::string& endpoint,
                                  uint16_t persistent_keepalive) {
    if (!decode_key(pubkey_b64)) {
        spdlog::warn("[dataplane] add_peer: invalid pubkey '{}'", pubkey_b64);
        return false;
    }
    if (pubkey_b64 == config_.public_key_b64) {
        spdlog::warn("[dataplane] add_peer: refusing to add ourselves as a peer");
        return false;
    }
    auto cidrs = Cidr::parse_list(allowed_ips);
    if (cidrs.empty()) {
        spdlog::warn("[dataplane] add_peer {}: no valid allowed IPs in '{}'",
                     pubkey_b64, allowed_ips);
        return false;
    }
    uint64_t packed_endpoint = 0;
    if (!endpoint.empty()) {
        auto parsed = parse_endpoint(endpoint);
        if (!parsed) {
            spdlog::warn("[dataplane] add_peer {}: bad endpoint '{}'", pubkey_b64, endpoint);
            return false;
        }
        packed_endpoint = *parsed;
    }

    PeerPtr peer;
    {
        std::unique_lock lock(tables_mtx_);
        if (auto it = by_pubkey_.find(pubkey_b64); it != by_pubkey_.end()) {
            // Update in place: routes and endpoint may have changed.
            it->second->allowed_ips = allowed_ips;
            it->second->cidrs       = cidrs;
            router_.remove_routes_for(it->second);
            router_.add_routes(cidrs, it->second);
            if (packed_endpoint)
                it->second->endpoint.store(packed_endpoint, std::memory_order_relaxed);
            return true;
        }

        uint32_t index;
        if (!free_indices_.empty()) {
            index = free_indices_.back();
            free_indices_.pop_back();
        } else {
            index = next_index_++;
        }

        Tunn* tunn = new_tunnel(config_.private_key_b64.c_str(), pubkey_b64.c_str(),
                                nullptr, persistent_keepalive, index);
        if (!tunn) {
            free_indices_.push_back(index);
            spdlog::error("[dataplane] new_tunnel failed for peer {}", pubkey_b64);
            return false;
        }

        peer              = std::make_shared<Peer>();
        peer->tunn        = tunn;
        peer->index       = index;
        peer->pubkey_b64  = pubkey_b64;
        peer->allowed_ips = allowed_ips;
        peer->cidrs       = std::move(cidrs);
        peer->keepalive   = persistent_keepalive;
        peer->endpoint.store(packed_endpoint, std::memory_order_relaxed);

        by_pubkey_[pubkey_b64] = peer;
        by_index_[index]       = peer;
        router_.add_routes(peer->cidrs, peer);
    }

    // Known endpoint (backbone peer): initiate immediately rather than
    // waiting for traffic — gossip expects the tunnel to come up proactively.
    if (packed_endpoint && running_) {
        std::vector<uint8_t> buf(wire::kMaxOverhead + 64);
        auto r = wireguard_force_handshake(peer->tunn, buf.data(),
                                           static_cast<uint32_t>(buf.size()));
        if (r.op == WRITE_TO_NETWORK && r.size > 0)
            send_udp(buf.data(), r.size, packed_endpoint);
    }

    spdlog::debug("[dataplane] added peer {} (allowed: {}, endpoint: {})",
                  pubkey_b64, allowed_ips, endpoint.empty() ? "<roaming>" : endpoint);
    return true;
}

bool UserspaceDataplane::add_identity_session(const std::string& pubkey_b64,
                                              const std::string& conn_id,
                                              const std::string& endpoint,
                                              bool is_initiator,
                                              InboundIpHandler sink,
                                              uint16_t persistent_keepalive) {
    if (!decode_key(pubkey_b64)) {
        spdlog::warn("[dataplane] add_identity_session: invalid pubkey '{}'", pubkey_b64);
        return false;
    }
    if (pubkey_b64 == config_.public_key_b64) {
        spdlog::warn("[dataplane] add_identity_session: refusing to add ourselves");
        return false;
    }
    uint64_t packed_endpoint = 0;
    if (!endpoint.empty()) {
        auto parsed = parse_endpoint(endpoint);
        if (!parsed) {
            spdlog::warn("[dataplane] add_identity_session {}: bad endpoint '{}'",
                         pubkey_b64, endpoint);
            return false;
        }
        packed_endpoint = *parsed;
    }

    PeerPtr peer;
    {
        std::unique_lock lock(tables_mtx_);
        if (auto it = by_pubkey_.find(pubkey_b64); it != by_pubkey_.end()) {
            // Refuse to shadow a routed peer with the same static; for an
            // existing identity session just refresh its endpoint.
            if (!it->second->identity_keyed) {
                spdlog::warn("[dataplane] add_identity_session {}: static already "
                             "used by a routed peer", pubkey_b64);
                return false;
            }
            if (packed_endpoint)
                it->second->endpoint.store(packed_endpoint, std::memory_order_relaxed);
            return true;
        }

        uint32_t index;
        if (!free_indices_.empty()) {
            index = free_indices_.back();
            free_indices_.pop_back();
        } else {
            index = next_index_++;
        }

        Tunn* tunn = new_tunnel(config_.private_key_b64.c_str(), pubkey_b64.c_str(),
                                nullptr, persistent_keepalive, index);
        if (!tunn) {
            free_indices_.push_back(index);
            spdlog::error("[dataplane] new_tunnel failed for identity session {}", pubkey_b64);
            return false;
        }

        peer                = std::make_shared<Peer>();
        peer->tunn          = tunn;
        peer->index         = index;
        peer->pubkey_b64    = pubkey_b64;
        peer->keepalive     = persistent_keepalive;
        peer->identity_keyed= true;
        peer->conn_id       = conn_id;
        peer->sink          = std::move(sink);
        peer->endpoint.store(packed_endpoint, std::memory_order_relaxed);

        by_pubkey_[pubkey_b64] = peer;
        by_index_[index]       = peer;
        // Deliberately NOT added to router_ — identity sessions are keyed by the
        // Noise static / receiver index, never by a virtual IP.
    }

    if (is_initiator && packed_endpoint && running_) {
        std::vector<uint8_t> buf(wire::kMaxOverhead + 64);
        auto r = wireguard_force_handshake(peer->tunn, buf.data(),
                                           static_cast<uint32_t>(buf.size()));
        if (r.op == WRITE_TO_NETWORK && r.size > 0)
            send_udp(buf.data(), r.size, packed_endpoint);
    }

    spdlog::debug("[dataplane] added identity session {} (conn_id={}, endpoint={})",
                  pubkey_b64, conn_id, endpoint.empty() ? "<roaming>" : endpoint);
    return true;
}

bool UserspaceDataplane::send_on_session(const std::string& pubkey_b64,
                                         std::span<const uint8_t> ip_packet) {
    PeerPtr peer = peer_by_pubkey(pubkey_b64);
    if (!peer || !peer->identity_keyed) return false;
    thread_local std::vector<uint8_t> scratch(kBufSize + wire::kMaxOverhead);
    return encrypt_and_send(peer, ip_packet, scratch);
}

bool UserspaceDataplane::remove_peer(const std::string& pubkey_b64) {
    std::unique_lock lock(tables_mtx_);
    auto it = by_pubkey_.find(pubkey_b64);
    if (it == by_pubkey_.end()) return false;

    PeerPtr peer = it->second;
    router_.remove_routes_for(peer);
    by_index_.erase(peer->index);
    free_indices_.push_back(peer->index);
    by_pubkey_.erase(it);
    // The Tunn is freed by ~Peer once in-flight rx workers drop their refs.
    return true;
}

bool UserspaceDataplane::has_peer(const std::string& pubkey_b64) const {
    std::shared_lock lock(tables_mtx_);
    return by_pubkey_.count(pubkey_b64) > 0;
}

bool UserspaceDataplane::update_endpoint(const std::string& pubkey_b64,
                                         const std::string& endpoint) {
    auto parsed = parse_endpoint(endpoint);
    if (!parsed) return false;

    PeerPtr peer = peer_by_pubkey(pubkey_b64);
    if (!peer) return false;
    peer->endpoint.store(*parsed, std::memory_order_relaxed);

    // The old path may be dead — rekey toward the new endpoint.
    if (running_) {
        std::vector<uint8_t> buf(wire::kMaxOverhead + 64);
        auto r = wireguard_force_handshake(peer->tunn, buf.data(),
                                           static_cast<uint32_t>(buf.size()));
        if (r.op == WRITE_TO_NETWORK && r.size > 0)
            send_udp(buf.data(), r.size, *parsed);
    }
    return true;
}

std::vector<BoringtunPeer> UserspaceDataplane::snapshot_peers() const {
    std::vector<PeerPtr> peers;
    {
        std::shared_lock lock(tables_mtx_);
        peers.reserve(by_pubkey_.size());
        for (const auto& [pubkey, peer] : by_pubkey_) peers.push_back(peer);
    }

    std::vector<BoringtunPeer> out;
    out.reserve(peers.size());
    const auto now = static_cast<uint64_t>(std::time(nullptr));
    for (const auto& peer : peers) {
        BoringtunPeer snapshot;
        snapshot.public_key           = peer->pubkey_b64;
        snapshot.allowed_ips          = peer->allowed_ips;
        snapshot.endpoint             = endpoint_to_string(peer->endpoint.load(std::memory_order_relaxed));
        snapshot.persistent_keepalive = peer->keepalive;
        auto st = wireguard_stats(peer->tunn);
        if (st.time_since_last_handshake >= 0)
            snapshot.last_handshake = now - static_cast<uint64_t>(st.time_since_last_handshake);
        snapshot.rx_bytes = st.rx_bytes;
        snapshot.tx_bytes = st.tx_bytes;
        out.push_back(std::move(snapshot));
    }
    return out;
}

size_t UserspaceDataplane::peer_count() const {
    std::shared_lock lock(tables_mtx_);
    return by_pubkey_.size();
}

void UserspaceDataplane::set_inbound_handler(InboundIpHandler handler) {
    std::unique_lock lock(inbound_mtx_);
    inbound_ = std::move(handler);
}

// ---------------------------------------------------------------------------
// Data path
// ---------------------------------------------------------------------------

bool UserspaceDataplane::send_outbound_ip_packet(std::span<const uint8_t> ip_packet) {
    auto dst = wire::ipv4::dst_addr(ip_packet);
    if (!dst) return false;

    IpRouter<PeerPtr>::Result route;
    {
        std::shared_lock lock(tables_mtx_);
        route = router_.lookup(*dst);
    }
    if (route.verdict != IpRouter<PeerPtr>::Verdict::Peer) return false;

    thread_local std::vector<uint8_t> scratch(kBufSize + wire::kMaxOverhead);
    return encrypt_and_send(route.peer, ip_packet, scratch);
}

bool UserspaceDataplane::encrypt_and_send(const PeerPtr& dst,
                                          std::span<const uint8_t> ip_pkt,
                                          std::vector<uint8_t>& scratch) {
    auto r = wireguard_write(dst->tunn, ip_pkt.data(),
                             static_cast<uint32_t>(ip_pkt.size()),
                             scratch.data(), static_cast<uint32_t>(scratch.size()));
    if (r.op != WRITE_TO_NETWORK || r.size == 0)
        return r.op == WIREGUARD_DONE;  // queued pending handshake

    uint64_t endpoint = dst->endpoint.load(std::memory_order_relaxed);
    if (!endpoint) return false;
    send_udp(scratch.data(), r.size, endpoint);
    return true;
}

void UserspaceDataplane::send_udp(const uint8_t* data, size_t len, uint64_t to_packed) {
    sockaddr_in addr = unpack_endpoint(to_packed);
    sendto(static_cast<socket_t>(socket_), reinterpret_cast<const char*>(data),
#ifdef _WIN32
           static_cast<int>(len),
#else
           len,
#endif
           0, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
}

UserspaceDataplane::PeerPtr UserspaceDataplane::peer_by_index(uint32_t index) const {
    std::shared_lock lock(tables_mtx_);
    auto it = by_index_.find(index);
    return it == by_index_.end() ? nullptr : it->second;
}

UserspaceDataplane::PeerPtr
UserspaceDataplane::peer_by_pubkey(const std::string& pubkey_b64) const {
    std::shared_lock lock(tables_mtx_);
    auto it = by_pubkey_.find(pubkey_b64);
    return it == by_pubkey_.end() ? nullptr : it->second;
}

void UserspaceDataplane::rx_loop() {
    std::vector<uint8_t> rx_buf(kBufSize);
    std::vector<uint8_t> scratch_a(kBufSize + wire::kMaxOverhead);
    std::vector<uint8_t> scratch_b(kBufSize + wire::kMaxOverhead);

    while (running_) {
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        auto n = recvfrom(static_cast<socket_t>(socket_),
                          reinterpret_cast<char*>(rx_buf.data()),
#ifdef _WIN32
                          static_cast<int>(rx_buf.size()),
#else
                          rx_buf.size(),
#endif
                          0, reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n <= 0) continue;  // timeout / shutdown / error

        handle_datagram({rx_buf.data(), static_cast<size_t>(n)},
                        pack_sockaddr(from), scratch_a, scratch_b);
    }
}

void UserspaceDataplane::handle_datagram(std::span<const uint8_t> pkt,
                                         uint64_t from_packed,
                                         std::vector<uint8_t>& scratch_a,
                                         std::vector<uint8_t>& scratch_b) {
    auto type = wire::parse_type(pkt);
    if (!type) return;  // not a boringtun/Noise packet

    PeerPtr peer;
    if (*type == wire::MsgType::HandshakeInit) {
        // Rare path: one anonymous parse identifies the initiator, then the
        // peer's own Tunn fully verifies and answers.
        std::array<uint8_t, 32> initiator{};
        if (wireguard_parse_handshake_anon(config_.private_key_b64.c_str(),
                                           config_.public_key_b64.c_str(),
                                           pkt.data(), static_cast<uint32_t>(pkt.size()),
                                           initiator.data()) != 0)
            return;
        peer = peer_by_pubkey(encode_key(initiator.data()));
        if (!peer) return;  // unknown initiator — drop
    } else {
        auto receiver = wire::receiver_index(pkt);
        if (!receiver) return;
        peer = peer_by_index(wire::peer_index(*receiver));
        if (!peer) return;
    }

    auto r = wireguard_read(peer->tunn, pkt.data(), static_cast<uint32_t>(pkt.size()),
                            scratch_a.data(), static_cast<uint32_t>(scratch_a.size()));

    switch (r.op) {
        case WRITE_TO_NETWORK:
            // Valid handshake initiation/response — reply to where it came from
            // (authenticated by the Noise handshake itself).
            send_udp(scratch_a.data(), r.size, from_packed);
            peer->endpoint.store(from_packed, std::memory_order_relaxed);
            drain_followups(peer, from_packed, scratch_a, scratch_b);
            break;

        case WRITE_TO_TUNNEL_IPV4:
            // Authenticated transport data — roaming endpoint update.
            peer->endpoint.store(from_packed, std::memory_order_relaxed);
            route_decrypted({scratch_a.data(), r.size}, peer, scratch_b);
            drain_followups(peer, from_packed, scratch_a, scratch_b);
            break;

        case WIREGUARD_DONE:
            // For transport packets this means an authenticated keepalive.
            if (*type == wire::MsgType::TransportData)
                peer->endpoint.store(from_packed, std::memory_order_relaxed);
            break;

        case WRITE_TO_TUNNEL_IPV6:  // IPv4-only mesh — drop explicitly
        case WIREGUARD_ERROR:
        default:
            break;
    }
}

void UserspaceDataplane::drain_followups(const PeerPtr& peer, uint64_t to_packed,
                                         std::vector<uint8_t>& scratch_a,
                                         std::vector<uint8_t>& scratch_b) {
    // After a handshake completes boringtun queues the response/keepalive and
    // any packets buffered while no session existed. The empty read must pass
    // a valid pointer: the FFI builds a Rust slice from it, and a null slice
    // base is undefined behavior there even at length zero.
    static constexpr uint8_t kEmpty[1] = {0};
    while (true) {
        auto r = wireguard_read(peer->tunn, kEmpty, 0, scratch_a.data(),
                                static_cast<uint32_t>(scratch_a.size()));
        if (r.op == WRITE_TO_NETWORK && r.size > 0) {
            send_udp(scratch_a.data(), r.size, to_packed);
        } else if (r.op == WRITE_TO_TUNNEL_IPV4 && r.size > 0) {
            route_decrypted({scratch_a.data(), r.size}, peer, scratch_b);
        } else {
            break;
        }
    }
}

void UserspaceDataplane::route_decrypted(std::span<const uint8_t> ip_pkt,
                                         const PeerPtr& src,
                                         std::vector<uint8_t>& scratch) {
    // Identity-keyed routing-layer session: authentication is the Noise static
    // itself (the peer was authorized at add time), so there is no virtual-IP
    // scope to check and no cryptokey routing — deliver straight to the session
    // sink. The app layer runs HELLO and gates payload over this stream.
    if (src->identity_keyed) {
        if (src->sink) src->sink(ip_pkt);
        return;
    }

    auto src_ip = wire::ipv4::src_addr(ip_pkt);
    auto dst_ip = wire::ipv4::dst_addr(ip_pkt);
    if (!src_ip || !dst_ip) return;

    // Cryptokey routing source check: the inner source
    // address must be inside the sending peer's allowed IPs, or a peer could
    // spoof traffic from addresses it does not own.
    bool src_allowed = false;
    for (const auto& cidr : src->cidrs) {
        if (cidr.contains(*src_ip)) {
            src_allowed = true;
            break;
        }
    }
    if (!src_allowed) {
        spdlog::debug("[dataplane] dropped packet from {} with spoofed source",
                      src->pubkey_b64);
        return;
    }

    IpRouter<PeerPtr>::Result route;
    {
        std::shared_lock lock(tables_mtx_);
        route = router_.lookup(*dst_ip);
    }
    switch (route.verdict) {
        case IpRouter<PeerPtr>::Verdict::Local: {
            std::shared_lock lock(inbound_mtx_);
            if (inbound_) inbound_(ip_pkt);
            break;
        }
        case IpRouter<PeerPtr>::Verdict::Peer:
            if (route.peer != src)  // no hairpin back out the same peer
                encrypt_and_send(route.peer, ip_pkt, scratch);
            break;
        case IpRouter<PeerPtr>::Verdict::Drop:
            break;
    }
}

void UserspaceDataplane::timer_loop() {
    std::vector<uint8_t> buf(wire::kMaxOverhead + 64);
    std::vector<PeerPtr> due;
    int slot = 0;

    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Staggered ticking: each peer is serviced every kTimerSlots * 10ms
        // (= 250ms), but the work is spread so large peer tables never tick
        // in one burst.
        due.clear();
        {
            std::shared_lock lock(tables_mtx_);
            for (const auto& [index, peer] : by_index_)
                if (static_cast<int>(index % kTimerSlots) == slot) due.push_back(peer);
        }
        for (const auto& peer : due) {
            auto r = wireguard_tick(peer->tunn, buf.data(),
                                    static_cast<uint32_t>(buf.size()));
            if (r.op == WRITE_TO_NETWORK && r.size > 0) {
                uint64_t endpoint = peer->endpoint.load(std::memory_order_relaxed);
                if (endpoint) send_udp(buf.data(), r.size, endpoint);
            }
        }
        slot = (slot + 1) % kTimerSlots;
    }
}

} // namespace nexus::boringtun
