#include <LemonadeNexusSDK/ServiceIngress.hpp>
#include "MeshProxy.hpp"

#include <httplib.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <set>
#include <thread>

namespace lnsdk {

namespace {

using json = nlohmann::json;

constexpr size_t kMaxMethods = 16;
constexpr size_t kMaxMethodLength = 16;
constexpr size_t kMaxPathLength = 1024;
constexpr size_t kMaxAppTextLength = 128;

std::string upper(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}

std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string strip_prefix_length(std::string address) {
    if (const auto slash = address.find('/'); slash != std::string::npos) address.resize(slash);
    return address;
}

int64_t epoch_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

/// A header value with control characters removed, so a value from the peer
/// list can never end a header line.
std::string header_safe(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const unsigned char c : value) {
        if (c >= 0x20 && c != 0x7f) out.push_back(static_cast<char>(c));
    }
    return out;
}

/// A JSON number that is a non-negative integer (nlohmann stores a literal
/// `3` as a signed integer and a parsed `3` as unsigned; both count).
std::optional<uint64_t> unsigned_integer(const json& value) {
    if (value.is_number_unsigned()) return value.get<uint64_t>();
    if (value.is_number_integer() && value.get<int64_t>() >= 0) return static_cast<uint64_t>(value.get<int64_t>());
    return std::nullopt;
}

/// An absolute path without whitespace or control characters.
bool valid_route_path(std::string_view path) {
    if (path.empty() || path.front() != '/' || path.size() > kMaxPathLength) return false;
    return std::none_of(path.begin(), path.end(), [](unsigned char c) {
        return c <= 0x20 || c == 0x7f || c == '?' || c == '#';
    });
}

/// `path` is `prefix`, or a path below it.
bool prefix_matches(std::string_view prefix, std::string_view path) {
    if (path.size() < prefix.size() || path.compare(0, prefix.size(), prefix) != 0) return false;
    if (path.size() == prefix.size() || prefix.back() == '/') return true;
    return path[prefix.size()] == '/';
}

/// Headers the front end never copies to the local service: hop-by-hop
/// headers, the ones httplib sets for the forwarded request, the connection
/// facts httplib stores in the header map, and every X-Nexus-* header, which
/// only the SDK sets.
bool dropped_header(std::string_view lowered_name) {
    static constexpr std::string_view kDropped[] = {
        "host", "connection", "keep-alive", "transfer-encoding", "content-length", "content-type",
        "upgrade", "proxy-connection", "te", "trailer", "expect",
        "remote_addr", "remote_port", "local_addr", "local_port",
    };
    for (const auto name : kDropped) {
        if (lowered_name == name) return true;
    }
    return lowered_name.rfind("x-nexus-", 0) == 0;
}

/// "tcp:<host>:<port>" -> {host, port}; nullopt for anything else.
std::optional<std::pair<std::string, uint16_t>> parse_tcp_target(const std::string& target) {
    constexpr std::string_view scheme = "tcp:";
    if (target.compare(0, scheme.size(), scheme) != 0) return std::nullopt;
    const auto rest = target.substr(scheme.size());
    const auto colon = rest.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= rest.size()) return std::nullopt;
    auto host = rest.substr(0, colon);
    const auto port_text = rest.substr(colon + 1);
    if (port_text.size() > 5) return std::nullopt;
    uint32_t port = 0;
    for (const char c : port_text) {
        if (c < '0' || c > '9') return std::nullopt;
        port = port * 10 + static_cast<uint32_t>(c - '0');
    }
    if (port == 0 || port > 65535) return std::nullopt;
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    return std::make_pair(host, static_cast<uint16_t>(port));
}

struct Refusal {
    std::string_view kind;
    std::string_view code;
    std::string_view error;
};

constexpr Refusal kDenied{"denied", "nexus_permission_denied", "The peer does not hold the permission of this route"};
constexpr Refusal kUnregistered{"unregistered", "nexus_route_unregistered", "No registered route matches this request"};
constexpr Refusal kUnattributed{"unattributed", "nexus_peer_unattributed", "The connection is not attributed to a mesh peer"};

}  // namespace

