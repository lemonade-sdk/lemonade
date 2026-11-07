#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace lnsdk {

/// One endpoint the caller is authorized to reach, identified by its stable
/// endpoint identifier (not a virtual IP).
struct AuthorizedEndpoint {
    std::string identifier;
    std::string node_id;
    std::string hostname;
    std::string region;
    bool        is_inference{false};
    std::string tunnel_ip;
};

/// Result of GET /api/routing/profile.
struct RoutingProfile {
    std::string node_id;
    std::string pubkey;
    std::string endpoint_identifier;
    std::string hostname;
    std::string parent_id;
    std::vector<AuthorizedEndpoint> authorized_endpoints;
    uint64_t    total{0};
    uint64_t    page{0};
    uint64_t    page_size{0};
};

/// Result of POST /api/routing/request.
struct ConnectionRequestResult {
    std::string connection_id;
    std::string state;
};

/// Result of POST /api/routing/connect: how/where to reach the endpoint.
struct ConnectionDirective {
    std::string connection_id;
    std::string endpoint_identifier;
    std::string endpoint_mesh_pubkey;  ///< E2E Noise static (from peer_binding)
    std::string data_path;             ///< "direct" | "relay"
    std::string relay_endpoint;
    std::string conn_nonce_b64;
    uint64_t    punch_at{0};
    bool        ticket_signed{false};
    std::vector<std::string> endpoint_candidates;
};

/// Result of GET /api/routing/session/{id}.
struct ConnectionStatus {
    std::string connection_id;
    std::string phase;
    std::string data_path;
    uint64_t    created_at{0};
    uint64_t    expires_at{0};
};

inline void from_json(const nlohmann::json& j, AuthorizedEndpoint& e) {
    e.identifier   = j.value("identifier", "");
    e.node_id      = j.value("node_id", "");
    e.hostname     = j.value("hostname", "");
    e.region       = j.value("region", "");
    e.is_inference = j.value("is_inference", false);
    e.tunnel_ip    = j.value("tunnel_ip", "");
}

inline void from_json(const nlohmann::json& j, RoutingProfile& p) {
    if (auto it = j.find("identity"); it != j.end()) {
        p.node_id             = it->value("node_id", "");
        p.pubkey              = it->value("pubkey", "");
        p.endpoint_identifier = it->value("endpoint_identifier", "");
        p.hostname            = it->value("hostname", "");
    }
    if (auto it = j.find("access_chain"); it != j.end())
        p.parent_id = it->value("parent_id", "");
    if (auto it = j.find("authorized_endpoints"); it != j.end() && it->is_array())
        p.authorized_endpoints = it->get<std::vector<AuthorizedEndpoint>>();
    p.total     = j.value("total", uint64_t{0});
    p.page      = j.value("page", uint64_t{0});
    p.page_size = j.value("page_size", uint64_t{0});
}

inline void from_json(const nlohmann::json& j, ConnectionRequestResult& r) {
    r.connection_id = j.value("connection_id", "");
    r.state         = j.value("state", "");
}

inline void from_json(const nlohmann::json& j, ConnectionDirective& d) {
    d.connection_id       = j.value("connection_id", "");
    d.data_path           = j.value("data_path", "");
    d.relay_endpoint      = j.value("relay_endpoint", "");
    d.conn_nonce_b64      = j.value("conn_nonce", "");
    d.punch_at            = j.value("punch_at", uint64_t{0});
    if (auto it = j.find("peer_binding"); it != j.end()) {
        d.endpoint_identifier = it->value("identifier", "");
        d.endpoint_mesh_pubkey = it->value("mesh_pubkey", "");
    }
    if (auto it = j.find("ticket"); it != j.end())
        d.ticket_signed = it->value("signed", false);
    if (auto it = j.find("endpoint_candidates"); it != j.end() && it->is_array()) {
        for (const auto& c : *it)
            if (c.contains("endpoint")) d.endpoint_candidates.push_back(c.value("endpoint", ""));
    }
}

inline void from_json(const nlohmann::json& j, ConnectionStatus& s) {
    s.connection_id = j.value("connection_id", "");
    s.phase         = j.value("phase", "");
    s.data_path     = j.value("data_path", "");
    s.created_at    = j.value("created_at", uint64_t{0});
    s.expires_at    = j.value("expires_at", uint64_t{0});
}

} // namespace lnsdk
