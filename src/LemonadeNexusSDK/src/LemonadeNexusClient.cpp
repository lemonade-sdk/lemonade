#include <LemonadeNexusSDK/LemonadeNexusClient.hpp>
#include <LemonadeNexusSDK/LatencyMonitor.hpp>
#include <LemonadeNexusSDK/MeshOrchestrator.hpp>

#include <httplib.h>
#include <spdlog/spdlog.h>
#include <sodium.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <utility>

// Cross-platform name resolution (getaddrinfo / inet_ntop / sockaddr_in).
// On Windows these live in the Winsock headers; Winsock is already initialized
// process-wide by httplib (included above).
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#endif

namespace lnsdk {

// The one crypto construction this SDK speaks; mirrors
// nexus::crypto::kEncryptedBlobVersion. Version 1 is
// XChaCha20-Poly1305-IETF with a 24-byte nonce.
static constexpr unsigned kEncryptedBlobVersion = 1;

// Byte-identical mirror of nexus::crypto::aead_aad:
//   version || LP(purpose) || LP(context[0]) ...   (LP = u32 LE length + bytes)
// The server builds the same bytes; if these ever diverge the tag fails, which
// is the intended failure mode rather than a silent mismatch.
static std::vector<uint8_t> aead_aad(
        std::string_view purpose,
        std::initializer_list<std::span<const uint8_t>> context) {
    std::vector<uint8_t> aad;
    const auto put = [&aad](const uint8_t* data, std::size_t size) {
        const auto n = static_cast<uint32_t>(size);
        aad.push_back(static_cast<uint8_t>(n & 0xFF));
        aad.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
        aad.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
        aad.push_back(static_cast<uint8_t>((n >> 24) & 0xFF));
        aad.insert(aad.end(), data, data + size);
    };
    aad.push_back(static_cast<uint8_t>(kEncryptedBlobVersion));
    put(reinterpret_cast<const uint8_t*>(purpose.data()), purpose.size());
    for (const auto& piece : context) put(piece.data(), piece.size());
    return aad;
}

using json = nlohmann::json;

namespace {
// Private mesh transport timeouts. These bound the TLS handshake + response
// over the tunnel, so they must be generous enough for a real (possibly lossy)
// tunnel to complete its handshake round-trips — too tight and valid calls fail
// with "SSL connection failed". Responsiveness is handled by the breaker below
// (it stops repeated long waits) and by deferring UI fetches off the transition,
// NOT by starving the handshake.
constexpr int     kMeshConnectTimeoutSec  = 3;
constexpr int     kMeshReadTimeoutSec     = 8;
// After a mesh failure, fail fast (skip the doomed handshake) for this long so a
// persistently-down tunnel doesn't re-freeze every periodic caller.
constexpr int64_t kMeshBreakerCooldownMs  = 8000;

int64_t steady_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Resolve a hostname to its first IPv4 address (dotted string), or "" on
/// failure. Used to learn the server's mesh IP from its private FQDN (which the
/// authoritative DNS maps to the in-mesh address, e.g. 10.64.0.1).
std::string resolve_ipv4(const std::string& host) {
    struct addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return {};
    char buf[INET_ADDRSTRLEN] = {0};
    auto* sin = reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
    inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof buf);
    freeaddrinfo(res);
    return buf;
}
}  // namespace

// ---------------------------------------------------------------------------
// PIMPL implementation
// ---------------------------------------------------------------------------

struct LemonadeNexusClient::Impl {
    ServerConfig config;
    Identity     identity;
    std::string  session_token;
    std::string  node_id;
    std::string  link_token;           // single-use device link token for the next join
    std::vector<std::pair<uint16_t, std::string>> exposed_services;  // vport -> target
    std::string  server_seip_fqdn;     // e.g. "<id>.<region>.seip.lemonade-nexus.io" — public API after join
    std::string  server_private_fqdn;  // e.g. "private.<id>.<region>.seip.lemonade-nexus.io"
    std::string  server_tunnel_ip;    // e.g. "10.64.0.1" — HTTPS fallback
    uint16_t     private_port{9101};
    mutable std::mutex mutex;

    // Server pool with health state
    struct ServerState {
        ServerEndpoint endpoint;
        bool           healthy{true};
        uint32_t       consecutive_failures{0};
        uint64_t       last_health_check_ms{0};
    };
    std::vector<ServerState> server_pool;
    std::size_t              current_server{0};
    bool                     has_discovered{false};

    // Latency-based auto-switching
    std::unique_ptr<LatencyMonitor> latency_monitor;

    // The client's single userspace mesh dataplane (boringtun Noise + smoltcp
    // netstack, no TUN). Brought up at join; carries both mesh P2P traffic and
    // the private-API transport (see private_http_*).
    BoringtunMesh meshplane;
    // The join-assigned dataplane config, kept so callers can inspect it.
    BoringtunConfig boringtun_config;

    // Circuit breaker for the private mesh transport: steady-clock ms until which
    // private calls fail fast without attempting the SSL handshake (0 = closed).
    std::atomic<int64_t> mesh_breaker_until_ms{0};

    // Every private request shares one loopback egress through the netstack, so
    // only one may be in flight: the orchestrator's heartbeat/peer-refresh
    // thread otherwise collides with app calls and both fail the TLS handshake.
    std::mutex private_api_mtx;

    // One keep-alive connection for the private API, reused across requests.
    // A connection per call meant a TLS handshake every time (~570ms) and
    // enough socket churn through the netstack bridge to stall new connects.
    std::unique_ptr<httplib::SSLClient> private_cli;
    uint16_t                            private_cli_port{0};

    httplib::SSLClient* private_client(uint16_t egress_port) {
        if (private_cli && private_cli_port == egress_port) return private_cli.get();
        private_cli = std::make_unique<httplib::SSLClient>(server_private_fqdn, egress_port);
        // The private FQDN lands on the loopback egress; SNI + certificate
        // verification still use the FQDN (against the public CA).
        private_cli->set_hostname_addr_map({{server_private_fqdn, "127.0.0.1"}});
        private_cli->set_connection_timeout(kMeshConnectTimeoutSec);
        private_cli->set_read_timeout(kMeshReadTimeoutSec);
        private_cli->set_keep_alive(true);
        private_cli_port = egress_port;
        return private_cli.get();
    }

    void drop_private_client() {
        private_cli.reset();
        private_cli_port = 0;
    }

    // Mesh P2P orchestrator
    std::unique_ptr<MeshOrchestrator> mesh_orchestrator;
    LemonadeNexusClient::MeshStateCallback mesh_callback;

    explicit Impl(const ServerConfig& cfg) : config{cfg} {
        for (const auto& ep : config.servers) {
            server_pool.push_back({ep, true, 0, 0});
        }
    }

    // Build the base URL for a specific server. Always HTTPS — the mesh has no
    // plaintext path; `host` must be the server's cert FQDN (verified by
    // httplib's default trust-store check).
    static std::string base_url(const ServerEndpoint& ep) {
        return "https://" + ep.host + ":" + std::to_string(ep.port);
    }

    // Get the current server's base URL
    std::string current_base_url() const {
        if (server_pool.empty()) return "https://127.0.0.1:9100";
        return base_url(server_pool[current_server].endpoint);
    }

    // Mark the current server as failed, rotate to next healthy
    void mark_failed() {
        if (server_pool.empty()) return;
        auto& s = server_pool[current_server];
        s.consecutive_failures++;
        if (s.consecutive_failures >= 3) {
            s.healthy = false;
        }
        rotate_server();
    }

    // Move to the next healthy server
    void rotate_server() {
        if (server_pool.size() <= 1) return;
        auto start = current_server;
        for (std::size_t i = 1; i < server_pool.size(); ++i) {
            auto idx = (start + i) % server_pool.size();
            if (server_pool[idx].healthy) {
                current_server = idx;
                spdlog::info("[LemonadeNexusClient] rotated to server {}:{}",
                              server_pool[idx].endpoint.host,
                              server_pool[idx].endpoint.port);
                return;
            }
        }
        // All unhealthy — reset all and try first
        for (auto& s : server_pool) {
            s.healthy = true;
            s.consecutive_failures = 0;
        }
        current_server = 0;
        spdlog::warn("[LemonadeNexusClient] all servers unhealthy, resetting pool");
    }

    // Mark current server as successful
    void mark_success() {
        if (server_pool.empty()) return;
        auto& s = server_pool[current_server];
        s.healthy = true;
        s.consecutive_failures = 0;
    }

    // Build common headers (including Bearer auth when session token is set)
    httplib::Headers auth_headers() const {
        httplib::Headers hdrs;
        if (!session_token.empty()) {
            hdrs.emplace("Authorization", "Bearer " + session_token);
        }
        return hdrs;
    }

