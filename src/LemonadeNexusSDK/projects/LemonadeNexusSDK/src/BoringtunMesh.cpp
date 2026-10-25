#include <LemonadeNexusSDK/BoringtunMesh.hpp>
#include <algorithm>

#include <LemonadeNexus/Boringtun/IpRouter.hpp>
#include <LemonadeNexus/Boringtun/UserspaceDataplane.hpp>
#include <LemonadeNexus/Network/StunWire.hpp>
#include <LemonadeNexus/Relay/RelayWire.hpp>

#include <virtual_netstack.h>
#include <sodium.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_set>
#include <utility>

namespace lnsdk {

using nexus::boringtun::Cidr;
using nexus::boringtun::UserspaceDataplane;

namespace {

std::string addr_part(const std::string& cidr) {
    auto slash = cidr.find('/');
    return slash == std::string::npos ? cidr : cidr.substr(0, slash);
}

int prefix_part(const std::string& cidr, int fallback) {
    auto slash = cidr.find('/');
    if (slash == std::string::npos) return fallback;
    try {
        return std::stoi(cidr.substr(slash + 1));
    } catch (...) {
        return fallback;
    }
}

/// The private pool IPAM assigns peer subnets from (IPAMService kPrivateBase).
constexpr const char* kPrivatePool = "10.128.0.0/9";
/// The largest peer subnet accepted (IPAM allocates /24 to /30 blocks).
constexpr uint8_t kMaxSubnetPrefixLen = 24;

bool overlaps(const Cidr& a, const Cidr& b) {
    return a.contains(b.network) || b.contains(a.network);
}

/// `inner` lies entirely inside `outer`.
bool within(const Cidr& inner, const Cidr& outer) {
    return inner.prefix_len >= outer.prefix_len && outer.contains(inner.network);
}

/// A tunnel address: one address, bare or "/32".
std::optional<Cidr> parse_tunnel_address(const std::string& text) {
    if (text.empty()) return std::nullopt;
    const auto slash = text.find('/');
    if (slash != std::string::npos && text.substr(slash) != "/32") return std::nullopt;
    auto cidr = Cidr::parse(text);
    if (!cidr || cidr->prefix_len != 32) return std::nullopt;
    return cidr;
}

/// A private subnet given as "network/prefix", with no host bits set.
std::optional<Cidr> parse_subnet(const std::string& text) {
    if (text.find('/') == std::string::npos) return std::nullopt;
    auto cidr = Cidr::parse(text);
    if (!cidr) return std::nullopt;
    const auto exact = Cidr::parse(addr_part(text) + "/32");
    if (!exact || exact->network != cidr->network) return std::nullopt;
    return cidr;
}

/// The netstack hands every outbound IP packet here; re-inject into the
/// dataplane for encryption + send. ctx is the UserspaceDataplane*.
void mesh_output_trampoline(void* ctx, const uint8_t* pkt, size_t len) {
    if (!ctx || !pkt || len == 0) return;
    static_cast<UserspaceDataplane*>(ctx)->send_outbound_ip_packet({pkt, len});
}

}  // namespace

struct BoringtunMesh::Impl {
    UserspaceDataplane dp;
    std::string        server_pubkey;     // never removed by sync_peers
    std::atomic<bool>  active{false};
    BoringtunConfig    started_cfg;       // what the live plane was started with

    // Guards ns, tunnel_addr, and egress_cache. stop() destroys the netstack
    // while egress and ingress calls may run on other threads, so every read
    // or write of ns must hold this lock.
    std::mutex                                            ns_mtx;
    NsHandle*                                             ns{nullptr};
    std::string                                           tunnel_addr;  // our virtual IP (no prefix), egress source
    std::map<std::pair<std::string, uint16_t>, uint16_t>  egress_cache;

    // Peers whose traffic goes through a relay: pubkey -> {local shim
    // endpoint, relay endpoint}. Bounded by the peer set; guarded by relay_mtx.
    struct RelayPath {
        std::string local_endpoint;
        std::string relay_endpoint;
    };
    mutable std::mutex                  relay_mtx;
    std::map<std::string, RelayPath>    relay_paths;
    // The endpoint sync_peers or set_peer_relay last wrote for each peer.
    // An endpoint is written again only when it changes. Guarded by relay_mtx.
    std::map<std::string, std::string>  written_endpoints;

