#include <LemonadeNexusSDK/BoringtunMesh.hpp>
#include <algorithm>

#include <LemonadeNexus/Boringtun/IpRouter.hpp>
#include <LemonadeNexus/Boringtun/UserspaceDataplane.hpp>

#include <virtual_netstack.h>
#include <sodium.h>
#include <spdlog/spdlog.h>

#include <atomic>
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
};

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

    std::unordered_set<std::string> want;
    for (std::size_t i = 0; i < desired.size(); ++i) {
        const auto& p = desired[i];
        if (p.mesh_pubkey.empty()) continue;
        const std::string endpoint = p.endpoint.empty() ? p.relay_endpoint : p.endpoint;
        const std::string& allowed = allowed_list[i];
        if (allowed.empty()) continue;
        want.insert(p.mesh_pubkey);
        (void)impl_->dp.add_peer(p.mesh_pubkey, allowed, endpoint,
                                 static_cast<uint16_t>(p.keepalive ? p.keepalive : 25));
    }

    // Remove peers the server no longer lists (never the server peer itself).
    for (const auto& peer : impl_->dp.snapshot_peers()) {
        if (peer.public_key == impl_->server_pubkey) continue;
        if (!want.count(peer.public_key)) (void)impl_->dp.remove_peer(peer.public_key);
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

    for (const auto& peer : impl_->dp.snapshot_peers()) {
        if (peer.public_key == impl_->server_pubkey) continue;
        MeshPeer mp;
        mp.mesh_pubkey   = peer.public_key;
        mp.tunnel_ip     = addr_part(peer.allowed_ips);  // first cidr's address
        mp.endpoint      = peer.endpoint;
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