    std::optional<json> http_get(const std::string& path, int& status_out) {
        // Try current server, then failover once
        for (int attempt = 0; attempt < 2; ++attempt) {
            try {
                httplib::Client cli(current_base_url());
                cli.set_connection_timeout(config.connect_timeout_sec);
                cli.set_read_timeout(config.read_timeout_sec);
                cli.set_write_timeout(config.write_timeout_sec);

                auto rtt_start = std::chrono::steady_clock::now();
                auto res = cli.Get(path, auth_headers());
                auto rtt_end = std::chrono::steady_clock::now();

                if (!res) {
                    spdlog::warn("[LemonadeNexusClient] GET {} failed: connection error (server {}:{})",
                                  path, server_pool[current_server].endpoint.host,
                                  server_pool[current_server].endpoint.port);
                    mark_failed();
                    if (attempt == 0 && server_pool.size() > 1) continue;
                    status_out = 0;
                    return std::nullopt;
                }

                // Record RTT for latency monitor
                double rtt_ms = std::chrono::duration<double, std::milli>(rtt_end - rtt_start).count();
                if (latency_monitor) {
                    latency_monitor->record_rtt(rtt_ms);
                }

                mark_success();
                status_out = res->status;
                if (res->status < 200 || res->status >= 300) {
                    spdlog::warn("[LemonadeNexusClient] GET {} returned status {}", path, res->status);
                    try {
                        return json::parse(res->body);
                    } catch (...) {
                        return std::nullopt;
                    }
                }
                return json::parse(res->body);
            } catch (const std::exception& e) {
                spdlog::error("[LemonadeNexusClient] GET {} exception: {}", path, e.what());
                mark_failed();
                if (attempt == 0 && server_pool.size() > 1) continue;
                status_out = 0;
                return std::nullopt;
            }
        }
        status_out = 0;
        return std::nullopt;
    }

    std::optional<json> http_post(const std::string& path, const json& body, int& status_out) {
        // Try current server, then failover once
        for (int attempt = 0; attempt < 2; ++attempt) {
            try {
                httplib::Client cli(current_base_url());
                cli.set_connection_timeout(config.connect_timeout_sec);
                cli.set_read_timeout(config.read_timeout_sec);
                cli.set_write_timeout(config.write_timeout_sec);

                auto rtt_start = std::chrono::steady_clock::now();
                auto res = cli.Post(path, auth_headers(), body.dump(), "application/json");
                auto rtt_end = std::chrono::steady_clock::now();

                if (!res) {
                    spdlog::warn("[LemonadeNexusClient] POST {} failed: connection error (server {}:{})",
                                  path, server_pool[current_server].endpoint.host,
                                  server_pool[current_server].endpoint.port);
                    mark_failed();
                    if (attempt == 0 && server_pool.size() > 1) continue;
                    status_out = 0;
                    return std::nullopt;
                }

                // Record RTT for latency monitor
                double rtt_ms = std::chrono::duration<double, std::milli>(rtt_end - rtt_start).count();
                if (latency_monitor) {
                    latency_monitor->record_rtt(rtt_ms);
                }

                mark_success();
                status_out = res->status;
                if (res->status < 200 || res->status >= 300) {
                    spdlog::warn("[LemonadeNexusClient] POST {} returned status {}", path, res->status);
                    try {
                        return json::parse(res->body);
                    } catch (...) {
                        json err;
                        err["error"] = "HTTP " + std::to_string(res->status);
                        return err;
                    }
                }
                return json::parse(res->body);
            } catch (const std::exception& e) {
                spdlog::error("[LemonadeNexusClient] POST {} exception: {}", path, e.what());
                mark_failed();
                if (attempt == 0 && server_pool.size() > 1) continue;
                status_out = 0;
                return std::nullopt;
            }
        }
        status_out = 0;
        return std::nullopt;
    }

    // --- Private API (verified HTTPS over the mesh, by private FQDN) ---

    // Send a private-API request over the userspace mesh dataplane — the only
    // path to the server's private routes (the server terminates the tunnel in
    // userspace + smoltcp, so there is no OS route to its tunnel IP). Egresses a
    // loopback port bridged through the netstack to the server's private API.
    //
    // The private API carries the public cert for server_private_fqdn. We connect
    // BY that FQDN (SNI + certificate verification) while the addr-map lands the
    // socket on the loopback egress the netstack opened — split-horizon DNS:
    // verified public TLS over the mesh, with no by-IP or verification-disabled
    // path.
    //
    // Returns the parsed body if the dataplane served the request (success OR
    // error status); `served` distinguishes "handled" from "not attempted / no
    // response" so the caller can fall through to legacy transports.
    std::optional<json> mesh_request(const char* method, const std::string& path,
                                     const std::string& body, int& status_out,
                                     bool& served, bool use_breaker = true) {
        std::lock_guard egress_lock(private_api_mtx);
        served = false;
        if (!meshplane.is_active() || server_tunnel_ip.empty() || server_private_fqdn.empty()) {
            spdlog::warn("[LemonadeNexusClient] private(mesh) {} {} SKIP "
                         "(active={}, tunnel_ip='{}', fqdn='{}')",
                         method, path, meshplane.is_active(), server_tunnel_ip,
                         server_private_fqdn);
            return std::nullopt;
        }
        // The breaker throttles the periodic callers (heartbeat every 5s, peer
        // refresh) so a down tunnel can't block them repeatedly. Callers acting
        // on a user request skip it: otherwise a failing heartbeat keeps the
        // breaker armed and starves every interactive call.
        int64_t now_ms = steady_now_ms();
        if (use_breaker && now_ms < mesh_breaker_until_ms.load(std::memory_order_relaxed)) {
            spdlog::debug("[LemonadeNexusClient] private(mesh) {} {} fail-fast (breaker open)",
                          method, path);
            return std::nullopt;
        }
        uint16_t lp = meshplane.tcp_egress(server_tunnel_ip, private_port);
        spdlog::debug("[LemonadeNexusClient] private(mesh) {} {} -> egress {}:{} = 127.0.0.1:{}",
                      method, path, server_tunnel_ip, private_port, lp);
        if (lp == 0) {
            mesh_breaker_until_ms.store(now_ms + kMeshBreakerCooldownMs, std::memory_order_relaxed);
            return std::nullopt;
        }

        // The packet that triggers a boringtun handshake is dropped while the
        // session is negotiated, so a cold or expired tunnel loses the first
        // connect. By the time it fails the session is usually live — retry
        // once rather than surfacing a failure the caller can't act on.
        for (int attempt = 0; attempt < 2; ++attempt) {
            try {
                auto* cli = private_client(lp);
                httplib::Result res = (std::string(method) == "GET")
                    ? cli->Get(path, auth_headers())
                    : cli->Post(path, auth_headers(), body, "application/json");
                if (res) {
                    served = true;
                    status_out = res->status;
                    mesh_breaker_until_ms.store(0, std::memory_order_relaxed);  // healthy
                    spdlog::debug("[LemonadeNexusClient] private(mesh) {} {} -> HTTP {}",
                                  method, path, res->status);
                    try { return json::parse(res->body); }
                    catch (...) {
                        if (res->status >= 200 && res->status < 300) return std::nullopt;
                        json err; err["error"] = "HTTP " + std::to_string(res->status);
                        return err;
                    }
                }
                // A pooled connection the server has since closed fails on use;
                // drop it so the retry dials a fresh one.
                drop_private_client();
                if (attempt == 0) {
                    spdlog::debug("[LemonadeNexusClient] private(mesh) {} {} retrying "
                                  "after {}", method, path, httplib::to_string(res.error()));
                    continue;
                }
                spdlog::warn("[LemonadeNexusClient] private(mesh) {} {} NO RESPONSE "
                             "(httplib err={})", method, path, httplib::to_string(res.error()));
            } catch (const std::exception& e) {
                drop_private_client();
                if (attempt == 0) continue;
                spdlog::warn("[LemonadeNexusClient] private(mesh) {} {} EXCEPTION: {}",
                             method, path, e.what());
            }
        }
        if (use_breaker) {
            mesh_breaker_until_ms.store(now_ms + kMeshBreakerCooldownMs,
                                        std::memory_order_relaxed);
        }
        return std::nullopt;
    }

    // Private routes exist ONLY on the server's private API, reachable ONLY over
    // the mesh dataplane — no public or OS-routed fallback (the public API 404s
    // them and the private FQDN has no OS route). Fail fast instead.
    std::optional<json> private_http_get(const std::string& path, int& status_out,
                                         bool use_breaker = true) {
        bool served = false;
        auto r = mesh_request("GET", path, "", status_out, served, use_breaker);
        if (!served) status_out = 0;
        return served ? r : std::nullopt;
    }

    std::optional<json> private_http_post(const std::string& path, const json& body,
                                          int& status_out, bool use_breaker = true) {
        bool served = false;
        auto r = mesh_request("POST", path, body.dump(), status_out, served, use_breaker);
        if (!served) status_out = 0;
        return served ? r : std::nullopt;
    }

    // Discover additional servers via /api/servers
    void discover_servers() {
        if (has_discovered) return;
        int status = 0;
        auto resp = http_get("/api/servers", status);
        if (!resp || !resp->is_array()) return;

        for (const auto& srv : *resp) {
            uint16_t port = srv.value("http_port", uint16_t{9100});

            // The public API is verified HTTPS, so we can only add a discovered
            // server by its certificate FQDN. Skip IP-only entries rather than
            // connect unverified (no plaintext / verify-off path).
            std::string host = srv.value("fqdn", "");
            if (host.empty()) {
                spdlog::debug("[LemonadeNexusClient] skipping discovered server with no "
                              "cert FQDN (endpoint={})", srv.value("endpoint", ""));
                continue;
            }

            // Check if already in pool
            bool exists = false;
            for (const auto& s : server_pool) {
                if (s.endpoint.host == host && s.endpoint.port == port) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                ServerState new_srv;
                new_srv.endpoint = {host, port, true};
                server_pool.push_back(new_srv);
                spdlog::info("[LemonadeNexusClient] discovered server {}:{}", host, port);
            }
        }
        has_discovered = true;
    }

    // Refresh health of all servers in the pool
    void refresh_health() {
        auto now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());