    // One reflexive-address query at a time (query_mtx). The rx handler
    // matches a reply against stun_txn and stun_server under stun_mtx.
    std::mutex                                         query_mtx;
    std::mutex                                         stun_mtx;
    std::condition_variable                            stun_cv;
    std::optional<nexus::network::stun::TransactionId> stun_txn;
    std::string                                        stun_server;
    std::optional<nexus::network::stun::MappedAddress> stun_result;

    void on_stun(std::span<const uint8_t> msg, const std::string& from) {
        std::lock_guard lock(stun_mtx);
        if (!stun_txn || stun_result || from != stun_server) return;
        auto mapped = nexus::network::stun::decode_binding_response(msg, *stun_txn);
        if (!mapped) return;
        stun_result = mapped;
        stun_cv.notify_all();
    }
};

namespace {

std::optional<std::array<uint8_t, 32>> decode_key32(const std::string& b64) {
    std::array<uint8_t, 32> raw{};
    size_t len = 0;
    if (sodium_base642bin(raw.data(), raw.size(), b64.c_str(), b64.size(), nullptr, &len,
                          nullptr, sodium_base64_VARIANT_ORIGINAL) != 0 ||
        len != raw.size()) {
        return std::nullopt;
    }
    return raw;
}

}  // namespace

BoringtunMesh::BoringtunMesh() : impl_(std::make_unique<Impl>()) {}

BoringtunMesh::~BoringtunMesh() {
    stop();
}

bool BoringtunMesh::start(const BoringtunConfig& config) {
    if (impl_->active.load()) {
        const auto& live = impl_->started_cfg;
        if (live.private_key == config.private_key &&
            live.tunnel_ip == config.tunnel_ip &&
            live.server_public_key == config.server_public_key &&
            live.server_endpoint == config.server_endpoint) {
            return true;
        }
        // A re-join issues a fresh mesh IP/keys. Keeping the old plane would
        // leave us handshaking with credentials the server has replaced.
        spdlog::info("[BoringtunMesh] config changed — restarting dataplane");
        stop();
    }
    if (config.private_key.empty() || config.public_key.empty()) return false;

    UserspaceDataplane::Config dpcfg;
    dpcfg.private_key_b64 = config.private_key;
    dpcfg.public_key_b64  = config.public_key;
    dpcfg.listen_port     = config.listen_port;  // 0 = random

    NsHandle* ns = ns_create(dpcfg.mtu, &mesh_output_trampoline, &impl_->dp);
    if (!ns) return false;

    // dp.stop() joins the receive workers, so this handler never runs after
    // stop() destroys the netstack.
    impl_->dp.set_inbound_handler([ns](std::span<const uint8_t> pkt) {
        if (!pkt.empty()) ns_feed_inbound(ns, pkt.data(), pkt.size());
    });
    impl_->dp.set_stun_handler([impl = impl_.get()](std::span<const uint8_t> msg,
                                                    const std::string& from) {
        impl->on_stun(msg, from);
    });

    if (!impl_->dp.start(dpcfg)) {
        ns_destroy(ns);
        return false;
    }

    // Register our virtual address; widen the netstack prefix to the mesh plane
    // (from the first allowed-IPs entry) so mesh peers are on-link for egress.
    const std::string tunnel_addr = addr_part(config.tunnel_ip);
    if (!tunnel_addr.empty()) {
        if (auto c = Cidr::parse(tunnel_addr); c && c->prefix_len == 32) {
            impl_->dp.add_local_ip(c->network);
        }
        int prefix = 10;
        if (!config.allowed_ips.empty()) {
            prefix = prefix_part(config.allowed_ips.front(), 10);
        }
        ns_add_local_ip(ns, (tunnel_addr + "/" + std::to_string(prefix)).c_str());
    }
    {
        std::lock_guard lock(impl_->ns_mtx);
        impl_->ns          = ns;
        impl_->tunnel_addr = tunnel_addr;
    }

    // Add the server as the initial peer (its allowed-ips cover the mesh plane,
    // so private-API egress to the server tunnel IP cryptokey-routes here).
    impl_->server_pubkey = config.server_public_key;
    if (!impl_->server_pubkey.empty()) {
        std::string allowed;
        for (const auto& a : config.allowed_ips) {
            if (!allowed.empty()) allowed += ",";
            allowed += a;
        }
        if (allowed.empty()) allowed = "0.0.0.0/0";
        (void)impl_->dp.add_peer(
            impl_->server_pubkey, allowed, config.server_endpoint,
            static_cast<uint16_t>(config.keepalive ? config.keepalive : 25));
    }

    impl_->started_cfg = config;
    impl_->active = true;
    spdlog::info("[BoringtunMesh] dataplane up: mesh_ip={}, server_peer={}…",
                 tunnel_addr,
                 impl_->server_pubkey.substr(0, std::min<size_t>(8, impl_->server_pubkey.size())));
    return true;
}

void BoringtunMesh::stop() {
    if (!impl_->active.exchange(false)) return;
    {
        std::lock_guard lock(impl_->relay_mtx);
        impl_->relay_paths.clear();
        impl_->written_endpoints.clear();
    }
    impl_->dp.stop();  // stop feeding the netstack before tearing it down
    NsHandle* ns = nullptr;
    {
        std::lock_guard lock(impl_->ns_mtx);
        ns = std::exchange(impl_->ns, nullptr);
        impl_->egress_cache.clear();
    }
    // No other thread can reach the handle now: each one reads ns under
    // ns_mtx and finds null. ns_destroy joins the stack thread, so it runs
    // outside the lock.
    ns_destroy(ns);
    spdlog::info("[BoringtunMesh] dataplane down");
}

bool BoringtunMesh::is_active() const {
    return impl_->active.load();
}

uint16_t BoringtunMesh::tcp_egress(const std::string& dst_ip, uint16_t dst_port) {
    if (!impl_->active.load()) return 0;
    std::lock_guard lock(impl_->ns_mtx);
    if (!impl_->ns || impl_->tunnel_addr.empty()) return 0;
    const auto key = std::make_pair(dst_ip, dst_port);
    if (auto it = impl_->egress_cache.find(key); it != impl_->egress_cache.end())
        return it->second;
    uint16_t local =
        ns_add_tcp_egress(impl_->ns, dst_ip.c_str(), dst_port, impl_->tunnel_addr.c_str());
    if (local != 0) {
        impl_->egress_cache[key] = local;
        spdlog::debug("[BoringtunMesh] egress 127.0.0.1:{} -> {}:{}", local, dst_ip, dst_port);
    }
    return local;
}

void BoringtunMesh::close_egress(const std::string& dst_ip, uint16_t dst_port) {
    std::lock_guard lock(impl_->ns_mtx);
    impl_->egress_cache.erase({dst_ip, dst_port});
    if (!impl_->ns) return;
    ns_remove_tcp_egress(impl_->ns, dst_ip.c_str(), dst_port);
    spdlog::debug("[BoringtunMesh] egress to {}:{} closed", dst_ip, dst_port);
}

bool BoringtunMesh::tcp_ingress(uint16_t vport, const std::string& target) {
    if (!impl_->active.load()) return false;
    if (vport == 0 || target.empty()) return false;
    std::lock_guard lock(impl_->ns_mtx);
    if (!impl_->ns || impl_->tunnel_addr.empty()) return false;
    if (ns_add_tcp_forward(impl_->ns, impl_->tunnel_addr.c_str(), vport,
                           target.c_str()) != 0) {
        spdlog::warn("[BoringtunMesh] ingress {}:{} -> {} failed",
                     impl_->tunnel_addr, vport, target);
        return false;
    }
    spdlog::info("[BoringtunMesh] ingress {}:{} -> {}",
                 impl_->tunnel_addr, vport, target);
    return true;
}

uint16_t BoringtunMesh::bound_port() const {
    return impl_->active.load() ? impl_->dp.bound_port() : 0;
}

std::string BoringtunMesh::server_endpoint() const {
    return impl_->active.load() ? impl_->started_cfg.server_endpoint : std::string{};
}

std::optional<std::string> BoringtunMesh::query_reflexive(const std::string& stun_endpoint,
                                                          std::chrono::milliseconds timeout,
                                                          uint32_t attempts) {
    namespace stun = nexus::network::stun;
    if (!impl_->active.load() || attempts == 0 || timeout.count() <= 0) return std::nullopt;
    std::lock_guard query(impl_->query_mtx);

    stun::TransactionId txn{};
    randombytes_buf(txn.data(), txn.size());
    const auto request = stun::encode_binding_request(txn);
    {
        std::lock_guard lock(impl_->stun_mtx);
        impl_->stun_txn = txn;
        impl_->stun_server = stun_endpoint;
        impl_->stun_result.reset();
    }

    // RFC 5389 resends the same request (same transaction id) on loss.
    const auto per_attempt = timeout / attempts;
    std::optional<stun::MappedAddress> result;
    for (uint32_t i = 0; i < attempts && !result; ++i) {
        if (!impl_->dp.send_stun(stun_endpoint, request)) break;
        std::unique_lock lock(impl_->stun_mtx);
        impl_->stun_cv.wait_for(lock, per_attempt,
                                [this] { return impl_->stun_result.has_value(); });
        result = impl_->stun_result;
    }
    {
        std::lock_guard lock(impl_->stun_mtx);
        impl_->stun_txn.reset();
        impl_->stun_server.clear();
        impl_->stun_result.reset();
    }
    if (!result) return std::nullopt;

    char buf[32];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u:%u", (result->ipv4 >> 24) & 0xFF,
                  (result->ipv4 >> 16) & 0xFF, (result->ipv4 >> 8) & 0xFF, result->ipv4 & 0xFF,
                  static_cast<unsigned>(result->port));
    return std::string(buf);
}