void to_json(json& j, const IngressDeniedEvent& e) {
    j = json{
        {"kind",            e.kind},
        {"vport",           e.vport},
        {"peer_node_id",    e.peer_node_id},
        {"peer_key",        e.peer_key},
        {"peer_tunnel_ip",  e.peer_tunnel_ip},
        {"method",          e.method},
        {"path",            e.path},
        {"family",          e.family},
        {"required",        e.required},
        {"catalog_version", e.catalog_version},
        {"time",            e.time},
    };
}

// ---------------------------------------------------------------------------
// Catalog and policy
// ---------------------------------------------------------------------------

namespace service_ingress {

bool valid_family_name(std::string_view name) {
    if (name.empty() || name.size() > 63) return false;
    const auto head = static_cast<unsigned char>(name.front());
    if (!(std::islower(head) || std::isdigit(head))) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::islower(c) || std::isdigit(c) || c == '_' || c == '.' || c == '-';
    });
}

Result<ServiceCatalog> parse_catalog(const json& catalog) {
    Result<ServiceCatalog> result;
    result.http_status = 400;
    const auto fail = [&](std::string error) {
        result.error = std::move(error);
        return result;
    };
    if (!catalog.is_object()) return fail("the catalog must be a JSON object");
    if (catalog.dump().size() > kCatalogTextLimit) return fail("the catalog is larger than 256 KiB");
    const auto version = catalog.contains("version") ? unsigned_integer(catalog["version"]) : std::nullopt;
    if (!version) return fail("the catalog needs an unsigned integer version");
    ServiceCatalog parsed;
    parsed.version = *version;
    if (catalog.contains("app")) {
        const auto& app = catalog["app"];
        if (!app.is_object()) return fail("app must be an object");
        for (const auto* field : {"name", "version"}) {
            if (!app.contains(field)) continue;
            if (!app[field].is_string() || app[field].get_ref<const std::string&>().size() > kMaxAppTextLength) {
                return fail(std::string("app.") + field + " must be a string of at most 128 characters");
            }
        }
        parsed.app_name = app.value("name", "");
        parsed.app_version = app.value("version", "");
    }
    if (!catalog.contains("families") || !catalog["families"].is_array()) {
        return fail("the catalog needs a families array");
    }
    const auto& families = catalog["families"];
    if (families.size() > kMaxFamilies) return fail("the catalog has more than 64 families");
    std::set<std::string> family_names;
    std::set<std::string> routes_seen;
    size_t route_count = 0;
    for (const auto& family : families) {
        if (!family.is_object() || !family.contains("name") || !family["name"].is_string()) {
            return fail("each family needs a name");
        }
        ServiceFamily out;
        out.name = family["name"].get<std::string>();
        if (!valid_family_name(out.name)) {
            return fail("the family name '" + out.name + "' is not [a-z0-9][a-z0-9_.-]{0,62}");
        }
        if (!family_names.insert(out.name).second) return fail("the family " + out.name + " is listed twice");
        if (!family.contains("routes") || !family["routes"].is_array()) {
            return fail("the family " + out.name + " needs a routes array");
        }
        for (const auto& route : family["routes"]) {
            if (++route_count > kMaxRoutes) return fail("the catalog has more than 1024 routes");
            if (!route.is_object()) return fail("each route of " + out.name + " must be an object");
            ServiceRoute parsed_route;
            if (!route.contains("methods") || !route["methods"].is_array() || route["methods"].empty() ||
                route["methods"].size() > kMaxMethods) {
                return fail("a route of " + out.name + " needs a methods array of 1 to 16 entries");
            }
            for (const auto& method : route["methods"]) {
                if (!method.is_string()) return fail("a route of " + out.name + " has a method that is not a string");
                auto token = upper(method.get<std::string>());
                if (token.empty() || token.size() > kMaxMethodLength ||
                    !std::all_of(token.begin(), token.end(), [](unsigned char c) { return c >= 'A' && c <= 'Z'; })) {
                    return fail("a route of " + out.name + " has an invalid method token");
                }
                if (std::find(parsed_route.methods.begin(), parsed_route.methods.end(), token) == parsed_route.methods.end()) {
                    parsed_route.methods.push_back(std::move(token));
                }
            }
            const bool has_path = route.contains("path");
            const bool has_prefix = route.contains("prefix");
            if (has_path == has_prefix) return fail("a route of " + out.name + " needs exactly one of path or prefix");
            const auto& value = has_path ? route["path"] : route["prefix"];
            if (!value.is_string() || !valid_route_path(value.get_ref<const std::string&>())) {
                return fail("a route of " + out.name + " has an invalid " + (has_path ? "path" : "prefix"));
            }
            (has_path ? parsed_route.path : parsed_route.prefix) = value.get<std::string>();
            for (const auto& method : parsed_route.methods) {
                const auto key = method + (has_path ? " path " : " prefix ") + value.get<std::string>();
                if (!routes_seen.insert(key).second) {
                    return fail("the route " + method + " " + value.get<std::string>() + " is listed twice");
                }
            }
            out.routes.push_back(std::move(parsed_route));
        }
        parsed.families.push_back(std::move(out));
    }
    parsed.source = catalog;
    result.ok = true;
    result.http_status = 0;
    result.value = std::move(parsed);
    return result;
}