        for (auto& s : server_pool) {
            try {
                httplib::Client cli(base_url(s.endpoint));
                cli.set_connection_timeout(config.connect_timeout_sec);
                cli.set_read_timeout(config.read_timeout_sec);

                auto res = cli.Get("/api/health");
                s.last_health_check_ms = now_ms;
                if (res && res->status == 200) {
                    s.healthy = true;
                    s.consecutive_failures = 0;
                } else {
                    s.consecutive_failures++;
                    if (s.consecutive_failures >= 3) s.healthy = false;
                }
            } catch (...) {
                s.consecutive_failures++;
                if (s.consecutive_failures >= 3) s.healthy = false;
                s.last_health_check_ms = now_ms;
            }
        }
    }

    // Sign a delta with the current identity
    void sign_delta(TreeDelta& delta) {
        if (!identity.is_valid()) {
            spdlog::warn("[LemonadeNexusClient] cannot sign delta: no identity set");
            return;
        }
        delta.signer_pubkey = identity.pubkey_string();
        delta.timestamp = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        auto canonical = canonical_delta_json(delta);
        auto canonical_bytes = std::vector<uint8_t>(canonical.begin(), canonical.end());
        auto sig = identity.sign(canonical_bytes);
        delta.signature = Identity::to_base64(sig);
    }
};

// ---------------------------------------------------------------------------
// Constructor / Destructor / Move
// ---------------------------------------------------------------------------

LemonadeNexusClient::LemonadeNexusClient(const ServerConfig& config)
    : impl_{std::make_unique<Impl>(config)}
{
}

LemonadeNexusClient::~LemonadeNexusClient() = default;

LemonadeNexusClient::LemonadeNexusClient(LemonadeNexusClient&&) noexcept = default;
LemonadeNexusClient& LemonadeNexusClient::operator=(LemonadeNexusClient&&) noexcept = default;

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

void LemonadeNexusClient::set_identity(const Identity& identity) {
    std::lock_guard lock(impl_->mutex);
    impl_->identity = identity;
}

Identity LemonadeNexusClient::identity() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->identity;
}

// ---------------------------------------------------------------------------
// Session state
// ---------------------------------------------------------------------------

void LemonadeNexusClient::set_session_token(const std::string& token) {
    std::lock_guard lock(impl_->mutex);
    impl_->session_token = token;
}

std::string LemonadeNexusClient::session_token() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->session_token;
}

std::string LemonadeNexusClient::node_id() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->node_id;
}

void LemonadeNexusClient::set_node_id(const std::string& id) {
    std::lock_guard lock(impl_->mutex);
    impl_->node_id = id;
}

void LemonadeNexusClient::set_link_token(const std::string& token) {
    std::lock_guard lock(impl_->mutex);
    impl_->link_token = token;
}

Result<nlohmann::json> LemonadeNexusClient::create_link_token(uint32_t ttl_sec) {
    Result<nlohmann::json> result;
    json body = {{"ttl_sec", ttl_sec}};
    int status = 0;
    auto resp = impl_->private_http_post("/api/link/token", body, status);
    result.http_status = status;

    if (!resp) {
        result.error = "link token request failed (server unreachable)";
        return result;
    }
    if (status != 200) {
        result.error = resp->value("error", "link token request failed");
        return result;
    }

    result.ok    = true;
    result.value = *resp;
    return result;
}

StatusResult LemonadeNexusClient::expose_service(uint16_t vport, const std::string& target) {
    StatusResult result;
    if (vport == 0 || target.empty()) {
        result.error = "vport and target required";
        return result;
    }
    if (!impl_->meshplane.is_active()) {
        result.error = "mesh dataplane not active — join first";
        return result;
    }
    if (!impl_->meshplane.tcp_ingress(vport, target)) {
        result.error = "failed to register ingress forward";
        return result;
    }
    {
        std::lock_guard lock(impl_->mutex);
        auto& v = impl_->exposed_services;
        bool known = false;
        for (const auto& e : v) {
            if (e.first == vport && e.second == target) { known = true; break; }
        }
        if (!known) v.emplace_back(vport, target);
    }
    result.ok = true;
    return result;
}

std::vector<std::pair<uint16_t, std::string>> LemonadeNexusClient::exposed_services() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->exposed_services;
}

uint16_t LemonadeNexusClient::open_egress(const std::string& dst_ip, uint16_t dst_port) {
    return impl_->meshplane.tcp_egress(dst_ip, dst_port);
}

void LemonadeNexusClient::close_egress(const std::string& dst_ip, uint16_t dst_port) {
    impl_->meshplane.close_egress(dst_ip, dst_port);
}

// ---------------------------------------------------------------------------
// Health
// ---------------------------------------------------------------------------

Result<HealthStatus> LemonadeNexusClient::check_health() {
    Result<HealthStatus> result;
    int status = 0;
    auto resp = impl_->http_get("/api/health", status);
    result.http_status = status;

    if (!resp) {
        result.error = "server unreachable";
        return result;
    }

    result.ok = true;
    result.value.status          = resp->value("status", "");
    result.value.service         = resp->value("service", "");
    result.value.dns_base_domain = resp->value("dns_base_domain", "");

    // Auto-discover additional servers after first successful health check
    if (result.ok && impl_->config.auto_discover && !impl_->has_discovered) {
        impl_->discover_servers();
    }

    return result;
}

// ---------------------------------------------------------------------------
// Server discovery & health
// ---------------------------------------------------------------------------

void LemonadeNexusClient::discover_servers() {
    std::lock_guard lock(impl_->mutex);
    impl_->has_discovered = false; // reset so discover_servers() actually runs
    impl_->discover_servers();
}

void LemonadeNexusClient::refresh_health() {
    std::lock_guard lock(impl_->mutex);
    impl_->refresh_health();
}

// ---------------------------------------------------------------------------
// Latency-based auto-switching
// ---------------------------------------------------------------------------

void LemonadeNexusClient::enable_auto_switching(const LatencyConfig& config) {
    std::lock_guard lock(impl_->mutex);

    // Stop any existing monitor
    if (impl_->latency_monitor) {
        impl_->latency_monitor->stop();
    }

    impl_->latency_monitor = std::make_unique<LatencyMonitor>(config);

    // Populate with current server pool
    std::vector<ServerConfig> server_configs;
    for (const auto& s : impl_->server_pool) {
        ServerConfig sc;
        sc.servers = {s.endpoint};
        server_configs.push_back(sc);
    }
    impl_->latency_monitor->set_servers(server_configs);

    // Set the current server
    if (!impl_->server_pool.empty()) {
        ServerConfig cur;
        cur.servers = {impl_->server_pool[impl_->current_server].endpoint};
        impl_->latency_monitor->set_current_server(cur);
    }

    // Set the switch callback
    impl_->latency_monitor->set_switch_callback(
        [this](const ServerConfig& new_server) {
            std::lock_guard inner_lock(impl_->mutex);
            if (new_server.servers.empty()) return;

            const auto& ep = new_server.servers[0];

            // Find this endpoint in our server pool and switch to it
            for (std::size_t i = 0; i < impl_->server_pool.size(); ++i) {
                if (impl_->server_pool[i].endpoint.host == ep.host &&
                    impl_->server_pool[i].endpoint.port == ep.port) {
                    impl_->current_server = i;
                    spdlog::info("[LemonadeNexusClient] auto-switched to server {}:{}",
                                  ep.host, ep.port);
                    break;
                }
            }
        });

    impl_->latency_monitor->start();
    spdlog::info("[LemonadeNexusClient] latency-based auto-switching enabled "
                 "(threshold={:.0f}ms, hysteresis={:.0f}%, cooldown={}s)",
                 config.threshold_ms, config.hysteresis * 100.0, config.cooldown_sec);
}

void LemonadeNexusClient::disable_auto_switching() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->latency_monitor) {
        impl_->latency_monitor->stop();
        impl_->latency_monitor.reset();
        spdlog::info("[LemonadeNexusClient] latency-based auto-switching disabled");
    }
}

double LemonadeNexusClient::current_latency_ms() const {
    std::lock_guard lock(impl_->mutex);
    if (impl_->latency_monitor) {
        return impl_->latency_monitor->current_rtt_ms();
    }
    return 0.0;
}

std::vector<ServerLatency> LemonadeNexusClient::server_latencies() const {
    std::lock_guard lock(impl_->mutex);
    if (impl_->latency_monitor) {
        return impl_->latency_monitor->get_stats();
    }
    return {};
}

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------

Result<AuthResponse> LemonadeNexusClient::authenticate_ed25519() {
    Result<AuthResponse> result;

    // Copy identity under lock to avoid racing with other threads
    Identity local_identity;
    {
        std::lock_guard lock(impl_->mutex);
        local_identity = impl_->identity;
    }

    if (!local_identity.is_valid()) {
        result.error = "no identity set — call set_identity() first";
        return result;
    }

    // Phase 1: Request a challenge nonce from the server
    auto pubkey_b64 = Identity::to_base64(
        std::span<const uint8_t>(local_identity.public_key()));

    json challenge_body;
    challenge_body["pubkey"] = pubkey_b64;

    int challenge_status = 0;
    auto challenge_resp = impl_->http_post("/api/auth/challenge", challenge_body, challenge_status);
    if (!challenge_resp) {
        result.error = "challenge request failed";
        result.http_status = challenge_status;
        return result;
    }

    auto challenge_b64 = challenge_resp->value("challenge", std::string{});
    if (challenge_b64.empty()) {
        result.error = "server returned empty challenge";
        result.http_status = challenge_status;
        return result;
    }

    // Phase 2: Sign the challenge with our Ed25519 private key
    auto challenge_bytes = Identity::from_base64(challenge_b64);
    auto signature = local_identity.sign(std::span<const uint8_t>(challenge_bytes));
    auto signature_b64 = Identity::to_base64(std::span<const uint8_t>(signature));

    // Phase 3: Authenticate with the signed challenge
    json auth_body;
    auth_body["method"]    = "ed25519";
    auth_body["pubkey"]    = pubkey_b64;
    auth_body["challenge"] = challenge_b64;
    auth_body["signature"] = signature_b64;

    int status = 0;
    auto resp = impl_->http_post("/api/auth", auth_body, status);
    result.http_status = status;

    if (!resp) {
        result.error = "ed25519 auth request failed";
        return result;
    }

    result.value.authenticated = resp->value("authenticated", false);
    result.value.user_id       = resp->value("user_id", "");
    result.value.session_token = resp->value("session_token", "");
    result.value.error         = resp->value("error", resp->value("error_message", ""));

    if (result.value.authenticated) {
        result.ok = true;
        std::lock_guard lock(impl_->mutex);
        impl_->session_token = result.value.session_token;
    } else {
        result.error = result.value.error;
    }
    return result;
}