bool BoringtunMesh::probe_peer(const std::string& pubkey, const std::string& endpoint) {
    if (!impl_->active.load() || pubkey.empty() || pubkey == impl_->server_pubkey) return false;
    return impl_->dp.probe_endpoint(pubkey, endpoint);
}

StatusResult BoringtunMesh::sync_peers(const std::vector<MeshPeer>& desired) {
    StatusResult result;
    if (!impl_->active.load()) {
        result.error = "mesh dataplane not active";
        return result;
    }

    std::string own_tunnel_ip;
    {
        std::lock_guard lock(impl_->ns_mtx);
        own_tunnel_ip = impl_->tunnel_addr;
    }
    const auto allowed_list =
        peer_allowed_ips(desired, impl_->started_cfg.allowed_ips, own_tunnel_ip);

    std::map<std::string, Impl::RelayPath> relayed;
    std::map<std::string, std::string> written;
    {
        std::lock_guard lock(impl_->relay_mtx);
        relayed = impl_->relay_paths;
        written = impl_->written_endpoints;
    }

    std::unordered_set<std::string> want;
    for (std::size_t i = 0; i < desired.size(); ++i) {
        const auto& p = desired[i];
        if (p.mesh_pubkey.empty()) continue;
        const auto relay = relayed.find(p.mesh_pubkey);
        const std::string endpoint = relay != relayed.end() ? relay->second.local_endpoint
                                     : p.endpoint.empty()   ? p.relay_endpoint
                                                            : p.endpoint;
        const std::string& allowed = allowed_list[i];
        if (allowed.empty()) continue;
        want.insert(p.mesh_pubkey);
        // Write the endpoint only when it changed since the last write. The
        // dataplane moves a peer to the source of its authenticated packets,
        // and rewriting an unchanged listed endpoint would undo that move.
        const auto last = written.find(p.mesh_pubkey);
        const bool changed = last == written.end() || last->second != endpoint;
        if (impl_->dp.add_peer(p.mesh_pubkey, allowed, changed ? endpoint : std::string{},
                               static_cast<uint16_t>(p.keepalive ? p.keepalive : 25)) &&
            changed) {
            written[p.mesh_pubkey] = endpoint;
        }
    }

    // Remove peers the server no longer lists (never the server peer itself).
    for (const auto& peer : impl_->dp.snapshot_peers()) {
        if (peer.public_key == impl_->server_pubkey) continue;
        if (!want.count(peer.public_key)) (void)impl_->dp.remove_peer(peer.public_key);
    }
    {
        std::lock_guard lock(impl_->relay_mtx);
        std::erase_if(impl_->relay_paths,
                      [&want](const auto& entry) { return !want.count(entry.first); });
        std::erase_if(written, [&want](const auto& entry) { return !want.count(entry.first); });
        // A relay path set or cleared after the snapshot changed the endpoint
        // record; that newer record wins.
        for (const auto& [pubkey, path] : impl_->relay_paths) {
            if (want.count(pubkey)) written[pubkey] = path.local_endpoint;
        }
        for (const auto& [pubkey, path] : relayed) {
            if (!impl_->relay_paths.contains(pubkey)) written.erase(pubkey);
        }
        impl_->written_endpoints = std::move(written);
    }

    result.ok = true;
    return result;
}