IngressDecision evaluate(const ServiceCatalog& catalog, const std::vector<std::string>& peer_permissions,
                         std::string_view method, std::string_view path) {
    IngressDecision decision;
    const auto token = upper(method);
    for (const auto& family : catalog.families) {
        for (const auto& route : family.routes) {
            if (std::find(route.methods.begin(), route.methods.end(), token) == route.methods.end()) continue;
            const bool matched = route.path.empty() ? prefix_matches(route.prefix, path) : path == route.path;
            if (!matched) continue;
            decision.family = family.name;
            decision.required = "svc:" + family.name;
            const bool allowed = std::any_of(peer_permissions.begin(), peer_permissions.end(),
                [&](const std::string& permission) { return permission == "admin" || permission == decision.required; });
            decision.verdict = allowed ? IngressVerdict::Allowed : IngressVerdict::Denied;
            return decision;
        }
    }
    return decision;
}

json refusal_body(const ServiceCatalog& catalog, std::string_view code, std::string_view error,
                  const IngressDecision& decision) {
    return json{
        {"error",           std::string(error)},
        {"code",            std::string(code)},
        {"family",          decision.family},
        {"required",        decision.required},
        {"catalog_version", catalog.version},
        {"app",             {{"name", catalog.app_name}, {"version", catalog.app_version}}},
    };
}

}  // namespace service_ingress

// ---------------------------------------------------------------------------
// Front ends
// ---------------------------------------------------------------------------

struct ServiceIngress::FrontEnd {
    std::unique_ptr<httplib::Server>  server;
    std::thread                       thread;
    uint16_t                          port{0};
    std::shared_ptr<MeshPortProxies>  proxies = std::make_shared<MeshPortProxies>();
};

ServiceIngress::ServiceIngress(PeerSource peers) : peers_(std::move(peers)) {}

ServiceIngress::~ServiceIngress() {
    stop();
    // No front end thread runs, so no callback runs either.
}

StatusResult ServiceIngress::set_catalog(uint16_t vport, const json& catalog) {
    StatusResult result;
    if (vport == 0) {
        result.http_status = 400;
        result.error = "vport must not be 0";
        return result;
    }
    auto parsed = service_ingress::parse_catalog(catalog);
    if (!parsed.ok) {
        result.http_status = parsed.http_status;
        result.error = std::move(parsed.error);
        return result;
    }
    std::lock_guard lock(mutex_);
    services_[vport].catalog = std::make_shared<const ServiceCatalog>(std::move(parsed.value));
    result.ok = true;
    return result;
}

json ServiceIngress::catalog(uint16_t vport) const {
    std::lock_guard lock(mutex_);
    const auto it = services_.find(vport);
    if (it == services_.end() || !it->second.catalog) return json::object();
    return it->second.catalog->source;
}