Result<AuthResponse> LemonadeNexusClient::authenticate(const std::string& username,
                                                         const std::string& password) {
    Result<AuthResponse> result;
    json body;
    body["method"]   = "password";
    body["username"] = username;
    body["password"] = password;

    if (impl_->identity.is_valid()) {
        body["client_pubkey"] = impl_->identity.pubkey_string();
    }

    int status = 0;
    auto resp = impl_->http_post("/api/auth", body, status);
    result.http_status = status;

    if (!resp) {
        result.error = "authentication request failed";
        return result;
    }

    result.value.authenticated = resp->value("authenticated", false);
    result.value.user_id       = resp->value("user_id", "");
    result.value.session_token = resp->value("session_token", "");
    result.value.error         = resp->value("error", resp->value("error_message", ""));

    if (result.value.authenticated) {
        result.ok = true;
        std::lock_guard lock(impl_->mutex);
        impl_->session_token = result.value.session_token;
    } else {
        result.error = result.value.error;
    }
    return result;
}

Result<AuthResponse> LemonadeNexusClient::authenticate_passkey(const nlohmann::json& passkey_data) {
    Result<AuthResponse> result;
    json body = passkey_data;
    body["method"] = "passkey";

    int status = 0;
    auto resp = impl_->http_post("/api/auth", body, status);
    result.http_status = status;

    if (!resp) {
        result.error = "passkey auth request failed";
        return result;
    }

    result.value.authenticated = resp->value("authenticated", false);
    result.value.user_id       = resp->value("user_id", "");
    result.value.session_token = resp->value("session_token", "");
    result.value.error         = resp->value("error", resp->value("error_message", ""));

    if (result.value.authenticated) {
        result.ok = true;
        std::lock_guard lock(impl_->mutex);
        impl_->session_token = result.value.session_token;
    } else {
        result.error = result.value.error;
    }
    return result;
}

Result<std::string> LemonadeNexusClient::issue_passkey_challenge(const std::string& user_id) {
    Result<std::string> result;

    json body;
    body["type"]    = "passkey";
    body["user_id"] = user_id;

    int status = 0;
    auto resp = impl_->http_post("/api/auth/challenge", body, status);
    result.http_status = status;

    if (!resp) {
        result.error = "challenge request failed";
        return result;
    }

    auto challenge = resp->value("challenge", std::string{});
    if (challenge.empty()) {
        result.error = resp->value("error", "server returned empty challenge");
        return result;
    }

    result.value = std::move(challenge);
    result.ok    = true;
    return result;
}

Result<AuthResponse> LemonadeNexusClient::authenticate_token(const std::string& token) {
    Result<AuthResponse> result;
    json body;
    body["method"] = "token-link";
    body["token"]  = token;

    int status = 0;
    auto resp = impl_->http_post("/api/auth", body, status);
    result.http_status = status;

    if (!resp) {
        result.error = "token auth request failed";
        return result;
    }

    result.value.authenticated = resp->value("authenticated", false);
    result.value.user_id       = resp->value("user_id", "");
    result.value.session_token = resp->value("session_token", "");
    result.value.error         = resp->value("error", resp->value("error_message", ""));

    if (result.value.authenticated) {
        result.ok = true;
        std::lock_guard lock(impl_->mutex);
        impl_->session_token = result.value.session_token;
    } else {
        result.error = result.value.error;
    }
    return result;
}

Result<AuthResponse> LemonadeNexusClient::register_passkey(const PasskeyRegistration& reg) {
    Result<AuthResponse> result;
    json body;
    body["user_id"]         = reg.user_id;
    body["credential_data"] = reg.credential_data;

    int status = 0;
    auto resp = impl_->http_post("/api/auth/register", body, status);
    result.http_status = status;

    if (!resp) {
        result.error = "passkey registration request failed";
        return result;
    }

    result.value.authenticated = resp->value("authenticated", false);
    result.value.user_id       = resp->value("user_id", "");
    result.value.session_token = resp->value("session_token", "");
    result.value.error         = resp->value("error", resp->value("error_message", ""));
    result.ok = result.value.authenticated;
    if (!result.ok) {
        result.error = result.value.error;
    }
    return result;
}