std::vector<std::string> BoringtunMesh::peer_allowed_ips(
    const std::vector<MeshPeer>& peers, const std::vector<std::string>& tunnel_plane,
    const std::string& own_tunnel_ip) {
    std::vector<Cidr> plane;
    for (const auto& text : tunnel_plane) {
        if (auto cidr = Cidr::parse(text)) plane.push_back(*cidr);
    }
    if (plane.empty()) plane.push_back(*Cidr::parse("10.64.0.0/10"));
    const Cidr pool = *Cidr::parse(kPrivatePool);
    const auto own = parse_tunnel_address(addr_part(own_tunnel_ip));

    const auto n = peers.size();
    std::vector<std::optional<Cidr>> tunnel(n);
    std::vector<std::optional<Cidr>> subnet(n);
    for (std::size_t i = 0; i < n; ++i) {
        auto addr = parse_tunnel_address(peers[i].tunnel_ip);
        if (addr && (own && addr->network == own->network)) addr.reset();
        if (addr && std::none_of(plane.begin(), plane.end(),
                                 [&](const Cidr& c) { return c.contains(addr->network); })) {
            addr.reset();
        }
        tunnel[i] = addr;
        if (!peers[i].private_subnet.empty()) {
            auto block = parse_subnet(peers[i].private_subnet);
            if (block && (block->prefix_len < kMaxSubnetPrefixLen || !within(*block, pool) ||
                          (own && block->contains(own->network)))) {
                block.reset();
            }
            subnet[i] = block;
        }
    }

    // Every route this device installs must have one owner. A claim that
    // collides with another peer's claim is refused on both sides, so the
    // order the server lists peers in never decides who wins.
    std::vector<bool> tunnel_ok(n, true);
    std::vector<bool> subnet_ok(n, true);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            if (tunnel[i] && tunnel[j] && tunnel[i]->network == tunnel[j]->network) {
                tunnel_ok[i] = tunnel_ok[j] = false;
            }
            if (subnet[i] && subnet[j] && overlaps(*subnet[i], *subnet[j])) {
                subnet_ok[i] = subnet_ok[j] = false;
            }
            if (subnet[i] && tunnel[j] && subnet[i]->contains(tunnel[j]->network)) {
                subnet_ok[i] = false;
            }
            if (subnet[j] && tunnel[i] && subnet[j]->contains(tunnel[i]->network)) {
                subnet_ok[j] = false;
            }
        }
    }

    std::vector<std::string> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (!tunnel[i] || !tunnel_ok[i]) {
            if (!peers[i].tunnel_ip.empty() || !peers[i].private_subnet.empty()) {
                spdlog::warn("[BoringtunMesh] peer {} claims tunnel address '{}' that is not "
                             "its own; no route installed",
                             peers[i].node_id, peers[i].tunnel_ip);
            }
            continue;
        }
        out[i] = addr_part(peers[i].tunnel_ip) + "/32";
        if (peers[i].private_subnet.empty()) continue;
        if (subnet[i] && subnet_ok[i]) {
            out[i] += "," + peers[i].private_subnet;
        } else {
            spdlog::warn("[BoringtunMesh] peer {} private subnet '{}' is outside the private "
                         "pool, too large, or overlaps another address; subnet not routed",
                         peers[i].node_id, peers[i].private_subnet);
        }
    }
    return out;
}

