#pragma once

/// Permission-based routing for a service this device exposes to mesh peers
/// (LemonadeNexusClient::expose_service).
///
/// The host registers a catalog: the route families of the service and the
/// routes of each family. For a port with a catalog the SDK terminates HTTP
/// at the ingress instead of bridging raw bytes: it attributes each
/// connection to the mesh peer it came from (the inner source address, which
/// the dataplane binds to the sending peer's key), matches the request to a
/// family, and forwards it to the local service only when the peer's
/// permissions hold `admin` or `svc:<family>`. Every refusal is answered
/// with HTTP 403 and reported through one callback. Ports without a catalog
/// keep the raw bridge.

#include <LemonadeNexusSDK/Error.hpp>
#include <LemonadeNexusSDK/Types.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace httplib {
struct Request;
struct Response;
}

namespace lnsdk {

/// One route of a catalog family: the HTTP methods it accepts and either an
/// exact path or a path prefix (exactly one of the two is set).
struct ServiceRoute {
    std::vector<std::string> methods;  ///< Upper-case method tokens, at least one.
    std::string              path;     ///< Exact request path, or "".
    std::string              prefix;   ///< Matches the prefix itself and every path below it, or "".
};

/// A family of routes that one permission (`svc:<name>`) grants.
struct ServiceFamily {
    std::string               name;
    std::vector<ServiceRoute> routes;
};

/// A validated service catalog (service_ingress::parse_catalog).
struct ServiceCatalog {
    uint64_t                   version{0};
    std::string                app_name;
    std::string                app_version;
    std::vector<ServiceFamily> families;
    nlohmann::json             source = nlohmann::json::object();  ///< The catalog as registered.
};

/// One refused ingress request (LemonadeNexusClient::set_ingress_denied_callback).
struct IngressDeniedEvent {
    /// "denied" (the peer lacks the family permission), "unregistered" (no
    /// route of the catalog matches), or "unattributed" (the connection does
    /// not come from a listed mesh peer).
    std::string kind;
    uint16_t    vport{0};
    std::string peer_node_id;     ///< Empty when unattributed.
    /// The key the permissions belong to: the peer's management key when the
    /// peer list carries it, otherwise the peer's mesh public key. Public keys only.
    std::string peer_key;
    std::string peer_tunnel_ip;   ///< The connection's source tunnel address.
    std::string method;
    std::string path;             ///< Request path without the query string. Never a body.
    std::string family;           ///< The matched family, or "".
    std::string required;         ///< "svc:<family>", or "".
    uint64_t    catalog_version{0};
    int64_t     time{0};          ///< Unix seconds.
};

void to_json(nlohmann::json& j, const IngressDeniedEvent& e);

/// Catalog validation and the route policy. Pure functions; thread-safe.
namespace service_ingress {

/// Largest catalog document accepted, in bytes of JSON text.
inline constexpr size_t kCatalogTextLimit = 256u * 1024u;
inline constexpr size_t kMaxFamilies = 64;
inline constexpr size_t kMaxRoutes = 1024;

/// The path the SDK answers itself with the registered catalog, for any
/// attributed peer.
inline constexpr std::string_view kCatalogPath = "/.nexus/catalog";

/// True for a family name that matches [a-z0-9][a-z0-9_.-]{0,62}.
[[nodiscard]] bool valid_family_name(std::string_view name);

/// Parses and validates a catalog:
/// {"version": <uint>, "app": {"name": "...", "version": "..."},
///  "families": [{"name": "inference",
///                "routes": [{"methods": ["POST"], "path": "/api/v1/chat/completions"},
///                           {"methods": ["GET"], "prefix": "/api/v1/models"}]}]}.
/// `version` is required; `app` is optional and its fields default to "".
/// Family names must be valid and unique. A route needs at least one method
/// token and exactly one of `path` or `prefix`, both absolute. The same
/// (method, kind, value) route may appear only once in the catalog. The
/// error names the first rule that fails.
[[nodiscard]] Result<ServiceCatalog> parse_catalog(const nlohmann::json& catalog);

enum class IngressVerdict {
    Allowed,
    Denied,        ///< A route matched; the peer lacks `admin` and `svc:<family>`.
    Unregistered,  ///< No route of the catalog matches method and path.
};

struct IngressDecision {
    IngressVerdict verdict{IngressVerdict::Unregistered};
    std::string    family;    ///< The matched family, or "".
    std::string    required;  ///< "svc:<family>" for a matched route, or "".
};

/// The route policy: the first route, in catalog order, whose method list
/// holds `method` and whose path or prefix matches `path` names the family.
/// The request is allowed when `peer_permissions` contains "admin" or
/// "svc:<family>". `path` is the request path without a query string.
[[nodiscard]] IngressDecision evaluate(const ServiceCatalog& catalog,
                                       const std::vector<std::string>& peer_permissions,
                                       std::string_view method, std::string_view path);

/// The refusal body: {"error", "code", "family", "required", "catalog_version",
/// "app": {"name", "version"}}. `code` is "nexus_permission_denied",
/// "nexus_route_unregistered" or "nexus_peer_unattributed".
[[nodiscard]] nlohmann::json refusal_body(const ServiceCatalog& catalog, std::string_view code,
                                          std::string_view error, const IngressDecision& decision);

}  // namespace service_ingress

/// The ingress front ends of the services with a catalog, one HTTP listener on
/// 127.0.0.1 per virtual port, and the catalogs themselves. Owned by
/// LemonadeNexusClient. All public functions are thread-safe.
class ServiceIngress {
public:
    /// The latest mesh peer list, with each peer's tunnel address and
    /// permissions. Called on a front end thread for every request.
    using PeerSource = std::function<std::vector<MeshPeer>()>;
    using DeniedCallback = std::function<void(const IngressDeniedEvent&)>;