Result<AuthResponse> LemonadeNexusClient::register_passkey_credential(const std::string& user_id,
                                                                        const PasskeyCredential& cred) {
    Result<AuthResponse> result;
    json body;
    body["user_id"]       = user_id;
    body["credential_id"] = cred.credential_id;
    body["public_key_x"]  = cred.public_key_x;
    body["public_key_y"]  = cred.public_key_y;

    int status = 0;
    auto resp = impl_->http_post("/api/auth/register", body, status);
    result.http_status = status;

    if (!resp) {
        result.error = "passkey registration request failed";
        return result;
    }

    result.value.authenticated = resp->value("authenticated", false);
    result.value.user_id       = resp->value("user_id", "");
    result.value.session_token = resp->value("session_token", "");
    result.value.error         = resp->value("error", resp->value("error_message", ""));
    result.ok = result.value.authenticated;
    if (!result.ok) {
        result.error = result.value.error;
    } else {
        std::lock_guard lock(impl_->mutex);
        impl_->session_token = result.value.session_token;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Tree operations
// ---------------------------------------------------------------------------

Result<TreeNode> LemonadeNexusClient::get_tree_node(const std::string& node_id) {
    Result<TreeNode> result;
    int status = 0;
    auto resp = impl_->private_http_get("/api/tree/node/" + node_id, status);
    result.http_status = status;

    if (!resp) {
        result.error = "node not found or server unreachable";
        return result;
    }

    if (status == 404) {
        result.error = "node not found";
        return result;
    }

    try {
        result.value = resp->get<TreeNode>();
        result.ok = true;
    } catch (const std::exception& e) {
        result.error = std::string("failed to parse node: ") + e.what();
    }
    return result;
}

Result<DeltaResult> LemonadeNexusClient::submit_delta(const TreeDelta& delta) {
    Result<DeltaResult> result;

    TreeDelta signed_delta = delta;
    impl_->sign_delta(signed_delta);

    json body = signed_delta;

    int status = 0;
    auto resp = impl_->private_http_post("/api/tree/delta", body, status);
    result.http_status = status;

    if (!resp) {
        result.error = "server unreachable";
        return result;
    }

    result.value.success        = resp->value("success", false);
    result.value.delta_sequence = resp->value("delta_sequence", uint64_t{0});
    result.value.node_id        = resp->value("node_id", "");
    result.value.tunnel_ip      = resp->value("tunnel_ip", "");
    result.value.private_subnet = resp->value("private_subnet", "");
    result.value.error          = resp->value("error", resp->value("error_message", ""));

    result.ok = result.value.success;
    if (!result.ok) {
        result.error = result.value.error;
    }
    return result;
}

Result<DeltaResult> LemonadeNexusClient::create_child_node(const std::string& parent_id,
                                                             const TreeNode& child) {
    Result<DeltaResult> result;

    json body;
    body["parent_id"] = parent_id;
    body["type"]      = node_type_to_string(child.type);
    if (!child.hostname.empty()) body["hostname"] = child.hostname;

    int status = 0;
    auto resp = impl_->private_http_post("/api/tree/node", body, status);
    result.http_status = status;

    if (!resp) {
        result.error = "server unreachable";
        return result;
    }

    result.value.success = resp->value("success", false);
    result.value.node_id = resp->value("node_id", "");
    result.value.error   = resp->value("error", "");
    result.ok = result.value.success;
    if (!result.ok) result.error = result.value.error;
    return result;
}

Result<DeltaResult> LemonadeNexusClient::update_node(const std::string& node_id,
                                                       const nlohmann::json& updates) {
    Result<DeltaResult> result;

    int status = 0;
    auto resp = impl_->private_http_post("/api/tree/node/update/" + node_id, updates, status);
    result.http_status = status;

    if (!resp) {
        result.error = "server unreachable";
        return result;
    }

    result.value.success = resp->value("success", false);
    result.value.node_id = resp->value("node_id", "");
    result.value.error   = resp->value("error", "");
    result.ok = result.value.success;
    if (!result.ok) result.error = result.value.error;
    return result;
}

Result<DeltaResult> LemonadeNexusClient::delete_node(const std::string& node_id) {
    Result<DeltaResult> result;

    int status = 0;
    json body = json::object(); // empty body
    auto resp = impl_->private_http_post("/api/tree/node/delete/" + node_id, body, status);
    result.http_status = status;

    if (!resp) {
        result.error = "server unreachable";
        return result;
    }

    result.value.success = resp->value("success", false);
    result.value.error   = resp->value("error", "");
    result.ok = result.value.success;
    if (!result.ok) result.error = result.value.error;
    return result;
}

Result<std::vector<TreeNode>> LemonadeNexusClient::get_children(const std::string& parent_id) {
    Result<std::vector<TreeNode>> result;
    int status = 0;
    auto resp = impl_->private_http_get("/api/tree/children/" + parent_id, status);
    result.http_status = status;

    if (!resp) {
        result.error = "server unreachable";
        return result;
    }

    try {
        if (resp->is_array()) {
            for (const auto& item : *resp) {
                result.value.push_back(item.get<TreeNode>());
            }
        }
        result.ok = true;
    } catch (const std::exception& e) {
        result.error = std::string("failed to parse children: ") + e.what();
    }
    return result;
}

// ---------------------------------------------------------------------------
// IPAM
// ---------------------------------------------------------------------------

Result<AllocationResponse> LemonadeNexusClient::allocate_ip(const AllocationRequest& req) {
    Result<AllocationResponse> result;

    json body;
    body["node_id"] = req.node_id;
    switch (req.block_type) {
        case BlockType::Tunnel:  body["block_type"] = "tunnel";  break;
        case BlockType::Private: body["block_type"] = "private"; break;
        case BlockType::Shared:  body["block_type"] = "shared";  break;
    }
    if (req.block_type != BlockType::Tunnel) {
        body["prefix_len"] = req.prefix_len;
    }

    int status = 0;
    auto resp = impl_->private_http_post("/api/ipam/allocate", body, status);
    result.http_status = status;

    if (!resp) {
        result.error = "server unreachable";
        return result;
    }

    result.value.success = resp->value("success", false);
    result.value.network = resp->value("network", "");
    result.value.node_id = resp->value("node_id", "");
    result.ok = result.value.success;

    if (!result.ok) {
        result.error = resp->value("error", "allocation failed");
    }
    return result;
}

Result<AllocationResponse> LemonadeNexusClient::allocate_tunnel_ip(const std::string& node_id) {
    AllocationRequest req;
    req.node_id    = node_id;
    req.block_type = BlockType::Tunnel;
    return allocate_ip(req);
}

// ---------------------------------------------------------------------------
// Relay
// ---------------------------------------------------------------------------

Result<std::vector<RelayNodeInfo>> LemonadeNexusClient::list_relays() {
    Result<std::vector<RelayNodeInfo>> result;

    int status = 0;
    auto resp = impl_->private_http_get("/api/relay/list", status);
    result.http_status = status;

    if (!resp) {
        result.error = "server unreachable";
        return result;
    }

    try {
        if (resp->is_array()) {
            for (const auto& r : *resp) {
                RelayNodeInfo info;
                info.relay_id         = r.value("relay_id", "");
                info.endpoint         = r.value("endpoint", "");
                info.region           = r.value("region", "");
                info.reputation_score = r.value("reputation_score", 0.0f);
                info.supports_stun    = r.value("supports_stun", true);
                info.supports_relay   = r.value("supports_relay", true);
                result.value.push_back(std::move(info));
            }
        }
        result.ok = true;
    } catch (const std::exception& e) {
        result.error = std::string("failed to parse relay list: ") + e.what();
    }
    return result;
}

Result<RelayTicket> LemonadeNexusClient::request_relay_ticket(const std::string& peer_id,
                                                                const std::string& relay_id) {
    Result<RelayTicket> result;

    json body;
    body["peer_id"]  = peer_id;
    body["relay_id"] = relay_id;

    int status = 0;
    auto resp = impl_->private_http_post("/api/relay/ticket", body, status);
    result.http_status = status;

    if (!resp) {
        result.error = "server unreachable";
        return result;
    }

    result.value.peer_id       = resp->value("peer_id", "");
    result.value.relay_id      = resp->value("relay_id", "");
    result.value.session_nonce = resp->value("session_nonce", "");
    result.value.issued_at     = resp->value("issued_at", uint64_t{0});
    result.value.expires_at    = resp->value("expires_at", uint64_t{0});
    result.value.signature     = resp->value("signature", "");
    result.ok = true;
    return result;
}

Result<RelayRegisterResult> LemonadeNexusClient::register_relay(const RelayRegistration& reg) {
    Result<RelayRegisterResult> result;

    json body;
    body["relay_id"]        = reg.relay_id;
    body["endpoint"]        = reg.endpoint;
    body["region"]          = reg.region;
    body["public_key"]      = reg.public_key;
    body["capacity_mbps"]   = reg.capacity_mbps;
    body["supports_stun"]   = reg.supports_stun;
    body["supports_relay"]  = reg.supports_relay;

    int status = 0;
    auto resp = impl_->private_http_post("/api/relay/register", body, status);
    result.http_status = status;

    if (!resp) {
        result.error = "server unreachable";
        return result;
    }

    result.value.success  = resp->value("success", false);
    result.value.relay_id = resp->value("relay_id", "");
    result.ok = result.value.success;

    if (!result.ok) {
        result.error = resp->value("error", "registration failed");
    }
    return result;
}

// ---------------------------------------------------------------------------
// Certificates
// ---------------------------------------------------------------------------

Result<CertStatus> LemonadeNexusClient::get_cert_status(const std::string& domain) {
    Result<CertStatus> result;

    int status = 0;
    auto resp = impl_->private_http_get("/api/certs/" + domain, status);
    result.http_status = status;

    if (!resp) {
        result.error = "server unreachable";
        return result;
    }

    if (status == 404) {
        result.value.domain   = domain;
        result.value.has_cert = false;
        result.ok = true;
        return result;
    }

    result.value.domain     = resp->value("domain", domain);
    result.value.has_cert   = resp->value("has_cert", false);
    result.value.expires_at = resp->value("expires_at", uint64_t{0});
    result.ok = true;
    return result;
}

Result<IssuedCertBundle> LemonadeNexusClient::request_certificate(const std::string& hostname) {
    Result<IssuedCertBundle> result;

    // Copy identity under lock to avoid racing with other threads
    Identity local_identity;
    {
        std::lock_guard lock(impl_->mutex);
        local_identity = impl_->identity;
    }

    if (!local_identity.is_valid()) {
        result.error = "identity required for certificate request";
        return result;
    }

    json body;
    body["hostname"]      = hostname;
    body["client_pubkey"] = local_identity.pubkey_string();

    int status = 0;
    auto resp = impl_->http_post("/api/certs/issue", body, status);
    result.http_status = status;

    if (!resp) {
        result.error = "server unreachable";
        return result;
    }

    if (status != 200) {
        result.error = resp->value("error", "certificate request failed");
        return result;
    }

    result.value.domain           = resp->value("domain", "");
    result.value.fullchain_pem    = resp->value("fullchain_pem", "");
    result.value.encrypted_privkey = resp->value("encrypted_privkey", "");
    result.value.crypto_version    = resp->value("crypto_version", 0u);
    result.value.nonce            = resp->value("nonce", "");
    result.value.ephemeral_pubkey = resp->value("ephemeral_pubkey", "");
    result.value.expires_at       = resp->value("expires_at", uint64_t{0});
    result.ok = true;
    return result;
}

Result<DecryptedCert> LemonadeNexusClient::decrypt_certificate(const IssuedCertBundle& bundle) {
    Result<DecryptedCert> result;

    // Copy identity under lock to avoid racing with other threads
    Identity local_identity;
    {
        std::lock_guard lock(impl_->mutex);
        local_identity = impl_->identity;
    }

    if (!local_identity.is_valid()) {
        result.error = "identity required for decryption";
        return result;
    }

    // 1. Decode the server's ephemeral X25519 public key
    auto eph_bytes = Identity::from_base64(bundle.ephemeral_pubkey);
    if (eph_bytes.size() != 32) {
        result.error = "invalid ephemeral_pubkey size";
        return result;
    }

    // 2. Convert our Ed25519 private key to X25519
    // crypto_sign_ed25519_sk_to_curve25519 converts the 64-byte Ed25519 secret
    // to a 32-byte X25519 private key
    uint8_t our_x25519_sk[32];
    if (crypto_sign_ed25519_sk_to_curve25519(our_x25519_sk,
            local_identity.private_key().data()) != 0) {
        result.error = "Ed25519→X25519 conversion failed";
        return result;
    }

    // 3. X25519 DH: shared_secret = DH(our_x_sk, ephemeral_pk)
    uint8_t shared_secret[32];
    if (crypto_scalarmult(shared_secret, our_x25519_sk, eph_bytes.data()) != 0) {
        result.error = "X25519 DH failed";
        return result;
    }

    // 4. HKDF-SHA256 to derive the 32-byte AEAD key
    // Extract: PRK = HMAC-SHA256(salt="", IKM=shared_secret)
    const std::string info_str = "lemonade-nexus-cert-issue";
    uint8_t prk[32];
    uint8_t empty_salt[32] = {};
    crypto_auth_hmacsha256(prk, shared_secret, 32, empty_salt);

    // Expand: OKM = HMAC-SHA256(PRK, info + 0x01)
    uint8_t aes_key[32];
    {
        crypto_auth_hmacsha256_state st;
        crypto_auth_hmacsha256_init(&st, prk, 32);
        crypto_auth_hmacsha256_update(&st,
            reinterpret_cast<const uint8_t*>(info_str.data()), info_str.size());
        uint8_t counter = 0x01;
        crypto_auth_hmacsha256_update(&st, &counter, 1);
        crypto_auth_hmacsha256_final(&st, aes_key);
    }

    // 5. Decrypt. One construction, named by the bundle's version — never
    //    inferred from a field width, and never chosen from this cpu.
    auto nonce_bytes = Identity::from_base64(bundle.nonce);
    auto ct_bytes = Identity::from_base64(bundle.encrypted_privkey);

    const auto fail = [&](const char* why) {
        result.error = why;
        sodium_memzero(our_x25519_sk, 32);
        sodium_memzero(shared_secret, 32);
        sodium_memzero(aes_key, 32);
        return result;
    };

    if (bundle.crypto_version != kEncryptedBlobVersion) {
        return fail("unsupported certificate bundle crypto version");
    }
    // Exact size before the pointer reaches libsodium.
    if (nonce_bytes.size() != crypto_aead_xchacha20poly1305_ietf_NPUBBYTES) {
        return fail("invalid nonce size");
    }
    if (ct_bytes.size() < crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        return fail("invalid ciphertext size");
    }

    std::vector<uint8_t> plaintext(ct_bytes.size());
    unsigned long long plaintext_len = 0;

    // Same binding the server used: this client's identity and this domain.
    const auto our_pk = local_identity.public_key();
    const auto aad = aead_aad(
        "cert-bundle",
        {std::span<const uint8_t>(our_pk.data(), our_pk.size()),
         std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bundle.domain.data()),
                                  bundle.domain.size())});

    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            plaintext.data(), &plaintext_len, nullptr,
            ct_bytes.data(), ct_bytes.size(), aad.data(), aad.size(),
            nonce_bytes.data(), aes_key) != 0) {
        return fail("decryption failed (wrong key or corrupted data)");
    }

    // Clean up sensitive material
    sodium_memzero(our_x25519_sk, 32);
    sodium_memzero(shared_secret, 32);
    sodium_memzero(aes_key, 32);

    result.value.domain        = bundle.domain;
    result.value.fullchain_pem = bundle.fullchain_pem;
    result.value.privkey_pem   = std::string(plaintext.begin(), plaintext.begin() + plaintext_len);
    result.value.expires_at    = bundle.expires_at;
    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// High-level composite operations