std::string BoringtunMesh::public_key() const {
    return impl_->active.load() ? impl_->started_cfg.public_key : std::string{};
}

std::optional<std::array<uint8_t, 64>> BoringtunMesh::relay_rendezvous(
    const std::string& peer_pubkey, const std::string& relay_id, uint64_t window) const {
    if (!impl_->active.load()) return std::nullopt;
    const auto own_pk = decode_key32(impl_->started_cfg.public_key);
    const auto peer_pk = decode_key32(peer_pubkey);
    auto own_sk = decode_key32(impl_->started_cfg.private_key);
    if (!own_pk || !peer_pk || !own_sk) {
        if (own_sk) sodium_memzero(own_sk->data(), own_sk->size());
        return std::nullopt;
    }
    std::array<uint8_t, 32> shared{};
    const bool ok = crypto_scalarmult(shared.data(), own_sk->data(), peer_pk->data()) == 0;
    sodium_memzero(own_sk->data(), own_sk->size());
    std::optional<nexus::relay::RelayRendezvous> material;
    if (ok) {
        material = nexus::relay::derive_relay_rendezvous(std::span<const uint8_t, 32>(shared),
                                                         *own_pk, *peer_pk, relay_id, window);
    }
    sodium_memzero(shared.data(), shared.size());
    if (!material) return std::nullopt;

    std::array<uint8_t, 64> out{};
    std::copy(material->id.begin(), material->id.end(), out.begin());
    std::copy(material->own_proof.begin(), material->own_proof.end(), out.begin() + 16);
    std::copy(material->peer_commitment.begin(), material->peer_commitment.end(),
              out.begin() + 32);
    sodium_memzero(material->own_proof.data(), material->own_proof.size());
    return out;
}