ServiceIngress::BridgeTarget ServiceIngress::bridge_target(uint16_t vport, const std::string& target) {
    BridgeTarget result;
    std::lock_guard lock(mutex_);
    const auto it = services_.find(vport);
    if (it == services_.end() || !it->second.catalog) {
        result.ok = true;
        result.target = target;
        return result;
    }
    auto& service = it->second;
    const auto parsed = parse_tcp_target(target);
    if (!parsed) {
        result.error = "a service with a catalog needs a tcp:<host>:<port> target";
        return result;
    }
    service.host = parsed->first;
    service.port = parsed->second;
    if (!service.front_end) {
        auto front_end = std::make_unique<FrontEnd>();
        front_end->server = std::make_unique<httplib::Server>();
        auto& server = *front_end->server;
        const auto serve = [this, vport](const httplib::Request& req, httplib::Response& res) {
            handle(vport, req, res);
        };
        server.Get(".*", serve);
        server.Post(".*", serve);
        server.Put(".*", serve);
        server.Patch(".*", serve);
        server.Delete(".*", serve);
        server.Options(".*", serve);
        server.set_read_timeout(30, 0);
        server.set_write_timeout(300, 0);
        server.set_keep_alive_timeout(2);
        server.set_payload_max_length(kProxyPayloadLimit);
        const int port = server.bind_to_any_port("127.0.0.1");
        if (port <= 0) {
            result.error = "cannot bind the ingress front end";
            return result;
        }
        front_end->port = static_cast<uint16_t>(port);
        auto* raw = front_end->server.get();
        front_end->thread = std::thread([raw] { raw->listen_after_bind(); });
        raw->wait_until_ready();
        service.front_end = std::move(front_end);
        spdlog::info("[ServiceIngress] vport {} is served through the front end on 127.0.0.1:{}", vport, port);
    }
    result.ok = true;
    result.front_end = true;
    result.target = "tcp:127.0.0.1:" + std::to_string(service.front_end->port);
    return result;
}

void ServiceIngress::connection_opened(uint16_t vport, uint16_t bridge_port, const std::string& src_ip,
                                       uint16_t src_port) {
    if (bridge_port == 0) return;
    std::lock_guard lock(mutex_);
    services_[vport].sources[bridge_port] = Source{src_ip, src_port};
}

void ServiceIngress::connection_closed(uint16_t vport, uint16_t bridge_port) {
    std::lock_guard lock(mutex_);
    const auto it = services_.find(vport);
    if (it != services_.end()) it->second.sources.erase(bridge_port);
}

void ServiceIngress::set_denied_callback(DeniedCallback cb) {
    std::lock_guard lock(callback_mutex_);
    callback_ = std::move(cb);
}

void ServiceIngress::stop() {
    std::vector<std::unique_ptr<FrontEnd>> stopped;
    {
        std::lock_guard lock(mutex_);
        for (auto& [vport, service] : services_) {
            if (service.front_end) stopped.push_back(std::move(service.front_end));
        }
    }
    for (auto& front_end : stopped) {
        front_end->server->stop();
        front_end->proxies->cancel_all();
        if (front_end->thread.joinable()) front_end->thread.join();
    }
}

void ServiceIngress::refuse(const ServiceCatalog& catalog, std::string_view code, std::string_view error,
                            const service_ingress::IngressDecision& decision, IngressDeniedEvent event,
                            httplib::Response& res) {
    res.set_header("Cache-Control", "no-store");
    res.status = 403;
    res.set_content(service_ingress::refusal_body(catalog, code, error, decision).dump(), "application/json");
    event.family = decision.family;
    event.required = decision.required;
    event.catalog_version = catalog.version;
    event.time = epoch_seconds();
    std::lock_guard lock(callback_mutex_);
    if (!callback_) return;
    try {
        callback_(event);
    } catch (const std::exception& e) {
        spdlog::warn("[ServiceIngress] denied callback threw: {}", e.what());
    } catch (...) {
        spdlog::warn("[ServiceIngress] denied callback threw");
    }
}