// ---------------------------------------------------------------------------

Result<JoinResult> LemonadeNexusClient::join_network(const std::string& username,
                                                       const std::string& password) {
    Result<JoinResult> result;

    // Copy identity and session state under lock to avoid racing with other threads
    Identity local_identity;
    bool needs_auth = false;
    {
        std::lock_guard lock(impl_->mutex);
        local_identity = impl_->identity;
        needs_auth = impl_->session_token.empty();
    }

    if (needs_auth) {
        Result<AuthResponse> auth;
        if (local_identity.is_valid()) {
            auth = authenticate_ed25519();
            if (!auth) {
                spdlog::warn("[LemonadeNexusClient] Ed25519 auth failed ({}), "
                             "falling back to password", auth.error);
                auth = authenticate(username, password);
            }
        } else {
            auth = authenticate(username, password);
        }
        if (!auth) {
            result.error = "authentication failed: " + auth.error;
            result.http_status = auth.http_status;
            return result;
        }
    }

    // Step 2: the mesh keypair is the identity-bound X25519 form of the
    // device identity — stable across launches, and derivable by the server
    // from the authenticated identity alone, which is what proves possession
    // of the static without any extra handshake. A device coming from the
    // legacy hash-derived static simply rotates on this join; the server's
    // rekey path handles that. Fall back to a random key only when there is
    // no identity (e.g. password-only join).
    auto [mesh_private_key, mesh_pubkey] = local_identity.is_valid()
        ? BoringtunMesh::identity_bound_keypair(
              std::span<const uint8_t>(local_identity.private_key()))
        : BoringtunMesh::generate_keypair();

    // Step 3: create endpoint node via the server's composite /api/join endpoint.
    // This endpoint handles node ID generation, parent assignment, IP allocation,
    // and hostname assignment all server-side.
    json join_body;
    if (local_identity.is_valid()) {
        join_body["public_key"] = local_identity.pubkey_string(); // "ed25519:base64..."
    }
    join_body["mesh_pubkey"] = mesh_pubkey;

    // Single-use device link token (see set_link_token / create_link_token)
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->link_token.empty()) {
            join_body["link_token"] = impl_->link_token;
        }
    }

    // Include auth credentials so /api/join can authenticate inline
    if (local_identity.is_valid()) {
        // Get a challenge nonce for Ed25519 auth
        auto pubkey_b64 = Identity::to_base64(
            std::span<const uint8_t>(local_identity.public_key()));

        json challenge_body;
        challenge_body["pubkey"] = pubkey_b64;

        int challenge_status = 0;
        auto challenge_resp = impl_->http_post("/api/auth/challenge",
                                                challenge_body, challenge_status);
        if (challenge_resp) {
            auto challenge_b64 = challenge_resp->value("challenge", std::string{});
            if (!challenge_b64.empty()) {
                auto challenge_bytes = Identity::from_base64(challenge_b64);
                auto signature = local_identity.sign(
                    std::span<const uint8_t>(challenge_bytes));
                auto signature_b64 = Identity::to_base64(
                    std::span<const uint8_t>(signature));

                join_body["method"]    = "ed25519";
                join_body["pubkey"]    = pubkey_b64;
                join_body["challenge"] = challenge_b64;
                join_body["signature"] = signature_b64;
            }
        }
    }

    if (!join_body.contains("method")) {
        // Fallback to password auth in join body
        join_body["method"]   = "password";
        join_body["username"] = username;
        join_body["password"] = password;
    }

    int status = 0;
    auto resp = impl_->http_post("/api/join", join_body, status);
    result.http_status = status;

    if (!resp) {
        result.error = "join request failed (server unreachable)";
        return result;
    }

    if (status != 200) {
        result.error = resp->value("error", resp->value("error_message", "join failed"));
        return result;
    }

    // Store the session token from the join response
    auto token = resp->value("token", std::string{});
    if (!token.empty()) {
        std::lock_guard lock(impl_->mutex);
        impl_->session_token = token;
    }

    auto node_id   = resp->value("node_id", std::string{});
    auto tunnel_ip = resp->value("tunnel_ip", std::string{});

    result.ok = true;
    result.value.success        = true;
    result.value.node_id        = node_id;
    result.value.tunnel_ip      = tunnel_ip;
    result.value.mesh_pubkey    = mesh_pubkey;

    {
        std::lock_guard lock(impl_->mutex);
        impl_->node_id = node_id;
        impl_->link_token.clear();  // consumed server-side on successful join
        // Store server private FQDN for HTTPS over the mesh
        // Store server tunnel IP for HTTP fallback
        auto srv_tunnel = resp->value("server_tunnel_ip", std::string{});
        if (!srv_tunnel.empty()) {
            impl_->server_tunnel_ip = srv_tunnel;
        }

        // Store SEIP FQDN — this is the server's canonical hostname
        auto srv_seip_fqdn = resp->value("server_seip_fqdn", std::string{});
        if (!srv_seip_fqdn.empty() && !impl_->config.pin_server) {
            impl_->server_seip_fqdn = srv_seip_fqdn;
            // Switch public API to use the SEIP hostname (proper cert match)
            auto public_port = static_cast<uint16_t>(impl_->server_pool.empty() ? 9100
                : impl_->server_pool[impl_->current_server].endpoint.port);
            impl_->server_pool.clear();
            impl_->server_pool.push_back({{srv_seip_fqdn, public_port, true}, true, 0, 0});
            impl_->current_server = 0;
            spdlog::info("[LemonadeNexusClient] switched public API to https://{}:{}",
                          srv_seip_fqdn, public_port);
        }

        // Store private FQDN, and resolve it to the server's in-mesh IP so the
        // mesh dataplane can egress to the private API (the FQDN maps to the
        // server's tunnel IP, e.g. 10.64.0.1, via the authoritative DNS).
        auto srv_priv_fqdn = resp->value("server_private_fqdn", std::string{});
        impl_->private_port = static_cast<uint16_t>(
            resp->value("private_api_port", 9101));
        if (!srv_priv_fqdn.empty()) {
            impl_->server_private_fqdn = srv_priv_fqdn;
            if (impl_->server_tunnel_ip.empty()) {
                auto ip = resolve_ipv4(srv_priv_fqdn);
                if (!ip.empty()) {
                    impl_->server_tunnel_ip = ip;
                    spdlog::info("[LemonadeNexusClient] server mesh IP {} (from {})",
                                  ip, srv_priv_fqdn);
                } else {
                    spdlog::warn("[LemonadeNexusClient] could not resolve private FQDN {}",
                                  srv_priv_fqdn);
                }
            }
            spdlog::info("[LemonadeNexusClient] private API: https://{}:{}",
                          srv_priv_fqdn, impl_->private_port);
        } else if (!srv_tunnel.empty()) {
            spdlog::info("[LemonadeNexusClient] private API fallback: https://{}:{}",
                          srv_tunnel, impl_->private_port);
        }
    }

    // Step 4: bring up the userspace mesh dataplane with the assigned config.
    if (!tunnel_ip.empty()) {
        BoringtunConfig bt;
        bt.private_key       = mesh_private_key;
        bt.public_key        = mesh_pubkey;
        bt.tunnel_ip         = tunnel_ip;
        bt.server_public_key = resp->value("mesh_server_pubkey", std::string{});
        bt.server_endpoint   = resp->value("mesh_endpoint", std::string{});
        bt.allowed_ips       = {resp->value("tunnel_subnet", std::string{"10.64.0.0/10"})};

        {
            std::lock_guard lock(impl_->mutex);
            impl_->boringtun_config = bt;
        }
        if (!impl_->meshplane.start(bt)) {
            spdlog::warn("[LemonadeNexusClient] mesh dataplane start failed");
        } else {
            // Fresh dataplane — close the breaker so private calls probe at once,
            // and drop any connection pooled against the old egress.
            impl_->mesh_breaker_until_ms.store(0, std::memory_order_relaxed);
            {
                std::lock_guard lock(impl_->private_api_mtx);
                impl_->drop_private_client();
            }
            // Re-apply service exposures registered before a re-join
            std::vector<std::pair<uint16_t, std::string>> exposures;
            {
                std::lock_guard lock(impl_->mutex);
                exposures = impl_->exposed_services;
            }
            for (const auto& [vport, target] : exposures) {
                if (!impl_->meshplane.tcp_ingress(vport, target)) {
                    spdlog::warn("[LemonadeNexusClient] re-exposing {} -> {} failed",
                                 vport, target);
                }
            }
        }
    }

    spdlog::info("[LemonadeNexusClient] joined network: node_id={}, tunnel_ip={}, mesh_pubkey={}",
                  node_id, tunnel_ip, mesh_pubkey);
    return result;
}