bool BoringtunMesh::set_peer_relay(const std::string& pubkey, const std::string& local_endpoint,
                                   const std::string& relay_endpoint) {
    if (!impl_->active.load() || pubkey == impl_->server_pubkey) return false;
    {
        std::lock_guard lock(impl_->relay_mtx);
        impl_->relay_paths[pubkey] = Impl::RelayPath{local_endpoint, relay_endpoint};
        impl_->written_endpoints[pubkey] = local_endpoint;
    }
    if (!impl_->dp.update_endpoint(pubkey, local_endpoint)) {
        std::lock_guard lock(impl_->relay_mtx);
        impl_->relay_paths.erase(pubkey);
        impl_->written_endpoints.erase(pubkey);
        return false;
    }
    spdlog::info("[BoringtunMesh] peer {}... now goes through relay {}",
                 pubkey.substr(0, std::min<size_t>(8, pubkey.size())), relay_endpoint);
    return true;
}

void BoringtunMesh::clear_peer_relay(const std::string& pubkey,
                                     const std::string& listed_endpoint) {
    std::lock_guard lock(impl_->relay_mtx);
    impl_->relay_paths.erase(pubkey);
    if (listed_endpoint.empty()) {
        // Forget the shim endpoint, so the next sync_peers writes the listed
        // endpoint again.
        impl_->written_endpoints.erase(pubkey);
    } else {
        impl_->written_endpoints[pubkey] = listed_endpoint;
    }
}

StatusResult BoringtunMesh::remove_peer(const std::string& pubkey) {
    StatusResult result;
    if (!impl_->active.load()) {
        result.error = "mesh dataplane not active";
        return result;
    }
    result.ok = impl_->dp.remove_peer(pubkey);
    return result;
}

MeshTunnelStatus BoringtunMesh::mesh_status() const {
    MeshTunnelStatus ms;
    ms.is_up = impl_->active.load();
    {
        std::lock_guard lock(impl_->ns_mtx);
        ms.tunnel_ip = impl_->tunnel_addr;
    }
    if (!ms.is_up) return ms;

    std::map<std::string, Impl::RelayPath> relayed;
    {
        std::lock_guard lock(impl_->relay_mtx);
        relayed = impl_->relay_paths;
    }

    for (const auto& peer : impl_->dp.snapshot_peers()) {
        if (peer.public_key == impl_->server_pubkey) continue;
        MeshPeer mp;
        mp.mesh_pubkey   = peer.public_key;
        mp.tunnel_ip     = addr_part(peer.allowed_ips);  // first cidr's address
        const auto relay = relayed.find(peer.public_key);
        if (relay != relayed.end() && peer.endpoint == relay->second.local_endpoint) {
            // The dataplane endpoint is the local relay shim; report the relay.
            mp.relay_endpoint = relay->second.relay_endpoint;
        } else {
            mp.endpoint = peer.endpoint;
        }
        mp.last_handshake = static_cast<int64_t>(peer.last_handshake);
        mp.rx_bytes      = peer.rx_bytes;
        mp.tx_bytes      = peer.tx_bytes;
        mp.keepalive     = peer.persistent_keepalive;
        ms.total_rx_bytes += peer.rx_bytes;
        ms.total_tx_bytes += peer.tx_bytes;
        ms.peers.push_back(std::move(mp));
    }
    ms.peer_count = static_cast<uint32_t>(ms.peers.size());
    // online_count is recomputed by MeshOrchestrator (it merges server last_seen).
    return ms;
}