    /// Where the netstack must bridge a virtual port.
    struct BridgeTarget {
        bool        ok{false};
        bool        front_end{false};  ///< True: `target` is a front end; report sources.
        std::string target;            ///< "tcp:127.0.0.1:<port>" of the front end, or the service target.
        std::string error;
    };

    explicit ServiceIngress(PeerSource peers);
    ~ServiceIngress();
    ServiceIngress(const ServiceIngress&) = delete;
    ServiceIngress& operator=(const ServiceIngress&) = delete;

    /// Validates `catalog` (service_ingress::parse_catalog) and makes it the
    /// catalog of `vport`, replacing any previous one. http_status 400 on a
    /// validation error. A running front end applies it to its next request.
    [[nodiscard]] StatusResult set_catalog(uint16_t vport, const nlohmann::json& catalog);

    /// The catalog of `vport` as registered, or {} when none.
    [[nodiscard]] nlohmann::json catalog(uint16_t vport) const;

    /// The bridge target for an exposure of `vport` to `target`. Without a
    /// catalog, `target` itself. With a catalog, the front end of `vport`,
    /// started on first use and kept across netstack restarts; `target` must
    /// be "tcp:<host>:<port>" and becomes where allowed requests go.
    [[nodiscard]] BridgeTarget bridge_target(uint16_t vport, const std::string& target);

    /// The source of a connection the netstack accepted on `vport` and
    /// bridged from loopback port `bridge_port` (BoringtunMesh ingress source
    /// handler). Must arrive before the connection's first request.
    void connection_opened(uint16_t vport, uint16_t bridge_port, const std::string& src_ip,
                           uint16_t src_port);
    void connection_closed(uint16_t vport, uint16_t bridge_port);

    /// Set the refusal callback (empty clears it). It runs on a front end
    /// thread with the request waiting, so it must return promptly; it must
    /// not destroy this object or call this method. When this returns, the
    /// previous callback is not running and is not called again.
    void set_denied_callback(DeniedCallback cb);

    /// Stops every front end and aborts their open requests. Catalogs and
    /// exposure targets stay registered; bridge_target() starts a front end
    /// again.
    void stop();

private:
    struct FrontEnd;
    struct Source {
        std::string ip;
        uint16_t    port{0};
    };
    struct Service {
        std::shared_ptr<const ServiceCatalog>  catalog;
        std::string                            host;   // service target host; "" until exposed
        uint16_t                               port{0};
        std::unique_ptr<FrontEnd>              front_end;
        std::unordered_map<uint16_t, Source>   sources;  // bridge port -> peer source
    };

    void handle(uint16_t vport, const httplib::Request& req, httplib::Response& res);
    void refuse(const ServiceCatalog& catalog, std::string_view code, std::string_view error,
                const service_ingress::IngressDecision& decision, IngressDeniedEvent event,
                httplib::Response& res);

    PeerSource peers_;
    mutable std::mutex mutex_;
    std::unordered_map<uint16_t, Service> services_;
    std::mutex callback_mutex_;
    DeniedCallback callback_;
};

}  // namespace lnsdk