StatusResult LemonadeNexusClient::leave_network() {
    StatusResult result;

    // Tear down the mesh dataplane if active
    if (impl_->meshplane.is_active()) {
        impl_->meshplane.stop();
    }

    std::string current_node_id;
    {
        std::lock_guard lock(impl_->mutex);
        current_node_id = impl_->node_id;
    }

    if (current_node_id.empty()) {
        result.error = "not currently joined (no node ID)";
        return result;
    }

    auto del = delete_node(current_node_id);
    if (!del) {
        result.error = "leave failed: " + del.error;
        result.http_status = del.http_status;
        return result;
    }

    {
        std::lock_guard lock(impl_->mutex);
        impl_->node_id.clear();
    }

    result.ok = true;
    spdlog::info("[LemonadeNexusClient] left network (was node {})", current_node_id);
    return result;
}

// ---------------------------------------------------------------------------
// Group membership management
// ---------------------------------------------------------------------------

Result<DeltaResult> LemonadeNexusClient::add_group_member(const std::string& node_id,
                                                             const GroupMember& member) {
    Result<DeltaResult> result;

    // Step 1: Fetch current node to get existing assignments
    auto node_result = get_tree_node(node_id);
    if (!node_result) {
        result.error = "failed to fetch node: " + node_result.error;
        result.http_status = node_result.http_status;
        return result;
    }

    // Step 2: Append the new member as an assignment
    TreeNode updated = node_result.value;
    Assignment new_assignment;
    new_assignment.management_pubkey = member.management_pubkey;
    new_assignment.permissions       = member.permissions;
    updated.assignments.push_back(new_assignment);

    // Step 3: Submit update_assignment delta
    TreeDelta delta;
    delta.operation      = "update_assignment";
    delta.target_node_id = node_id;
    delta.node_data      = updated;

    return submit_delta(delta);
}

Result<DeltaResult> LemonadeNexusClient::remove_group_member(const std::string& node_id,
                                                                const std::string& pubkey) {
    Result<DeltaResult> result;

    // Step 1: Fetch current node
    auto node_result = get_tree_node(node_id);
    if (!node_result) {
        result.error = "failed to fetch node: " + node_result.error;
        result.http_status = node_result.http_status;
        return result;
    }

    // Step 2: Remove the assignment matching the pubkey
    TreeNode updated = node_result.value;
    auto& assignments = updated.assignments;
    auto it = std::remove_if(assignments.begin(), assignments.end(),
        [&pubkey](const Assignment& a) { return a.management_pubkey == pubkey; });

    if (it == assignments.end()) {
        result.error = "member not found in assignments";
        return result;
    }
    assignments.erase(it, assignments.end());

    // Step 3: Submit update_assignment delta
    TreeDelta delta;
    delta.operation      = "update_assignment";
    delta.target_node_id = node_id;
    delta.node_data      = updated;

    return submit_delta(delta);
}

Result<std::vector<GroupMember>> LemonadeNexusClient::get_group_members(const std::string& node_id) {
    Result<std::vector<GroupMember>> result;

    auto node_result = get_tree_node(node_id);
    if (!node_result) {
        result.error = "failed to fetch node: " + node_result.error;
        result.http_status = node_result.http_status;
        return result;
    }

    for (const auto& a : node_result.value.assignments) {
        GroupMember member;
        member.management_pubkey = a.management_pubkey;
        member.permissions       = a.permissions;
        result.value.push_back(std::move(member));
    }
    result.ok = true;
    return result;
}

Result<GroupJoinResult> LemonadeNexusClient::join_group(const std::string& parent_node_id) {
    Result<GroupJoinResult> result;

    // Step 1: Create endpoint node under the parent group
    TreeNode endpoint_node;
    endpoint_node.type      = NodeType::Endpoint;
    endpoint_node.parent_id = parent_node_id;
    if (impl_->identity.is_valid()) {
        endpoint_node.mgmt_pubkey = impl_->identity.pubkey_string();
    }

    auto create_result = create_child_node(parent_node_id, endpoint_node);
    if (!create_result) {
        result.error = "failed to create endpoint node: " + create_result.error;
        result.http_status = create_result.http_status;
        return result;
    }

    std::string new_node_id = create_result.value.node_id;

    // Step 2: Allocate tunnel IP for the new node
    auto alloc = allocate_tunnel_ip(new_node_id);

    result.ok = true;
    result.value.success        = true;
    result.value.node_id        = new_node_id;
    result.value.parent_node_id = parent_node_id;
    result.value.tunnel_ip      = alloc ? alloc.value.network : "";

    {
        std::lock_guard lock(impl_->mutex);
        impl_->node_id = new_node_id;
    }

    spdlog::info("[LemonadeNexusClient] joined group {}: node_id={}, tunnel_ip={}",
                  parent_node_id, new_node_id, result.value.tunnel_ip);
    return result;
}

// ---------------------------------------------------------------------------
// Stats & server listing
// ---------------------------------------------------------------------------

Result<StatsResponse> LemonadeNexusClient::get_stats() {
    std::lock_guard lock(impl_->mutex);
    int status = 0;
    auto resp = impl_->http_get("/api/stats", status);
    if (!resp) return {false, {}, status, "Connection failed"};
    try {
        return {true, resp->get<StatsResponse>(), status, ""};
    } catch (const std::exception& e) {
        return {false, {}, status, std::string("Parse error: ") + e.what()};
    }
}

Result<std::vector<ServerEntry>> LemonadeNexusClient::get_servers() {
    std::lock_guard lock(impl_->mutex);
    int status = 0;
    auto resp = impl_->http_get("/api/servers", status);
    if (!resp) return {false, {}, status, "Connection failed"};
    try {
        if (!resp->is_array()) return {false, {}, status, "Expected array"};
        return {true, resp->get<std::vector<ServerEntry>>(), status, ""};
    } catch (const std::exception& e) {
        return {false, {}, status, std::string("Parse error: ") + e.what()};
    }
}

// ---------------------------------------------------------------------------
// Private-API transport
// ---------------------------------------------------------------------------

Result<std::string> LemonadeNexusClient::call_private_api(const std::string& method,
                                                         const std::string& path,
                                                         const std::string& body) {
    std::lock_guard lock(impl_->mutex);
    int status = 0;
    std::optional<json> resp;
    // App-initiated, so always attempt: the breaker is for periodic callers.
    constexpr bool kUseBreaker = false;
    if (method == "GET") {
        resp = impl_->private_http_get(path, status, kUseBreaker);
    } else {  // POST covers create/update/delete
        json jbody = json::object();
        if (!body.empty()) { try { jbody = json::parse(body); } catch (...) {} }
        resp = impl_->private_http_post(path, jbody, status, kUseBreaker);
    }
    if (!resp) return {false, {}, status, "Connection failed"};
    return {true, resp->dump(), status, ""};
}

// ---------------------------------------------------------------------------
// DDNS status
// ---------------------------------------------------------------------------

Result<DdnsStatus> LemonadeNexusClient::get_ddns_status() {
    std::lock_guard lock(impl_->mutex);
    int status = 0;
    auto resp = impl_->http_get("/api/ddns/status", status);
    if (!resp) return {false, {}, status, "Connection failed"};
    try {
        return {true, resp->get<DdnsStatus>(), status, ""};
    } catch (const std::exception& e) {
        return {false, {}, status, std::string("Parse error: ") + e.what()};
    }
}

// ---------------------------------------------------------------------------
// Attestation manifests
// ---------------------------------------------------------------------------

Result<AttestationManifests> LemonadeNexusClient::get_attestation_manifests() {
    std::lock_guard lock(impl_->mutex);
    int status = 0;
    auto resp = impl_->http_get("/api/attestation/manifests", status);
    if (!resp) return {false, {}, status, "Connection failed"};
    try {
        return {true, resp->get<AttestationManifests>(), status, ""};
    } catch (const std::exception& e) {
        return {false, {}, status, std::string("Parse error: ") + e.what()};
    }
}

// ---------------------------------------------------------------------------
// Mesh dataplane
// ---------------------------------------------------------------------------

bool LemonadeNexusClient::is_mesh_active() const {
    return impl_->meshplane.is_active();
}

// ---------------------------------------------------------------------------
// Mesh P2P networking
// ---------------------------------------------------------------------------