void ServiceIngress::handle(uint16_t vport, const httplib::Request& req, httplib::Response& res) {
    try {
        std::shared_ptr<const ServiceCatalog> catalog;
        std::shared_ptr<MeshPortProxies> proxies;
        std::string host;
        uint16_t port = 0;
        std::optional<Source> source;
        {
            std::lock_guard lock(mutex_);
            const auto it = services_.find(vport);
            if (it != services_.end()) {
                catalog = it->second.catalog;
                host = it->second.host;
                port = it->second.port;
                if (it->second.front_end) proxies = it->second.front_end->proxies;
                // Only the netstack's bridge sockets are attributed. Any other
                // loopback client of this port has no source.
                if (req.remote_addr == "127.0.0.1") {
                    const auto found = it->second.sources.find(static_cast<uint16_t>(req.remote_port));
                    if (found != it->second.sources.end()) source = found->second;
                }
            }
        }
        if (!catalog || !proxies) {
            reply_error(res, 503, "This service has no catalog");
            return;
        }
        IngressDeniedEvent event;
        event.vport = vport;
        event.method = req.method;
        event.path = req.path;
        service_ingress::IngressDecision decision;

        // Attribution: the source tunnel address names exactly one listed peer.
        std::optional<MeshPeer> peer;
        if (source) {
            event.peer_tunnel_ip = source->ip;
            size_t matches = 0;
            for (auto& candidate : peers_()) {
                if (strip_prefix_length(candidate.tunnel_ip) != source->ip) continue;
                if (++matches == 1) peer = std::move(candidate);
            }
            if (matches != 1) peer.reset();
        }
        if (!peer) {
            event.kind = std::string(kUnattributed.kind);
            refuse(*catalog, kUnattributed.code, kUnattributed.error, decision, std::move(event), res);
            return;
        }
        event.peer_node_id = peer->node_id;
        event.peer_key = peer->mgmt_pubkey.empty() ? peer->mesh_pubkey : peer->mgmt_pubkey;

        if (req.method == "GET" && req.path == service_ingress::kCatalogPath) {
            res.set_header("Cache-Control", "no-store");
            res.set_content(catalog->source.dump(), "application/json");
            return;
        }

        decision = service_ingress::evaluate(*catalog, peer->permissions, req.method, req.path);
        if (decision.verdict == service_ingress::IngressVerdict::Unregistered) {
            event.kind = std::string(kUnregistered.kind);
            refuse(*catalog, kUnregistered.code, kUnregistered.error, decision, std::move(event), res);
            return;
        }
        if (decision.verdict == service_ingress::IngressVerdict::Denied) {
            event.kind = std::string(kDenied.kind);
            refuse(*catalog, kDenied.code, kDenied.error, decision, std::move(event), res);
            return;
        }

        // Allowed: forward with the trusted headers. Client-supplied X-Nexus-*
        // headers were dropped above, so the service can trust these.
        httplib::Headers headers;
        for (const auto& [name, value] : req.headers) {
            if (dropped_header(lower(name))) continue;
            headers.emplace(name, value);
        }
        std::string permissions;
        for (const auto& permission : peer->permissions) {
            permissions += (permissions.empty() ? "" : ",") + header_safe(permission);
        }
        headers.emplace("X-Nexus-Peer-Node", header_safe(peer->node_id));
        headers.emplace("X-Nexus-Peer-Key", header_safe(event.peer_key));
        headers.emplace("X-Nexus-Peer-Permissions", permissions);
        headers.emplace("X-Nexus-Family", decision.family);
        headers.emplace("X-Nexus-Catalog-Version", std::to_string(catalog->version));
        if (req.is_multipart_form_data()) {
            const auto [type, encoded] = multipart_body(req);
            headers.emplace("Content-Type", type);
            proxy_request(proxies, host, port, req, req.target, std::move(headers), encoded, res);
            return;
        }
        if (req.has_header("Content-Type")) headers.emplace("Content-Type", req.get_header_value("Content-Type"));
        proxy_request(proxies, host, port, req, req.target, std::move(headers), req.body, res);
    } catch (const std::exception& e) {
        spdlog::warn("[ServiceIngress] request failed: {}", e.what());
        reply_error(res, 500, "The ingress could not serve this request");
    }
}

}  // namespace lnsdk
