#include <LemonadeNexusSDK/BoringtunMesh.hpp>
#include <algorithm>

#include <LemonadeNexusSDK/Transport/IpRouter.hpp>
#include <LemonadeNexusSDK/Transport/UserspaceDataplane.hpp>

#include <virtual_netstack.h>
#include <sodium.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <map>
#include <mutex>
#include <span>
#include <unordered_set>

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

/// allowed-ips string for a mesh peer: its tunnel IP plus optional private subnet.
std::string allowed_for(const std::string& tunnel_ip, const std::string& private_subnet) {
    std::string out = tunnel_ip;
    if (!private_subnet.empty()) out += out.empty() ? private_subnet : ("," + private_subnet);
    return out;
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
    NsHandle*          ns{nullptr};
    std::string        server_pubkey;     // never removed by sync_peers
    std::string        tunnel_addr;       // our virtual IP (no prefix), egress source
    std::atomic<bool>  active{false};
    BoringtunConfig    started_cfg;       // what the live plane was started with

    std::mutex                                            egress_mtx;
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

    impl_->ns = ns_create(dpcfg.mtu, &mesh_output_trampoline, &impl_->dp);
    if (!impl_->ns) return false;

    NsHandle* ns = impl_->ns;
    impl_->dp.set_inbound_handler([ns](std::span<const uint8_t> pkt) {
        if (!pkt.empty()) ns_feed_inbound(ns, pkt.data(), pkt.size());
    });

    if (!impl_->dp.start(dpcfg)) {
        ns_destroy(impl_->ns);
        impl_->ns = nullptr;
        return false;
    }

    // Register our virtual address; widen the netstack prefix to the mesh plane
    // (from the first allowed-IPs entry) so mesh peers are on-link for egress.
    impl_->tunnel_addr = addr_part(config.tunnel_ip);
    if (!impl_->tunnel_addr.empty()) {
        if (auto c = Cidr::parse(impl_->tunnel_addr); c && c->prefix_len == 32) {
            impl_->dp.add_local_ip(c->network);
        }
        int prefix = 10;
        if (!config.allowed_ips.empty()) {
            prefix = prefix_part(config.allowed_ips.front(), 10);
        }
        ns_add_local_ip(impl_->ns,
                        (impl_->tunnel_addr + "/" + std::to_string(prefix)).c_str());
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
                 impl_->tunnel_addr,
                 impl_->server_pubkey.substr(0, std::min<size_t>(8, impl_->server_pubkey.size())));
    return true;
}

void BoringtunMesh::stop() {
    if (!impl_->active.exchange(false)) return;
    impl_->dp.stop();  // stop feeding the netstack before tearing it down
    if (impl_->ns) {
        ns_destroy(impl_->ns);
        impl_->ns = nullptr;
    }
    std::lock_guard lock(impl_->egress_mtx);
    impl_->egress_cache.clear();
    spdlog::info("[BoringtunMesh] dataplane down");
}

bool BoringtunMesh::is_active() const {
    return impl_->active.load();
}

uint16_t BoringtunMesh::tcp_egress(const std::string& dst_ip, uint16_t dst_port) {
    if (!impl_->active.load() || !impl_->ns || impl_->tunnel_addr.empty()) return 0;
    std::lock_guard lock(impl_->egress_mtx);
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
    std::lock_guard lock(impl_->egress_mtx);
    if (!impl_->ns) return;
    ns_remove_tcp_egress(impl_->ns, dst_ip.c_str(), dst_port);
    impl_->egress_cache.erase({dst_ip, dst_port});
}

bool BoringtunMesh::tcp_ingress(uint16_t vport, const std::string& target) {
    if (!impl_->active.load() || !impl_->ns || impl_->tunnel_addr.empty()) return false;
    if (vport == 0 || target.empty()) return false;
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

    std::unordered_set<std::string> want;
    for (const auto& p : desired) {
        if (p.mesh_pubkey.empty()) continue;
        const std::string endpoint = p.endpoint.empty() ? p.relay_endpoint : p.endpoint;
        const std::string allowed = allowed_for(p.tunnel_ip, p.private_subnet);
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
    ms.is_up     = impl_->active.load();
    ms.tunnel_ip = impl_->tunnel_addr;
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