void LemonadeNexusClient::enable_mesh(const MeshConfig& config) {
    std::string nid;
    {
        std::lock_guard lock(impl_->mutex);
        nid = impl_->node_id;
    }
    if (nid.empty()) {
        spdlog::warn("[LemonadeNexusClient] enable_mesh called but node_id is not set");
        return;
    }

    if (!impl_->meshplane.is_active()) {
        spdlog::warn("[LemonadeNexusClient] enable_mesh: dataplane not up "
                     "(join the network first)");
        return;
    }

    // Stop existing orchestrator if any
    disable_mesh();

    impl_->mesh_orchestrator = std::make_unique<MeshOrchestrator>(
        *this, impl_->meshplane, nid);

    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->mesh_callback) {
            impl_->mesh_orchestrator->set_callback(impl_->mesh_callback);
        }
    }

    impl_->mesh_orchestrator->start(config);
}

void LemonadeNexusClient::disable_mesh() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->mesh_orchestrator) {
        impl_->mesh_orchestrator->stop();
        impl_->mesh_orchestrator.reset();
    }
}

MeshTunnelStatus LemonadeNexusClient::mesh_status() const {
    std::lock_guard lock(impl_->mutex);
    if (impl_->mesh_orchestrator) {
        return impl_->mesh_orchestrator->status();
    }
    return {};
}

std::vector<MeshPeer> LemonadeNexusClient::get_mesh_peers() const {
    std::lock_guard lock(impl_->mutex);
    if (impl_->mesh_orchestrator) {
        return impl_->mesh_orchestrator->peers();
    }
    return {};
}

void LemonadeNexusClient::refresh_mesh_peers() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->mesh_orchestrator) {
        impl_->mesh_orchestrator->refresh_now();
    }
}

void LemonadeNexusClient::set_mesh_callback(MeshStateCallback cb) {
    std::lock_guard lock(impl_->mutex);
    impl_->mesh_callback = std::move(cb);
    if (impl_->mesh_orchestrator) {
        impl_->mesh_orchestrator->set_callback(impl_->mesh_callback);
    }
}

Result<std::vector<MeshPeer>> LemonadeNexusClient::fetch_mesh_peers(const std::string& nid) {
    int status = 0;
    auto resp = impl_->private_http_get("/api/mesh/peers/" + nid, status);
    if (!resp) {
        return {false, {}, status, "mesh peers request failed"};
    }
    if (status != 200) {
        return {false, {}, status, resp->value("error", "mesh peers request failed")};
    }

    try {
        std::vector<MeshPeer> peers;
        if (resp->contains("peers") && (*resp)["peers"].is_array()) {
            for (const auto& p : (*resp)["peers"]) {
                MeshPeer mp;
                mp.node_id        = p.value("node_id", "");
                mp.hostname       = p.value("hostname", "");
                mp.mesh_pubkey    = p.value("mesh_pubkey", "");
                mp.tunnel_ip      = p.value("tunnel_ip", "");
                mp.private_subnet = p.value("private_subnet", "");
                mp.endpoint       = p.value("endpoint", "");
                mp.relay_endpoint = p.value("relay_endpoint", "");
                mp.is_online      = p.value("is_online", false);

                // Validate critical fields — reject peers with shell-unsafe
                // or malformed data before they reach mesh commands.
                // Check for shell metacharacters in all string fields.
                auto has_unsafe_chars = [](const std::string& s) {
                    for (char c : s) {
                        if (c == ';' || c == '|' || c == '&' || c == '$' ||
                            c == '`' || c == '(' || c == ')' || c == '{' ||
                            c == '}' || c == '<' || c == '>' || c == '\'' ||
                            c == '"' || c == '\\' || c == '\n' || c == '\r') {
                            return true;
                        }
                    }
                    return false;
                };
                if (has_unsafe_chars(mp.mesh_pubkey) || has_unsafe_chars(mp.endpoint) ||
                    has_unsafe_chars(mp.tunnel_ip) || has_unsafe_chars(mp.private_subnet) ||
                    has_unsafe_chars(mp.relay_endpoint) || has_unsafe_chars(mp.hostname)) {
                    spdlog::warn("[MeshPeers] Rejected peer '{}' with shell-unsafe characters",
                                  mp.node_id);
                    continue;  // skip this peer entirely
                }
                // Reject peers with empty pubkey (unusable for the mesh)
                if (mp.mesh_pubkey.empty()) {
                    spdlog::debug("[MeshPeers] Skipping peer '{}' with empty mesh_pubkey", mp.node_id);
                    continue;
                }

                peers.push_back(std::move(mp));
            }
        }
        return {true, std::move(peers), status, ""};
    } catch (const std::exception& e) {
        return {false, {}, status, std::string("Parse error: ") + e.what()};
    }
}

StatusResult LemonadeNexusClient::mesh_heartbeat(const std::string& nid,
                                                   const std::string& endpoint) {
    json body;
    body["node_id"]  = nid;
    body["endpoint"] = endpoint;

    int status = 0;
    auto resp = impl_->private_http_post("/api/mesh/heartbeat", body, status);
    if (!resp) {
        return {false, {}, status, "heartbeat request failed"};
    }
    if (status != 200) {
        return {false, {}, status, resp->value("error", "heartbeat failed")};
    }
    return {true, {}, status, ""};
}

// ---------------------------------------------------------------------------
// Routing layer (connect-by-identifier control plane)
// ---------------------------------------------------------------------------

Result<RoutingProfile> LemonadeNexusClient::get_routing_profile(int page, int page_size) {
    Result<RoutingProfile> result;
    int status = 0;
    auto path = "/api/routing/profile?page=" + std::to_string(page) +
                "&page_size=" + std::to_string(page_size);
    auto resp = impl_->private_http_get(path, status);
    result.http_status = status;
    if (!resp) { result.error = "routing profile unreachable"; return result; }
    try {
        result.value = resp->get<RoutingProfile>();
        result.ok = true;
    } catch (const std::exception& e) {
        result.error = std::string("failed to parse profile: ") + e.what();
    }
    return result;
}

Result<ConnectionRequestResult> LemonadeNexusClient::request_endpoint(
    const std::string& identifier, const std::string& conn_nonce_b64,
    const std::string& client_mesh_pubkey, const std::vector<std::string>& candidates) {
    Result<ConnectionRequestResult> result;
    nlohmann::json body = {
        {"identifier", identifier},
        {"conn_nonce", conn_nonce_b64},
        {"client_mesh_pubkey", client_mesh_pubkey},
        {"client_candidates", candidates},
    };
    int status = 0;
    auto resp = impl_->private_http_post("/api/routing/request", body, status);
    result.http_status = status;
    if (!resp) {
        result.error = status == 403 ? "endpoint not found or not authorized"
                                     : "routing request failed";
        return result;
    }
    try {
        result.value = resp->get<ConnectionRequestResult>();
        result.ok = true;
    } catch (const std::exception& e) {
        result.error = std::string("failed to parse request result: ") + e.what();
    }
    return result;
}

Result<ConnectionDirective> LemonadeNexusClient::routing_connect(
    const std::string& connection_id) {
    Result<ConnectionDirective> result;
    nlohmann::json body = {{"connection_id", connection_id}};
    int status = 0;
    auto resp = impl_->private_http_post("/api/routing/connect", body, status);
    result.http_status = status;
    if (!resp) {
        result.error = status == 409 ? "connection not ready" : "connect failed";
        return result;
    }
    try {
        result.value = resp->get<ConnectionDirective>();
        result.ok = true;
    } catch (const std::exception& e) {
        result.error = std::string("failed to parse directive: ") + e.what();
    }
    return result;
}

Result<ConnectionStatus> LemonadeNexusClient::connection_status(
    const std::string& connection_id) {
    Result<ConnectionStatus> result;
    int status = 0;
    auto resp = impl_->private_http_get("/api/routing/session/" + connection_id, status);
    result.http_status = status;
    if (!resp) {
        result.error = status == 404 ? "session not found" : "status unreachable";
        return result;
    }
    try {
        result.value = resp->get<ConnectionStatus>();
        result.ok = true;
    } catch (const std::exception& e) {
        result.error = std::string("failed to parse status: ") + e.what();
    }
    return result;
}

Result<nlohmann::json> LemonadeNexusClient::routing_register_endpoint(
    const std::string& cpu_id, const std::string& net_mac,
    const std::string& mesh_pubkey, const std::string& stun_endpoint) {
    Result<nlohmann::json> result;
    nlohmann::json body = {
        {"cpu_id", cpu_id}, {"net_mac", net_mac},
        {"mesh_pubkey", mesh_pubkey}, {"stun_endpoint", stun_endpoint},
    };
    int status = 0;
    auto resp = impl_->private_http_post("/api/routing/endpoint/register", body, status);
    result.http_status = status;
    if (!resp) { result.error = "endpoint register failed"; return result; }
    result.value = *resp;
    result.ok = true;
    return result;
}

Result<nlohmann::json> LemonadeNexusClient::routing_endpoint_ready(
    const std::string& connection_id, const std::string& cpu_id,
    const std::string& net_mac, const std::string& endpoint_mesh_pubkey,
    const std::vector<std::string>& candidates) {
    Result<nlohmann::json> result;
    nlohmann::json body = {
        {"connection_id", connection_id}, {"cpu_id", cpu_id}, {"net_mac", net_mac},
        {"endpoint_mesh_pubkey", endpoint_mesh_pubkey}, {"endpoint_candidates", candidates},
    };
    int status = 0;
    auto resp = impl_->private_http_post("/api/routing/endpoint/ready", body, status);
    result.http_status = status;
    if (!resp) { result.error = "endpoint ready failed"; return result; }
    result.value = *resp;
    result.ok = true;
    return result;
}

} // namespace lnsdk