std::pair<std::string, std::string> BoringtunMesh::generate_keypair() {
    unsigned char priv[32];
    unsigned char pub[32];
    randombytes_buf(priv, sizeof priv);
    priv[0]  &= 248;
    priv[31] &= 127;
    priv[31] |= 64;
    crypto_scalarmult_base(pub, priv);

    char priv_b64[sodium_base64_ENCODED_LEN(32, sodium_base64_VARIANT_ORIGINAL)];
    char pub_b64[sodium_base64_ENCODED_LEN(32, sodium_base64_VARIANT_ORIGINAL)];
    sodium_bin2base64(priv_b64, sizeof priv_b64, priv, 32, sodium_base64_VARIANT_ORIGINAL);
    sodium_bin2base64(pub_b64, sizeof pub_b64, pub, 32, sodium_base64_VARIANT_ORIGINAL);

    std::pair<std::string, std::string> keys{priv_b64, pub_b64};
    sodium_memzero(priv, sizeof priv);
    return keys;
}

std::pair<std::string, std::string> BoringtunMesh::derive_keypair(std::span<const uint8_t> seed) {
    unsigned char priv[32];
    unsigned char pub[32];

    // Keyed BLAKE2b domain-separates the mesh scalar from any other use of the
    // same device seed (e.g. ECIES), then apply the standard X25519 clamp.
    static constexpr unsigned char kCtx[crypto_generichash_KEYBYTES] = {
        'l','n','-','m','e','s','h','-','x','2','5','5','1','9'};
    crypto_generichash(priv, sizeof priv, seed.data(), seed.size(), kCtx, sizeof kCtx);
    priv[0]  &= 248;
    priv[31] &= 127;
    priv[31] |= 64;
    crypto_scalarmult_base(pub, priv);

    char priv_b64[sodium_base64_ENCODED_LEN(32, sodium_base64_VARIANT_ORIGINAL)];
    char pub_b64[sodium_base64_ENCODED_LEN(32, sodium_base64_VARIANT_ORIGINAL)];
    sodium_bin2base64(priv_b64, sizeof priv_b64, priv, 32, sodium_base64_VARIANT_ORIGINAL);
    sodium_bin2base64(pub_b64, sizeof pub_b64, pub, 32, sodium_base64_VARIANT_ORIGINAL);

    std::pair<std::string, std::string> keys{priv_b64, pub_b64};
    sodium_memzero(priv, sizeof priv);
    return keys;
}

std::pair<std::string, std::string> BoringtunMesh::identity_bound_keypair(
    std::span<const uint8_t> ed25519_secret_key) {
    unsigned char priv[32];
    unsigned char pub[32];
    if (ed25519_secret_key.size() != crypto_sign_SECRETKEYBYTES ||
        crypto_sign_ed25519_sk_to_curve25519(priv, ed25519_secret_key.data()) != 0) {
        return {};
    }
    crypto_scalarmult_base(pub, priv);

    char priv_b64[sodium_base64_ENCODED_LEN(32, sodium_base64_VARIANT_ORIGINAL)];
    char pub_b64[sodium_base64_ENCODED_LEN(32, sodium_base64_VARIANT_ORIGINAL)];
    sodium_bin2base64(priv_b64, sizeof priv_b64, priv, 32, sodium_base64_VARIANT_ORIGINAL);
    sodium_bin2base64(pub_b64, sizeof pub_b64, pub, 32, sodium_base64_VARIANT_ORIGINAL);

    std::pair<std::string, std::string> keys{priv_b64, pub_b64};
    sodium_memzero(priv, sizeof priv);
    return keys;
}

} // namespace lnsdk
