#include <LemonadeNexusSDK/Types.hpp>

#include <stdexcept>

namespace lnsdk {

using json = nlohmann::json;

// --- NodeType <-> string ---

std::string node_type_to_string(NodeType type) {
    switch (type) {
        case NodeType::Root:     return "root";
        case NodeType::Customer: return "customer";
        case NodeType::Endpoint: return "endpoint";
        case NodeType::Relay:    return "relay";
    }
    throw std::invalid_argument("Unknown NodeType value: " +
                                 std::to_string(static_cast<uint8_t>(type)));
}

NodeType string_to_node_type(std::string_view s) {
    if (s == "root")     return NodeType::Root;
    if (s == "customer") return NodeType::Customer;
    if (s == "endpoint") return NodeType::Endpoint;
    if (s == "relay")    return NodeType::Relay;
    throw std::invalid_argument("Unknown NodeType string: " + std::string(s));
}

// --- Assignment ---

void to_json(json& j, const Assignment& a) {
    j = json{
        {"management_pubkey", a.management_pubkey},
        {"permissions",       a.permissions},
    };
}

void from_json(const json& j, Assignment& a) {
    j.at("management_pubkey").get_to(a.management_pubkey);
    j.at("permissions").get_to(a.permissions);
}

// --- TreeNode ---
// Field names and ordering identical to server's TreeTypes.cpp

void to_json(json& j, const TreeNode& n) {
    j = json{
        {"id",                       n.id},
        {"parent_id",                n.parent_id},
        {"type",                     node_type_to_string(n.type)},
        {"hostname",                 n.hostname},
        {"tunnel_ip",                n.tunnel_ip},
        {"private_subnet",           n.private_subnet},
        {"private_shared_addresses", n.private_shared_addresses},
        {"shared_domain",            n.shared_domain},
        {"mgmt_pubkey",              n.mgmt_pubkey},
        {"wrapped_mgmt_privkey",     n.wrapped_mgmt_privkey},
        {"mesh_pubkey",              n.mesh_pubkey},
        {"assignments",              n.assignments},
        {"signature",                n.signature},
        {"listen_endpoint",          n.listen_endpoint},
        {"region",                   n.region},
        {"capacity_mbps",            n.capacity_mbps},
        {"reputation_score",         n.reputation_score},
        {"expires_at",               n.expires_at},
    };
}

void from_json(const json& j, TreeNode& n) {
    if (j.contains("wg_pubkey")) {
        throw std::invalid_argument("legacy 'wg_pubkey' label is not accepted; use 'mesh_pubkey'");
    }
    n.id                       = j.value("id", "");
    n.parent_id                = j.value("parent_id", "");
    if (j.contains("type") && j["type"].is_string()) {
        n.type = string_to_node_type(j["type"].get<std::string>());
    }
    n.hostname                 = j.value("hostname", "");
    n.tunnel_ip                = j.value("tunnel_ip", "");
    n.private_subnet           = j.value("private_subnet", "");
    n.private_shared_addresses = j.value("private_shared_addresses", "");
    n.shared_domain            = j.value("shared_domain", "");
    n.mgmt_pubkey              = j.value("mgmt_pubkey", "");
    n.wrapped_mgmt_privkey     = j.value("wrapped_mgmt_privkey", "");
    n.mesh_pubkey              = j.value("mesh_pubkey", "");
    if (j.contains("assignments") && j["assignments"].is_array()) {
        j["assignments"].get_to(n.assignments);
    }
    n.signature                = j.value("signature", "");
    n.listen_endpoint          = j.value("listen_endpoint", "");
    n.region                   = j.value("region", "");
    n.capacity_mbps            = j.value("capacity_mbps", uint32_t{0});
    n.reputation_score         = j.value("reputation_score", 0.0f);
    n.expires_at               = j.value("expires_at", uint64_t{0});
}

// --- TreeDelta ---

void to_json(json& j, const TreeDelta& d) {
    j = json{
        {"operation",      d.operation},
        {"target_node_id", d.target_node_id},
        {"node_data",      d.node_data},
        {"signer_pubkey",  d.signer_pubkey},
        {"signature",      d.signature},
        {"timestamp",      d.timestamp},
    };
}

void from_json(const json& j, TreeDelta& d) {
    if (j.contains("node_data") && j["node_data"].is_object() &&
        j["node_data"].contains("wg_pubkey")) {
        throw std::invalid_argument("legacy 'wg_pubkey' label is not accepted; use 'mesh_pubkey'");
    }
    j.at("operation").get_to(d.operation);
    j.at("target_node_id").get_to(d.target_node_id);
    j.at("node_data").get_to(d.node_data);
    d.signer_pubkey = j.value("signer_pubkey", "");
    d.signature     = j.value("signature", "");
    d.timestamp     = j.value("timestamp", uint64_t{0});
}

// --- Canonical JSON for signing ---
// Mirrors server's canonical_delta_json() exactly:
// sorted keys (nlohmann::json default), excludes "signature" field.

std::string canonical_delta_json(const TreeDelta& delta) {
    json j;
    // Mirrors the server: node_data is signed as-is under "mesh_pubkey".
    j["node_data"]      = delta.node_data;
    j["operation"]      = delta.operation;
    j["signer_pubkey"]  = delta.signer_pubkey;
    j["target_node_id"] = delta.target_node_id;
    j["timestamp"]      = delta.timestamp;
    // "signature" intentionally excluded for canonical form
    return j.dump();
}

// --- StatsResponse ---
void from_json(const json& j, StatsResponse& s) {
    s.service             = j.value("service", "");
    s.peer_count          = j.value("peer_count", uint32_t{0});
    s.private_api_enabled = j.value("private_api_enabled", false);
}

// --- ServerEntry ---
void from_json(const json& j, ServerEntry& s) {
    s.endpoint  = j.value("endpoint", "");
    s.pubkey    = j.value("pubkey", "");
    s.http_port = j.value("http_port", uint16_t{0});
    s.last_seen = j.value("last_seen", uint64_t{0});
    s.healthy   = j.value("healthy", false);
}

// --- DdnsStatus ---
void from_json(const json& j, DdnsStatus& s) {
    s.has_credentials = j.value("has_credentials", false);
    s.last_ip         = j.value("last_ip", "");
    s.binary_hash     = j.value("binary_hash", "");
    s.binary_approved = j.value("binary_approved", false);
}

// --- AttestationManifest ---
void from_json(const json& j, AttestationManifest& m) {
    m.version       = j.value("version", "");
    m.platform      = j.value("platform", "");
    m.binary_sha256 = j.value("binary_sha256", "");
    m.timestamp     = j.value("timestamp", uint64_t{0});
}

// --- AttestationManifests ---
void from_json(const json& j, AttestationManifests& a) {
    a.self_hash      = j.value("self_hash", "");
    a.self_approved  = j.value("self_approved", false);
    a.github_url     = j.value("github_url", "");
    a.minimum_version = j.value("minimum_version", "");
    a.manifest_fetch_interval_sec = j.value("manifest_fetch_interval_sec", uint32_t{0});
    if (j.contains("manifests") && j["manifests"].is_array()) {
        a.manifests = j["manifests"].get<std::vector<AttestationManifest>>();
    }
}

// --- MeshPeer ---
void to_json(json& j, const MeshPeer& p) {
    j = json{
        {"node_id",        p.node_id},
        {"hostname",       p.hostname},
        {"mesh_pubkey",    p.mesh_pubkey},
        {"tunnel_ip",      p.tunnel_ip},
        {"private_subnet", p.private_subnet},
        {"endpoint",       p.endpoint},
        {"relay_endpoint", p.relay_endpoint},
        {"is_online",      p.is_online},
        {"last_handshake", p.last_handshake},
        {"rx_bytes",       p.rx_bytes},
        {"tx_bytes",       p.tx_bytes},
        {"latency_ms",     p.latency_ms},
        {"keepalive",      p.keepalive},
        {"last_seen",      p.last_seen},
    };
}

void from_json(const json& j, MeshPeer& p) {
    p.node_id        = j.value("node_id", "");
    p.hostname       = j.value("hostname", "");
    p.mesh_pubkey    = j.value("mesh_pubkey", "");
    p.tunnel_ip      = j.value("tunnel_ip", "");
    p.private_subnet = j.value("private_subnet", "");
    p.endpoint       = j.value("endpoint", "");
    p.relay_endpoint = j.value("relay_endpoint", "");
    p.is_online      = j.value("is_online", false);
    p.last_handshake = j.value("last_handshake", int64_t{0});
    p.rx_bytes       = j.value("rx_bytes", uint64_t{0});
    p.tx_bytes       = j.value("tx_bytes", uint64_t{0});
    p.latency_ms     = j.value("latency_ms", int32_t{-1});
    p.keepalive      = j.value("keepalive", uint16_t{5});
    p.last_seen      = j.value("last_seen", uint64_t{0});
}

// --- MeshTunnelStatus ---
void to_json(json& j, const MeshTunnelStatus& s) {
    j = json{
        {"is_up",          s.is_up},
        {"tunnel_ip",      s.tunnel_ip},
        {"peer_count",     s.peer_count},
        {"online_count",   s.online_count},
        {"total_rx_bytes", s.total_rx_bytes},
        {"total_tx_bytes", s.total_tx_bytes},
        {"peers",          s.peers},
    };
}

void from_json(const json& j, MeshTunnelStatus& s) {
    s.is_up          = j.value("is_up", false);
    s.tunnel_ip      = j.value("tunnel_ip", "");
    s.peer_count     = j.value("peer_count", uint32_t{0});
    s.online_count   = j.value("online_count", uint32_t{0});
    s.total_rx_bytes = j.value("total_rx_bytes", uint64_t{0});
    s.total_tx_bytes = j.value("total_tx_bytes", uint64_t{0});
    if (j.contains("peers") && j["peers"].is_array()) {
        s.peers = j["peers"].get<std::vector<MeshPeer>>();
    }
}

// --- MeshConfig ---
void to_json(json& j, const MeshConfig& c) {
    j = json{
        {"peer_refresh_interval_sec", c.peer_refresh_interval_sec},
        {"heartbeat_interval_sec",    c.heartbeat_interval_sec},
        {"stun_refresh_interval_sec", c.stun_refresh_interval_sec},
        {"prefer_direct",             c.prefer_direct},
        {"auto_connect",              c.auto_connect},
    };
}

void from_json(const json& j, MeshConfig& c) {
    c.peer_refresh_interval_sec = j.value("peer_refresh_interval_sec", uint32_t{30});
    c.heartbeat_interval_sec    = j.value("heartbeat_interval_sec", uint32_t{15});
    c.stun_refresh_interval_sec = j.value("stun_refresh_interval_sec", uint32_t{60});
    c.prefer_direct             = j.value("prefer_direct", true);
    c.auto_connect              = j.value("auto_connect", true);
}

} // namespace lnsdk
