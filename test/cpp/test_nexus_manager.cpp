#include "lemon/nexus_manager.h"
#include "lemon/nexus_user_key.h"
#include "lemon/runtime_config.h"
#include "lemon/server/request_middleware.h"
#include "lemon/server/route_registry.h"
#include "lemon/utils/origin_utils.h"
#include "lemon/utils/path_utils.h"
#include "lemon/version.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <thread>
#ifdef LEMONADE_WITH_NEXUS
#include <LemonadeNexusSDK/lemonade_nexus.h>
#endif

namespace lemon {
// Each route folder's registration function, as server.cpp declares them: the test
// builds the real registry so the catalog check covers every route lemond serves.
void register_openai_text_routes(RouteRegistry& registry, ServerContext& ctx);
void register_openai_audio_routes(RouteRegistry& registry, ServerContext& ctx);
void register_openai_images_routes(RouteRegistry& registry, ServerContext& ctx);
void register_lemonade_models_routes(RouteRegistry& registry, ServerContext& ctx);
void register_openai_models_routes(RouteRegistry& registry, ServerContext& ctx);
void register_lemonade_load_routes(RouteRegistry& registry, ServerContext& ctx);
void register_lemonade_downloads_routes(RouteRegistry& registry, ServerContext& ctx);
void register_lemonade_inference_routes(RouteRegistry& registry, ServerContext& ctx);
void register_lemonade_backends_routes(RouteRegistry& registry, ServerContext& ctx);
void register_lemonade_system_routes(RouteRegistry& registry, ServerContext& ctx);
void register_lemonade_jobs_routes(RouteRegistry& registry, ServerContext& ctx);
void register_llamacpp_routes(RouteRegistry& registry, ServerContext& ctx);
void register_router_routes(RouteRegistry& registry, ServerContext& ctx);
void register_internal_routes(RouteRegistry& registry, ServerContext& ctx);
void register_ollama_routes(RouteRegistry& registry, ServerContext& ctx);
void register_anthropic_routes(RouteRegistry& registry, ServerContext& ctx);
void register_mcp_routes(RouteRegistry& registry, ServerContext& ctx);
}

namespace {
using nlohmann::json;
namespace access = lemon::nexus_access;

// Every manager in this test runs as a system service with its own identity file under
// the temporary root, whatever the host OS: no test reaches an OS keystore. The role is
// injected through the constructor; the rule itself is tested in check_identity_rules.
const lemon::ProcessRole kSystemRole{lemon::ProcessRole::Os::Linux, false, true};
const lemon::ProcessRole kUserRole{lemon::ProcessRole::Os::Linux, false, false};

bool throws(const std::function<void()>& action) {
    try { action(); } catch (const std::runtime_error&) { return true; }
    return false;
}

bool throws_logic(const std::function<void()>& action) {
    try { action(); } catch (const std::logic_error&) { return true; }
    return false;
}

// Stands in for Server::route_request_policy: the shared origin check and the API key.
void listener_policy(httplib::Server& server) {
    server.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        if (!lemon::utils::is_origin_allowed(req.get_header_value("Origin"), "", req.get_header_value("Host"))) {
            res.status = 403;
            return httplib::Server::HandlerResponse::Handled;
        }
        const auto key = lemon::utils::get_environment_variable_utf8("LEMONADE_API_KEY");
        if (!key.empty() && req.method != "OPTIONS" &&
            !access::bearer_matches(req.get_header_value("Authorization"), key, lemon::utils::get_environment_variable_utf8("LEMONADE_ADMIN_API_KEY"))) {
            res.status = 401;
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
}

void set_environment(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) unsetenv(name); else setenv(name, value.c_str(), 1);
#endif
}

void check_route_access(const std::function<void(bool, const char*)>& check) {
    using lemon::NexusListener;
    const auto route = [](NexusListener listener, const std::string& address, const std::string& operation,
                          const httplib::Headers& headers = httplib::Headers{}) {
        httplib::Request req;
        req.remote_addr = address;
        req.headers = headers;
        return access::route_access(listener, req, operation);
    };
    check(access::constant_time_equals("secret", "secret"), "Equal secrets match");
    check(!access::constant_time_equals("secre", "secret") && !access::constant_time_equals("secrets", "secret"), "Length differences never match");
    check(!access::bearer_matches("Bearer ", "", ""), "Empty API keys are not credentials");
    for (const auto* address : {"192.168.1.2", "10.1.2.3", "203.0.113.1"}) {
        for (const auto* op : {"status", "members", "enable", "relay", "key/handoff", "account/delete", "route-permissions"}) {
            check(route(NexusListener::Public, address, op, {{"Authorization", "Bearer admin"}}).first == 403,
                  "Neither a LAN address nor an API key authorizes remote Nexus requests");
        }
    }
    check(route(NexusListener::Public, "127.0.0.1", "status").first == 0, "Local discovery is key-free");
    check(route(NexusListener::Public, "127.0.0.1", "members").first == 307, "Local management retains its redirect");
    check(route(NexusListener::Management, "127.0.0.1", "enable").first == 0, "Local bootstrap is key-free");
    check(route(NexusListener::Management, "192.168.1.2", "enable").first == 403, "Bootstrap stays local");
    for (const auto* role : {"administrator", "moderator", "user"}) {
        std::string permissions;
        for (const auto& name : access::role_permissions(role)) {
            if (!permissions.empty()) permissions += ',';
            permissions += name.get<std::string>();
        }
        const httplib::Headers identity = {{"X-Nexus-Peer-Node", "caller"}, {"X-Nexus-Peer-Key", "ed25519:caller"},
            {"X-Nexus-Peer-Permissions", permissions}, {"X-Nexus-Family", "management"}, {"X-Nexus-Catalog-Version", "2"}};
        for (const auto* op : {"status", "members", "members/role", "tree/permissions", "tree/move", "invites", "route-permissions"}) {
            check(route(NexusListener::MeshIngress, "127.0.0.1", op, identity).first == (std::string(role) == "administrator" ? 0 : 403),
                  "Only attributed administrators may use remote management");
        }
        for (const auto* op : {"key/handoff", "key/unlock", "passkey/register", "account/delete", "enable", "disable", "relay", "selection", "invites/accept", "memberships/leave"}) {
            check(route(NexusListener::MeshIngress, "127.0.0.1", op, identity).first == 403,
                  "Even an administrator cannot remotely control local identity, lifecycle or relay");
        }
    }
    check(route(NexusListener::MeshIngress, "127.0.0.1", "members").first == 403, "Missing caller identity is refused");
    check(route(NexusListener::Public, "127.0.0.1", "relay").first == 0, "Local operators retain the relay");
    const json group_path = json::array({{{"assignments", json::array({
        {{"management_pubkey", "ed25519:owner"}, {"permissions", {"admin"}}},
        {{"management_pubkey", "ed25519:user"}, {"permissions", {"read", "connect_private", "svc:inference"}}}})}}});
    check(access::administers_tree(group_path, "ed25519:owner"), "Group ACL authorizes its administrator");
    check(!access::administers_tree(group_path, "ed25519:user") && !access::administers_tree(group_path, "ed25519:other"),
          "Server permissions cannot confer group administration on another caller");
    check(!access::administers_tree(json::array(), "ed25519:owner"), "Unavailable assignments fail closed");
    auto descendant = group_path;
    descendant.insert(descendant.begin(), json{{"assignments", json::array()}});
    check(access::administers_tree(descendant, "ed25519:owner"), "Ancestor administration applies within the group");
    auto revoked = group_path;
    revoked[0]["assignments"][0]["permissions"] = json::array({"read"});
    check(!access::administers_tree(revoked, "ed25519:owner"), "Revoked administration does not reuse an earlier grant");

    const json members = json::array({
        {{"node_id", "self"}, {"is_inference", true}, {"is_online", true}, {"tunnel_ip", "10.8.0.2/32"}},
        {{"node_id", "gpu"}, {"is_inference", true}, {"is_online", true}, {"tunnel_ip", "10.8.0.3/32"}},
        {{"node_id", "sleeping"}, {"is_inference", true}, {"is_online", false}, {"tunnel_ip", "10.8.0.4/32"}},
        {{"node_id", "laptop"}, {"is_inference", false}, {"is_online", true}, {"tunnel_ip", "10.8.0.5/32"}}});
    check(access::relay_target(members, "self", "gpu") == std::make_pair(0, std::string("10.8.0.3")), "Relay targets an online inference peer");
    check(access::relay_target(members, "self", "self").first == 400, "Relay refuses this device");
    check(access::relay_target(members, "self", "missing").first == 404, "Relay refuses an unknown device");
    check(access::relay_target(members, "self", "sleeping").first == 409, "Relay refuses an offline device");
    check(access::relay_target(members, "self", "laptop").first == 409, "Relay refuses a Tier 4 client");
}

void check_relay_stream(const std::function<void(bool, const char*)>& check) {
    httplib::Server peer;
    std::string seen_authorization;
    std::string seen_body;
    peer.Get("/api/v1/health", [&](const httplib::Request& req, httplib::Response& res) {
        seen_authorization = req.get_header_value("Authorization");
        res.set_content(R"({"status":"ok"})", "application/json");
    });
    peer.Post("/api/v1/chat/completions", [&](const httplib::Request& req, httplib::Response& res) {
        seen_authorization = req.get_header_value("Authorization");
        seen_body = req.body;
        if (seen_authorization != "Bearer peer-key") {
            res.status = 401;
            res.set_content(R"({"error":"API key required"})", "application/json");
            return;
        }
        const auto request = json::parse(req.body, nullptr, false);
        const auto failure = request.is_object() ? request.value("fail", std::string()) : std::string();
        if (failure == "before") {
            res.status = 503;
            res.set_content(R"({"error":{"message":"Model is loading","type":"backend_error"}})", "application/json");
            return;
        }
        res.set_chunked_content_provider("text/event-stream", [failure](size_t offset, httplib::DataSink& sink) {
            if (offset > 0) { sink.done(); return true; }
            if (failure == "during") {
                // One event, then the connection closes without the terminating chunk.
                const char* event = "data: {\"n\":1}\n\n";
                sink.write(event, std::char_traits<char>::length(event));
                return false;
            }
            for (const char* event : {"data: {\"n\":1}\n\n", "data: {\"n\":2}\n\n", "data: [DONE]\n\n"}) {
                sink.write(event, std::char_traits<char>::length(event));
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            sink.done();
            return true;
        });
    });
    const int peer_port = peer.bind_to_any_port("127.0.0.1");
    std::thread peer_worker([&] { peer.listen_after_bind(); });
    peer.wait_until_ready();

    httplib::Server front;
    const auto relay = [&](const httplib::Request& req, httplib::Response& res) {
        access::relay_http(static_cast<uint16_t>(peer_port), req.matches[1].str(), req, res, req.get_header_value(access::kPeerAuthorizationHeader), req.body);
    };
    front.Get(R"(/relay(/.*))", relay);
    front.Post(R"(/relay(/.*))", relay);
    front.Post(R"(/closed(/.*))", [&](const httplib::Request& req, httplib::Response& res) {
        access::relay_http(1, req.matches[1].str(), req, res, req.get_header_value(access::kPeerAuthorizationHeader), req.body);
    });
    const int front_port = front.bind_to_any_port("127.0.0.1");
    std::thread front_worker([&] { front.listen_after_bind(); });
    front.wait_until_ready();

    httplib::Client client("127.0.0.1", front_port);
    const httplib::Headers keys = {{"Authorization", "Bearer local-key"}, {access::kPeerAuthorizationHeader, "Bearer peer-key"}};
    auto streamed = client.Post("/relay/api/v1/chat/completions", keys, R"({"stream":true})", "application/json");
    check(streamed && streamed->status == 200, "Relay returns the peer status");
    check(streamed && streamed->get_header_value("Content-Type") == "text/event-stream", "Relay keeps the stream type");
    check(streamed && streamed->body == "data: {\"n\":1}\n\ndata: {\"n\":2}\n\ndata: [DONE]\n\n", "Relay delivers the stream intact");
    check(seen_authorization == "Bearer peer-key" && seen_body == R"({"stream":true})", "Relay sends only the peer key and the body");
    auto refused = client.Post("/relay/api/v1/chat/completions", keys, R"({"stream":true,"fail":"before"})", "application/json");
    check(refused && refused->status == 503, "A peer refusal before the stream keeps its status");
    check(refused && refused->get_header_value("Content-Type") == "application/json" &&
        refused->body == R"({"error":{"message":"Model is loading","type":"backend_error"}})", "A peer refusal before the stream keeps its JSON body");
    auto broken = client.Post("/relay/api/v1/chat/completions", keys, R"({"stream":true,"fail":"during"})", "application/json");
    check(broken && broken->status == 200 && broken->get_header_value("Content-Type") == "text/event-stream", "A stream that breaks stays a stream");
    const std::string first_event = "data: {\"n\":1}\n\n";
    const std::string error_prefix = "event: error\ndata: ";
    check(broken && broken->body.rfind(first_event, 0) == 0 && broken->body.compare(first_event.size(), error_prefix.size(), error_prefix) == 0 &&
        broken->body.size() > first_event.size() + error_prefix.size() + 2 && broken->body.substr(broken->body.size() - 2) == "\n\n",
        "A stream that breaks ends with one error event");
    if (broken && broken->body.size() > first_event.size() + error_prefix.size() + 2) {
        const auto payload = json::parse(broken->body.substr(first_event.size() + error_prefix.size(), broken->body.size() - first_event.size() - error_prefix.size() - 2), nullptr, false);
        check(payload.is_object() && payload.contains("error") && payload["error"].value("status_code", 0) == 502 &&
            !payload["error"].value("message", std::string()).empty(), "The error event carries the failure as JSON");
    }
    auto unauthorized = client.Post("/relay/api/v1/chat/completions", httplib::Headers{{"Authorization", "Bearer local-key"}}, "{}", "application/json");
    check(unauthorized && unauthorized->status == 401 && seen_authorization.empty(), "Relay never forwards the caller's own key");
    auto health = client.Get("/relay/api/v1/health", keys);
    check(health && health->status == 200 && health->body == R"({"status":"ok"})" && seen_authorization == "Bearer peer-key",
        "Relay forwards a GET with the peer key");
    auto unreachable = client.Post("/closed/api/v1/chat/completions", keys, "{}", "application/json");
    check(unreachable && unreachable->status == 502, "Relay reports an unreachable peer");
    front.stop(); front_worker.join();
    peer.stop(); peer_worker.join();
}

// A route that answers 200 so a test can tell a registered path from a missing one.
// A {name} in the path gets a string path argument, as a real route declares.
class StubRoute : public lemon::ApiRoute {
public:
    StubRoute(lemon::ServerContext& ctx, std::string id, std::string path, lemon::Prefixes prefixes,
              std::vector<std::string> methods = {"GET"})
        : lemon::ApiRoute(ctx), id_(std::move(id)), path_(std::move(path)), prefixes_(prefixes), methods_(std::move(methods)) {}
    lemon::RouteSpec spec() const override {
        lemon::RouteSpec s;
        s.id = id_;
        s.methods = methods_;
        s.paths = {path_};
        s.prefixes = prefixes_;
        for (size_t open = path_.find('{'); open != std::string::npos; open = path_.find('{', open + 1)) {
            lemon::RouteArg arg;
            arg.name = path_.substr(open + 1, path_.find('}', open) - open - 1);
            arg.in = lemon::ArgIn::Path;
            arg.schema = {{"type", "string"}};
            s.args.push_back(arg);
        }
        return s;
    }
    void handle(lemon::RouteRequest&, httplib::Response& res) override { res.set_content("{}", "application/json"); }

private:
    std::string id_, path_;
    lemon::Prefixes prefixes_;
    std::vector<std::string> methods_;
};

std::unique_ptr<StubRoute> stub(lemon::ServerContext& ctx, const char* id, const char* path, lemon::Prefixes prefixes,
                                std::vector<std::string> methods = {"GET"}) {
    return std::make_unique<StubRoute>(ctx, id, path, prefixes, std::move(methods));
}

// The catalog the SDK enforces at the mesh ingress: every registered route, grouped by
// family, with exact URLs or the prefix before a path parameter.
void check_service_catalog(const std::function<void(bool, const char*)>& check) {
    check(lemon::valid_route_family("inference") && lemon::valid_route_family("a") && lemon::valid_route_family("a.b-c_d9") &&
          lemon::valid_route_family(std::string(63, 'a')), "Family names follow the SDK grammar");
    check(!lemon::valid_route_family("") && !lemon::valid_route_family("Inference") && !lemon::valid_route_family("-x") &&
          !lemon::valid_route_family("a b") && !lemon::valid_route_family(std::string(64, 'a')), "Malformed family names are refused");

    lemon::ServerContext ctx;
    lemon::RouteRegistry registry;
    registry.add(stub(ctx, "test.chat", "chat/completions", lemon::Prefixes::Quad, {"POST"}), "inference");
    registry.add(stub(ctx, "test.models", "models", lemon::Prefixes::Quad), "inference");
    registry.add(stub(ctx, "test.model", "models/{id}", lemon::Prefixes::Quad), "inference");
    registry.add(stub(ctx, "test.options", "models/{id}/options", lemon::Prefixes::Quad, {"POST", "DELETE"}), "models_manage");
    registry.add(stub(ctx, "test.shutdown", "shutdown", lemon::Prefixes::Internal, {"POST"}), "internal");
    registry.add(stub(ctx, "test.mcp", "/mcp", lemon::Prefixes::Root, {"POST"}), "mcp");
    check(throws_logic([&] { registry.add(stub(ctx, "test.untagged", "untagged", lemon::Prefixes::Quad), ""); }), "Every route needs a family");
    check(throws_logic([&] { registry.add(stub(ctx, "test.upper", "upper", lemon::Prefixes::Quad), "Inference"); }), "A family name must match the grammar");
    const auto routes = registry.list();
    check(routes.size() == 6 && routes[0].at("family") == "inference" && routes[3].at("family") == "models_manage", "The route list carries each family");

    const auto catalog = access::service_catalog(routes, "2026.46.3");
    check(catalog.at("version") == 202604600300ULL + access::kCatalogRevision, "The catalog version packs the lemond version and the revision");
    check(catalog.at("app") == nlohmann::ordered_json({{"name", "lemonade"}, {"version", "2026.46.3"}}), "The catalog names the app");
    const auto& families = catalog.at("families");
    check(families.size() == 5 && families[0].at("name") == "inference" && families[1].at("name") == "models_manage" &&
          families[2].at("name") == "internal" && families[3].at("name") == "mcp", "Families keep registration order");
    const auto& inference = families[0].at("routes");
    check(inference.size() == 12, "A Quad route lists one entry per URL prefix");
    check(inference[0] == nlohmann::ordered_json({{"methods", {"POST"}}, {"path", "/api/v0/chat/completions"}}) &&
          inference[1].at("path") == "/api/v1/chat/completions" && inference[2].at("path") == "/v0/chat/completions" &&
          inference[3].at("path") == "/v1/chat/completions", "Exact paths carry their methods");
    check(inference[8] == nlohmann::ordered_json({{"methods", {"GET"}}, {"prefix", "/api/v0/models/"}}) &&
          !inference[8].contains("path"), "A path parameter becomes the prefix before it");
    check(families[1].at("routes")[1] == nlohmann::ordered_json({{"methods", {"POST", "DELETE"}}, {"prefix", "/api/v1/models/"}}),
          "A prefix claimed with other methods belongs to its own family");
    check(families[2].at("routes") == nlohmann::ordered_json::array({{{"methods", {"POST"}}, {"path", "/internal/shutdown"}}}),
          "Internal routes are catalogued under /internal/");
    check(families[3].at("routes")[0].at("path") == "/mcp", "Root routes keep their path");

    lemon::RouteRegistry conflicting;
    conflicting.add(stub(ctx, "test.model", "models/{id}", lemon::Prefixes::Quad), "inference");
    conflicting.add(stub(ctx, "test.files", "models/{id}/files", lemon::Prefixes::Quad), "models_manage");
    check(throws_logic([&] { access::service_catalog(conflicting.list(), "2026.46.3"); }), "Two families cannot claim one method and prefix");
    auto untagged = routes;
    untagged[0]["family"] = "";
    check(throws_logic([&] { access::service_catalog(untagged, "2026.46.3"); }), "A catalog refuses a route without a family");

    // The SDK refuses one method and URL listed twice, even within a family. Several
    // {id} routes under one collection share a prefix, a trailing slash is not another
    // path, and a path that equals a prefix of the same method is covered by it.
    const auto duplicate_claim = [](const nlohmann::ordered_json& catalog) {
        std::set<std::string> seen;
        for (const auto& family : catalog.at("families")) {
            for (const auto& route : family.at("routes")) {
                const bool exact = route.contains("path");
                for (const auto& method : route.at("methods")) {
                    const auto key = method.get<std::string>() + (exact ? " path " : " prefix ") + route.at(exact ? "path" : "prefix").get<std::string>();
                    if (!seen.insert(key).second) return key;
                }
            }
        }
        return std::string();
    };
    lemon::RouteRegistry repeated;
    repeated.add(stub(ctx, "test.model", "models/{id}", lemon::Prefixes::Quad), "inference");
    repeated.add(stub(ctx, "test.files", "models/{id}/files", lemon::Prefixes::Quad), "inference");
    repeated.add(stub(ctx, "test.options", "models/{id}/options", lemon::Prefixes::Quad, {"GET", "POST"}), "inference");
    repeated.add(stub(ctx, "test.list-slash", "models/", lemon::Prefixes::Quad), "inference");
    repeated.add(stub(ctx, "test.docs-slash", "docs/", lemon::Prefixes::Quad), "inference");
    repeated.add(stub(ctx, "test.root", "/", lemon::Prefixes::Root), "inference");
    const auto collapsed_catalog = access::service_catalog(repeated.list(), "2026.46.3");
    const auto& collapsed = collapsed_catalog.at("families")[0].at("routes");
    check(collapsed.size() == 13, "Repeated prefixes, covered paths and trailing slashes collapse");
    check(collapsed.size() == 13 && collapsed[0] == nlohmann::ordered_json({{"methods", {"GET"}}, {"prefix", "/api/v0/models/"}}) &&
          collapsed[3] == nlohmann::ordered_json({{"methods", {"GET"}}, {"prefix", "/v1/models/"}}) &&
          collapsed[4] == nlohmann::ordered_json({{"methods", {"POST"}}, {"prefix", "/api/v0/models/"}}) &&
          collapsed[8] == nlohmann::ordered_json({{"methods", {"GET"}}, {"path", "/api/v0/docs"}}) &&
          collapsed[12] == nlohmann::ordered_json({{"methods", {"GET"}}, {"path", "/"}}),
          "A repeated method drops from a later route, a trailing slash is stripped and the root path stays");
    check(duplicate_claim(collapsed_catalog).empty() && duplicate_claim(catalog).empty(), "No catalog lists a method and URL twice");

    check(access::catalog_version("2026.46.0~1780.7d29cf99") == 202604600000ULL + access::kCatalogRevision, "A development version packs its release numbers");
    check(access::catalog_version("2026.47.0") > access::catalog_version("2026.46.9") &&
          access::catalog_version("2027.1.0") > access::catalog_version("2026.52.9"), "Catalog versions grow with releases");
    check(access::catalog_version("9.1") == 0 && access::catalog_version("") == 0 && access::catalog_version("x.y.z") == 0 &&
          access::catalog_version("2026.1000.0") == 0, "A version without three numbers packs as 0");
    check(access::kCatalogRevision < 100, "The catalog revision fits its two digits");

    // The registry lemond serves: every route tagged with a known family, no conflicts.
    lemon::RouteRegistry real;
    lemon::register_openai_text_routes(real, ctx);
    lemon::register_openai_audio_routes(real, ctx);
    lemon::register_openai_images_routes(real, ctx);
    lemon::register_lemonade_models_routes(real, ctx);
    lemon::register_openai_models_routes(real, ctx);
    lemon::register_lemonade_load_routes(real, ctx);
    lemon::register_lemonade_downloads_routes(real, ctx);
    lemon::register_lemonade_inference_routes(real, ctx);
    lemon::register_lemonade_backends_routes(real, ctx);
    lemon::register_lemonade_system_routes(real, ctx);
    lemon::register_lemonade_jobs_routes(real, ctx);
    lemon::register_llamacpp_routes(real, ctx);
    lemon::register_router_routes(real, ctx);
    lemon::register_internal_routes(real, ctx);
    lemon::register_ollama_routes(real, ctx);
    lemon::register_anthropic_routes(real, ctx);
    lemon::register_mcp_routes(real, ctx);
    const auto all = real.list();
    std::map<std::string, int> per_family;
    bool known = true;
    for (const auto& route : all) {
        const auto family = route.value("family", std::string());
        known = known && std::find(access::kFamilies.begin(), access::kFamilies.end(), family) != access::kFamilies.end();
        ++per_family[family];
        if (route.value("prefixes", std::string()) == "Internal") known = known && family == "internal";
    }
    check(all.size() == 85 && known, "Every lemond route has one of the known families");
    check(per_family["inference"] == 34 && per_family["models_manage"] == 18 && per_family["internal"] == 14 && per_family["jobs"] == 7 &&
          per_family["backends"] == 5 && per_family["system"] == 3 && per_family["mcp"] == 2 && per_family["management"] == 2,
          "The family assignment matches the documented table");
    bool status_reads_in_inference = true;
    for (const auto& route : all) {
        for (const auto& path : route.at("paths")) {
            const auto name = path.get<std::string>();
            if (name == "system-stats" || name == "system-info" || name == "health" || name == "live") {
                status_reads_in_inference = status_reads_in_inference && route.value("family", std::string()) == "inference";
            }
        }
    }
    check(status_reads_in_inference, "Device status reads belong to the inference family");
    nlohmann::ordered_json live;
    check(!throws_logic([&] { live = access::service_catalog(all, LEMON_VERSION_STRING); }), "The live registry builds a catalog without conflicts");
    check(live.is_object() && live.at("version") == access::catalog_version(LEMON_VERSION_STRING) &&
          live.at("app").at("version") == LEMON_VERSION_STRING, "The live catalog carries lemond's version");
    bool chat_in_inference = false;
    bool shutdown_in_internal = false;
    for (const auto& family : live.value("families", nlohmann::ordered_json::array())) {
        for (const auto& route : family.at("routes")) {
            if (route.value("path", "") == "/api/v1/chat/completions") chat_in_inference = family.at("name") == "inference";
            if (route.value("path", "") == "/internal/shutdown") shutdown_in_internal = family.at("name") == "internal";
        }
    }
    check(chat_in_inference && shutdown_in_internal, "The live catalog places chat completions and shutdown in their families");
    int nexus_prefix_count = 0;
    for (const auto& family : live.at("families")) {
        if (family.at("name") != "management") continue;
        for (const auto& route : family.at("routes")) {
            if (route.value("prefix", "").find("/nexus/") != std::string::npos) ++nexus_prefix_count;
        }
    }
    check(nexus_prefix_count == 5, "The SDK catalog includes every Nexus alias under management");
    const auto repeated_live = duplicate_claim(live);
    check(repeated_live.empty(), ("The live catalog lists every method and URL once: " + repeated_live).c_str());
    const auto permissions = access::route_permissions(all);
    check(permissions.at("roles").size() == 3, "The route view publishes all three presets");
    std::set<std::string> displayed;
    bool accurate = true;
    bool parameterized = false;
    for (const auto& route : permissions.at("routes")) {
        for (const auto& item : route.at("paths")) {
            const auto path = item.get<std::string>();
            accurate = accurate && displayed.insert(route.at("method").get<std::string>() + " " + path).second &&
                path.rfind("/internal/", 0) != 0;
            parameterized = parameterized || path.find("{id}") != std::string::npos;
            const auto nexus = path.find("/nexus/");
            if (nexus != std::string::npos) {
                auto operation = path.substr(nexus + 7);
                const auto parameter = operation.find("{node_id}/");
                if (parameter != std::string::npos) operation.erase(parameter, 10);
                accurate = accurate && access::remote_operation(operation);
            }
        }
        const auto family = route.at("family").get<std::string>();
        for (const auto& role : permissions.at("roles")) {
            const auto id = role.at("id").get<std::string>();
            const bool expected = id == "admin" || family == "inference" ||
                (id == "moderator" && (family == "mcp" || family == "models_manage" || family == "jobs"));
            accurate = accurate && (std::find(route.at("roles").begin(), route.at("roles").end(), id) != route.at("roles").end()) == expected;
        }
    }
    check(accurate && parameterized, "The live route view preserves exact templates and role access without duplicates, internal or local-only operations");
    check(displayed.count("POST /api/v1/chat/completions") && displayed.count("POST /mcp") &&
        displayed.count("POST /api/v1/nexus/members/{node_id}/role") && displayed.count("GET /v1/nexus/route-permissions"),
        "The route view includes registered inference, MCP, management and alias routes");
#ifdef LEMONADE_WITH_NEXUS
    // The vendored SDK validates the catalog the way lemond's Tier 1 registration does;
    // no mesh is needed, and the closed port is never reached.
    std::unique_ptr<ln_client_t, decltype(&ln_destroy)> sdk(ln_create_tls("127.0.0.1", 9), ln_destroy);
    check(sdk != nullptr && ln_mesh_service_set_catalog(sdk.get(), 13305, live.dump().c_str()) == LN_OK, "The SDK accepts the live catalog");
    auto twice = live;
    twice["families"][0]["routes"].push_back(twice["families"][0]["routes"][0]);
    check(sdk != nullptr && ln_mesh_service_set_catalog(sdk.get(), 13305, twice.dump().c_str()) == LN_ERR_REJECTED,
          "The SDK refuses a repeated route, so the acceptance check is live");
#endif
}

void check_role_presets(const std::function<void(bool, const char*)>& check) {
    const json administrator = json::array({"read", "connect_private", "admin", "svc:inference", "svc:mcp", "svc:models_manage",
        "svc:backends", "svc:jobs", "svc:internal", "svc:system", "svc:management"});
    const json moderator = json::array({"read", "write", "add_child", "connect_private", "svc:inference", "svc:mcp", "svc:models_manage", "svc:jobs"});
    const json user = json::array({"read", "connect_private", "svc:inference"});
    check(access::role_permissions("administrator") == administrator, "Administrator holds admin, connect and every family");
    check(access::role_permissions("moderator") == moderator, "Moderator holds the tree names and the four families");
    check(access::role_permissions("user") == user, "User holds read, connect and inference");
    check(access::role_permissions("admin") == administrator && access::role_permissions("member") == user, "The former role names map to the presets");
    check(throws([&] { access::role_permissions("owner"); }) && throws([&] { access::role_permissions(""); }), "Unknown roles are refused");
    check(access::kFamilies.size() == 8, "Every family appears in the administrator preset");

    check(access::service_permission("svc:inference") && access::service_permission("svc:a.b-c_d") &&
          access::service_permission("svc:" + std::string(63, 'a')), "svc names follow the family grammar");
    check(!access::service_permission("svc:") && !access::service_permission("svc:Inference") && !access::service_permission("inference") &&
          !access::service_permission("svc:" + std::string(64, 'a')) && !access::service_permission("svc:a b"), "Malformed svc names are refused");
    check(access::group_permissions(json::array({"svc:inference", "write"}), false) == json::array({"read", "write", "svc:inference"}),
          "Group permissions accept svc names after the tree names");
    check(throws([&] { access::group_permissions(json::array({"svc:"}), false); }) &&
          throws([&] { access::group_permissions(json::array({"svc:Bad"}), false); }), "Group permissions refuse malformed svc names");
    check(access::invite_permissions(json::array({"svc:inference", "read"})) == json::array({"read", "svc:inference"}), "Invites carry svc names");
    check(throws([&] { access::invite_permissions(json::array({"svc:Bad"})); }), "Invites refuse malformed svc names");
    check(access::invite_permissions(access::role_permissions("moderator")) == moderator &&
          access::invite_permissions(access::role_permissions("user")) == user, "The moderator and user presets fit an invite");
    check(throws([&] { access::invite_permissions(access::role_permissions("administrator")); }), "An invite cannot carry the administrator preset");

    const json denied = {{"kind", "denied"}, {"vport", 13305}, {"peer_node_id", "node-a"}, {"peer_key", "ed25519:secret-key"},
        {"peer_tunnel_ip", "10.8.0.3"}, {"method", "POST"}, {"path", "/api/v1/chat/completions"}, {"family", "inference"},
        {"required", "svc:inference"}, {"catalog_version", 202604600300ULL}, {"time", "2026-10-28T00:00:00Z"}};
    const auto line = access::ingress_denied_log_line(denied);
    check(line == "Nexus route policy: denied POST /api/v1/chat/completions from node-a (family inference, needs svc:inference, catalog 202604600300)",
          "A refusal event becomes one log line");
    check(line.find("secret") == std::string::npos && line.find("10.8.0.3") == std::string::npos, "The log line carries no key or address");
    check(access::ingress_denied_log_line(json{{"kind", "unattributed"}, {"method", "GET"}, {"path", "/x"}}) ==
          "Nexus route policy: unattributed GET /x from - (family -, needs -, catalog -)", "Missing fields print as dashes");
    const auto hostile = access::ingress_denied_log_line(json{{"kind", "unregistered"}, {"method", "GET"}, {"path", "/a\nb\x7f"},
        {"peer_node_id", "n"}, {"required", json::array({"svc:a", "svc:b"})}, {"catalog_version", 7}});
    check(hostile == "Nexus route policy: unregistered GET /a?b? from n (family -, needs svc:a,svc:b, catalog 7)", "Control characters and lists are flattened");
    check(access::ingress_denied_log_line(json{{"path", std::string(300, 'p')}}).find(std::string(256, 'p') + "...") != std::string::npos,
          "Long fields are cut");
}

// The SDK sets the X-Nexus-* headers on a request it admitted to the mesh ingress
// listener. Every other listener removes them, so a client cannot pose as a mesh peer.
void check_peer_headers(const std::function<void(bool, const char*)>& check) {
    const httplib::Headers spoofed = {{"X-Nexus-Peer-Node", "node-a"}, {"X-Nexus-Peer-Key", "ed25519:k"}, {"X-Nexus-Peer-Permissions", "read,svc:inference"},
        {"x-nexus-family", "inference"}, {"X-Nexus-Catalog-Version", "7"}, {"Authorization", "Bearer key"}};
    httplib::Request req;
    req.headers = spoofed;
    access::accept_peer_headers(req, false);
    bool stripped = true;
    for (const auto& name : access::kPeerHeaders) stripped = stripped && !req.has_header(name);
    check(stripped && req.get_header_value("Authorization") == "Bearer key" && access::request_peer().node_id.empty(),
          "A non-mesh listener drops every peer header and keeps the rest");
    httplib::Request mesh;
    mesh.headers = spoofed;
    access::accept_peer_headers(mesh, true);
    check(mesh.get_header_value("X-Nexus-Peer-Node") == "node-a" && access::request_peer().node_id == "node-a" &&
          access::request_peer().family == "inference", "The mesh ingress listener keeps the headers and records the peer");
    httplib::Request plain;
    access::accept_peer_headers(plain, false);
    check(access::request_peer().node_id.empty() && access::request_peer().family.empty(), "The next request clears the peer");

    const auto install = [](httplib::Server& server, bool mesh_ingress) {
        server.set_pre_routing_handler([mesh_ingress](const httplib::Request& req, httplib::Response&) {
            access::accept_peer_headers(const_cast<httplib::Request&>(req), mesh_ingress);
            return httplib::Server::HandlerResponse::Unhandled;
        });
        server.Get("/echo", [](const httplib::Request& req, httplib::Response& res) {
            res.set_content(json{{"header", req.get_header_value("X-Nexus-Peer-Node")}, {"peer", access::request_peer().node_id},
                {"family", access::request_peer().family}}.dump(), "application/json");
        });
    };
    httplib::Server front;
    install(front, false);
    const int front_port = front.bind_to_any_port("127.0.0.1");
    std::thread front_worker([&] { front.listen_after_bind(); });
    front.wait_until_ready();
    httplib::Client client("127.0.0.1", front_port);
    auto echoed = client.Get("/echo", spoofed);
    check(echoed && echoed->status == 200 && json::parse(echoed->body) == json({{"header", ""}, {"peer", ""}, {"family", ""}}),
          "No route on a non-mesh listener sees a spoofed peer header");
    front.stop(); front_worker.join();
    httplib::Server ingress;
    install(ingress, true);
    const int ingress_port = ingress.bind_to_any_port("127.0.0.1");
    std::thread ingress_worker([&] { ingress.listen_after_bind(); });
    ingress.wait_until_ready();
    httplib::Client peer("127.0.0.1", ingress_port);
    auto attributed = peer.Get("/echo", spoofed);
    check(attributed && attributed->status == 200 && json::parse(attributed->body) == json({{"header", "node-a"}, {"peer", "node-a"}, {"family", "inference"}}),
          "Routes on the mesh ingress listener see the peer the SDK attributed");
    ingress.stop(); ingress_worker.join();
}

void check_status_fields(const std::filesystem::path& root, const std::function<void(bool, const char*)>& check) {
    lemon::ServerContext ctx;
    lemon::RouteRegistry registry;
    registry.add(stub(ctx, "test.chat", "chat/completions", lemon::Prefixes::Quad, {"POST"}), "inference");
    lemon::NexusManager manager((root / "status").string(), listener_policy, listener_policy, 0,
        access::service_catalog(registry.list(), "2026.46.3"), kSystemRole);
    const auto status = manager.status();
    check(status.at("catalog_version") == 202604600300ULL + access::kCatalogRevision, "Status reports the catalog version");
    check(status.at("app") == json({{"name", "lemonade"}, {"version", "2026.46.3"}}), "Status reports the catalog's app");
#ifdef LEMONADE_WITH_NEXUS
    check(status.at("route_policy") == (access::route_policy_available() ? "enforced" : "unavailable"),
          "Route policy follows the SDK's catalog interface");
#else
    check(status.at("route_policy") == "unavailable", "Route policy is unavailable without the SDK");
#endif
    manager.stop();
    lemon::NexusManager bare((root / "status-bare").string(), listener_policy, listener_policy, 0, nullptr, kSystemRole);
    const auto bare_status = bare.status();
    check(bare_status.at("catalog_version") == 0 && bare_status.at("route_policy") == "unavailable" &&
          bare_status.at("app") == json({{"name", "lemonade"}, {"version", LEMON_VERSION_STRING}}), "Without a catalog the route policy is unavailable");
    bare.stop();
}

// server.cpp installs the route registry on the mesh ingress listener without the
// /internal/* control routes: a peer arrives on loopback, where the admin key is usually unset.
void check_mesh_ingress_routes(const std::function<void(bool, const char*)>& check) {
    lemon::ServerContext ctx;
    lemon::RouteRegistry registry;
    registry.add(std::make_unique<StubRoute>(ctx, "test.health", "health", lemon::Prefixes::Quad), "inference");
    registry.add(std::make_unique<StubRoute>(ctx, "test.shutdown", "shutdown", lemon::Prefixes::Internal), "internal");

    httplib::Server mesh;
    registry.apply_without_internal(mesh);
    const int mesh_port = mesh.bind_to_any_port("127.0.0.1");
    std::thread mesh_worker([&] { mesh.listen_after_bind(); });
    mesh.wait_until_ready();
    httplib::Client peer("127.0.0.1", mesh_port);
    auto health = peer.Get("/api/v1/health");
    check(health && health->status == 200, "Mesh ingress serves the API routes");
    auto shutdown = peer.Get("/internal/shutdown");
    check(shutdown && shutdown->status == 404, "Mesh ingress has no /internal/* routes");
    mesh.stop(); mesh_worker.join();

    httplib::Server front;
    registry.apply(front);
    const int front_port = front.bind_to_any_port("127.0.0.1");
    std::thread front_worker([&] { front.listen_after_bind(); });
    front.wait_until_ready();
    httplib::Client local("127.0.0.1", front_port);
    auto local_shutdown = local.Get("/internal/shutdown");
    check(local_shutdown && local_shutdown->status == 200, "The public listener keeps its /internal/* routes");
    front.stop(); front_worker.join();
}

class RequestServer : public httplib::Server {
public:
    httplib::Response request(const std::string& address, const std::string& method, const std::string& path,
                              const std::string& body = "", const std::string& headers = "") {
        const auto input = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" +
            "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + headers + "\r\n" + body;
        httplib::detail::BufferStream stream;
        stream.write(input.data(), input.size());
        bool closed = false;
        if (!process_request(stream, address, 12345, "127.0.0.1", 8000, true, closed, nullptr)) {
            throw std::runtime_error("Test HTTP request failed");
        }
        const auto output = stream.get_buffer().substr(input.size());
        httplib::Response response;
        response.status = std::stoi(output.substr(9, 3));
        response.body = output.substr(output.find("\r\n\r\n") + 4);
        return response;
    }
};

void check_private_management(const std::filesystem::path& root, const std::function<void(bool, const char*)>& check) {
    const auto saved_key = lemon::utils::get_environment_variable_utf8("LEMONADE_API_KEY");
    const auto saved_admin = lemon::utils::get_environment_variable_utf8("LEMONADE_ADMIN_API_KEY");
    for (const auto* key : {"", "private-test-key"}) {
        set_environment("LEMONADE_API_KEY", key);
        set_environment("LEMONADE_ADMIN_API_KEY", "private-admin-key");
        lemon::RuntimeConfig config(json{{"host", "127.0.0.1"}});
        lemon::RouteRegistry registry;
        lemon::RequestMiddleware policy(&config, &registry);
        const auto install = [&](httplib::Server& server) { policy.install(server); };
        const auto ok = [](const auto&, auto& res) { res.set_content("{}", "application/json"); };
        lemon::NexusManager manager((root / (*key ? "private-access-keyed" : "private-access-unkeyed")).string(),
                                   install, install, 0, nullptr, kSystemRole,
                                   [&](httplib::Server& local) { local.Get("/api/v1/models", ok); },
                                   access::route_permissions(registry.list()));
        const int controller_port = std::stoi(manager.status().at("controller_url").get<std::string>().substr(17));
        httplib::Client local_selection("127.0.0.1", controller_port);
        const auto local_models = local_selection.Get("/api/v1/models");
        check(local_models && local_models->status == (*key ? 401 : 200),
              "Local selection uses ordinary API authentication without requiring a remote mesh identity");
        local_selection.set_default_headers({{"Authorization", "Bearer private-admin-key"}});
        const auto authorized_models = local_selection.Get("/api/v1/models");
        check(authorized_models && authorized_models->status == 200, "Local selection retains valid API-key access");
        RequestServer server;
        install(server);
        manager.register_routes(server);
        server.Get("/api/v1/models", ok);
        server.Get("/api/v1/nexus-other/status", ok);
        server.Get("/internal/config", ok);
        server.Post("/mcp", ok);
        for (const auto* address : {"10.0.0.0", "10.255.255.255", "172.16.0.0", "172.31.255.255",
                                    "192.168.0.0", "192.168.255.255", "::ffff:192.168.1.2", "::FFFF:ac10:1"}) {
            for (const auto* prefix : {"/api/v0/nexus/", "/api/v1/nexus/", "/v0/nexus/", "/v1/nexus/", "/api/nexus/"}) {
                for (const auto* operation : {"status", "enable", "device", "passkey/register", "key/unlock", "account/delete", "peers/gpu/api/v1/chat/completions"}) {
                    for (const auto* headers : {"", "Authorization: Bearer private-test-key\r\n", "Authorization: Bearer private-admin-key\r\n"}) {
                        check(server.request(address, operation == std::string("status") ? "GET" : "POST",
                                             std::string(prefix) + operation, "{}", headers).status == 403,
                              "Private callers need mesh identity even when an API key matches");
                    }
                }
            }
        }
        check(server.request("192.168.1.2", "GET", "/api/v1/nexus/status", "", "Origin: https://untrusted.example\r\n").status == 403,
              "Private management still checks the browser origin");
        for (const auto* address : {"9.255.255.255", "11.0.0.0", "172.15.255.255", "172.32.0.0",
                                    "192.167.255.255", "192.169.0.0", "100.64.0.1", "169.254.1.2", "203.0.113.1",
                                    "fc00::1", "2001:db8::1", "::ffff:203.0.113.1", "", "10.1.2.3.invalid"}) {
            check(server.request(address, "GET", "/api/v1/nexus/status").status == 403,
                  "Public remote Nexus requests require the authenticated mesh");
            check(server.request(address, "GET", "/api/v1/nexus/status", "",
                                 "X-Forwarded-For: 10.1.2.3\r\nForwarded: for=10.1.2.3\r\nX-Real-IP: 10.1.2.3\r\n").status == 403,
                  "Forwarded headers cannot grant local operator access");
        }
        for (const auto* path : {"/api/v1/models", "/api/v1/nexus-other/status"}) {
            check(server.request("192.168.1.2", "GET", path).status == (*key ? 401 : 200),
                  "Private-network exemption is limited to Nexus routes");
        }
        check(server.request("192.168.1.2", "POST", "/mcp", "{}").status == (*key ? 401 : 200),
              "Private Nexus access does not bypass MCP authentication");
        check(server.request("192.168.1.2", "GET", "/internal/config").status == 401,
              "Private Nexus access does not bypass the internal admin key");
        RequestServer management;
        install(management);
        manager.register_routes(management, lemon::NexusListener::Management);
        const auto routes = management.request("127.0.0.1", "GET", "/api/v1/nexus/route-permissions");
        check(routes.status == 200 && json::parse(routes.body).at("roles").size() == 3 &&
            json::parse(routes.body).contains("route_policy"), "Local route metadata requires no API key");
        check(server.request("192.168.1.2", "GET", "/api/v1/nexus/route-permissions", "", "Authorization: Bearer private-admin-key\r\n").status == 403,
            "The route metadata endpoint grants no LAN or API-key bypass");
        for (const auto* address : {"127.0.0.1", "::1", "::ffff:127.0.0.1"}) {
            for (const auto* prefix : {"/api/v0/nexus/", "/api/v1/nexus/", "/v0/nexus/", "/v1/nexus/", "/api/nexus/"}) {
                for (const auto* headers : {"", "Authorization: Bearer stale-key\r\n"}) {
                    const auto status = server.request(address, "GET", std::string(prefix) + "status", "", headers);
                    check(status.status == 200 && json::parse(status.body).contains("controller_url"),
                          "Local Nexus discovery needs no API key and returns its controller");
                    check(server.request(address, "POST", std::string(prefix) + "enable", "{}", headers).status == 307,
                          "Local Nexus setup retains the controller redirect without requiring an API key");
                    check(management.request(address, "POST", std::string(prefix) + "enable", "[]", headers).status == 400,
                          "The local Nexus controller reaches body validation without an API key");
                }
            }
            check(server.request(address, "GET", "/api/v1/models").status == (*key ? 401 : 200),
                  "Local Nexus access does not exempt direct inference API routes");
            check(server.request(address, "POST", "/mcp", "{}").status == (*key ? 401 : 200),
                  "Local Nexus access does not exempt MCP");
            check(server.request(address, "GET", "/internal/config").status == 401,
                  "Local Nexus access does not exempt the internal admin key");
            check(server.request(address, "GET", "/api/v1/nexus/status", "", "Origin: https://untrusted.example\r\n").status == 403,
                  "Local Nexus access still validates browser origins");
        }
        check(!manager.status().at("enabled"), "Refused LAN setup leaves Nexus disabled");
        RequestServer mesh;
        policy.install(mesh, true);
        manager.register_routes(mesh, lemon::NexusListener::MeshIngress);
        for (const auto* address : {"127.0.0.1", "192.168.1.2"}) {
            check(mesh.request(address, "GET", "/api/v1/nexus/status").status == 403,
                  "Mesh ingress never receives the private-network API-key exemption");
            check(mesh.request(address, "GET", "/api/v1/nexus/status", "", "Authorization: Bearer private-admin-key\r\n").status == 403,
                  "Mesh ingress refuses management regardless of the source address");
        }
        mesh.Get("/api/v1/models", ok);
        mesh.Post("/mcp", ok);
        const auto attributed = [](const std::string& permissions, const std::string& family) {
            return "X-Nexus-Peer-Node: caller\r\nX-Nexus-Peer-Key: ed25519:caller\r\nX-Nexus-Catalog-Version: 2\r\n" +
                std::string("X-Nexus-Peer-Permissions: ") + permissions + "\r\nX-Nexus-Family: " + family + "\r\n";
        };
        for (const auto* role : {"administrator", "moderator", "user"}) {
            std::string permissions;
            for (const auto& name : access::role_permissions(role)) {
                if (!permissions.empty()) permissions += ',';
                permissions += name.get<std::string>();
            }
            for (const auto* authorization : {"", "Authorization: Bearer stale-key\r\n"}) {
                const auto identity = attributed(permissions, "management") + authorization;
                const auto status = mesh.request("127.0.0.1", "GET", "/api/v1/nexus/status", "", identity);
                const auto routes = mesh.request("127.0.0.1", "GET", "/api/v1/nexus/route-permissions", "", identity);
                check(routes.status == (std::string(role) == "administrator" ? 200 : 403),
                    "Only an attributed mesh administrator can read the route metadata remotely");
                if (routes.status == 200) {
                    const auto route_data = json::parse(routes.body);
                    for (const auto& route : route_data.at("routes")) {
                        for (const auto& item : route.at("paths")) {
                            auto path = item.get<std::string>();
                            const auto parameter = path.find("{node_id}");
                            if (parameter != std::string::npos) path.replace(parameter, 9, "test-node");
                            check(mesh.request("127.0.0.1", route.at("method"), path, "[]", identity).status == 400,
                                "Every advertised Nexus method, path template and alias reaches the registered handler");
                        }
                    }
                }
                check(status.status == (std::string(role) == "administrator" ? 200 : 403),
                      "Real middleware permits only SDK-attributed administrators to read Nexus status without an API key");
                if (status.status == 200) check(!json::parse(status.body).contains("controller_url"), "Remote status hides the local controller");
                check(mesh.request("127.0.0.1", "GET", "/api/v1/models", "", attributed(permissions, "inference") + authorization).status == 200,
                      "All preset roles may use attributed inference without an API key");
                check(mesh.request("127.0.0.1", "POST", "/mcp", "{}", attributed(permissions, "mcp") + authorization).status ==
                      (std::string(role) == "user" ? 403 : 200), "Service grants distinguish user and moderator access");
                check(server.request("192.168.1.2", "GET", "/api/v1/nexus/status", "", identity).status == 403,
                      "Public listener rejects forged SDK identity headers");
                check(mesh.request("127.0.0.1", "POST", "/api/v1/nexus/account/delete", "{}", identity).status == 403,
                      "Remote callers cannot delete the server identity");
            }
        }
        for (const auto* permissions : {"svc:inference", "connect_private", "connect_shared,svc:inference", "connect_private,svc:inference_extra"}) {
            check(mesh.request("127.0.0.1", "GET", "/api/v1/models", "", attributed(permissions, "inference")).status == 403,
                  "Inference needs both exact private-connection and inference grants");
        }
        check(mesh.request("127.0.0.1", "GET", "/api/v1/nexus/status", "", attributed("connect_private,svc:management", "management")).status == 403,
              "A management service grant alone cannot administer Nexus");
        check(mesh.request("127.0.0.1", "GET", "/api/v1/models", "", attributed("admin", "inference") +
                           "X-Nexus-Peer-Key: forged\r\n").status == 403, "Ambiguous peer attribution is refused");
        check(mesh.request("127.0.0.1", "GET", "/api/v1/models", "", "Authorization: Bearer private-admin-key\r\n").status == 403,
              "An API key never bypasses missing mesh identity");
        manager.stop();
    }
    set_environment("LEMONADE_API_KEY", saved_key);
    set_environment("LEMONADE_ADMIN_API_KEY", saved_admin);
}

void check_listener_isolation(const std::filesystem::path& root, const std::function<void(bool, const char*)>& check) {
    const auto saved_key = lemon::utils::get_environment_variable_utf8("LEMONADE_API_KEY");
    const auto saved_admin = lemon::utils::get_environment_variable_utf8("LEMONADE_ADMIN_API_KEY");
    set_environment("LEMONADE_ADMIN_API_KEY", "");
    set_environment("LEMONADE_API_KEY", "remote-test-key");
    {
        lemon::NexusManager manager((root / "isolation").string(), listener_policy, listener_policy, 0, nullptr, kSystemRole);
        httplib::Server mesh;
        manager.register_routes(mesh, lemon::NexusListener::MeshIngress);
        const int mesh_port = mesh.bind_to_any_port("127.0.0.1");
        std::thread mesh_worker([&] { mesh.listen_after_bind(); });
        mesh.wait_until_ready();
        httplib::Client peer("127.0.0.1", mesh_port);
        peer.set_default_headers({{"Authorization", "Bearer remote-test-key"}});
        auto status = peer.Get("/api/v1/nexus/status");
        check(status && status->status == 403, "Mesh peers cannot read Nexus status");
        auto enable = peer.Post("/api/v1/nexus/enable", "{}", "application/json");
        check(enable && enable->status == 403 && !manager.status().at("enabled"), "Mesh peers cannot enable Nexus");
        auto role = peer.Post("/api/v1/nexus/members/peer/role", R"({"role":"admin"})", "application/json");
        check(role && role->status == 403, "Mesh peers cannot change member roles");
        auto selection = peer.Post("/api/v1/nexus/selection", R"({"target":"local"})", "application/json");
        check(selection && selection->status == 403, "Mesh peers cannot change the mesh port selection");
        auto bounced = peer.Post("/api/v1/nexus/peers/gpu/api/v1/chat/completions", "{}", "application/json");
        check(bounced && bounced->status == 403, "Mesh peers cannot relay through this device");
        auto handoff = peer.Get("/api/v1/nexus/key/handoff");
        check(handoff && handoff->status == 403, "Mesh peers cannot start an identity hand-off");
        mesh.stop(); mesh_worker.join();

        RequestServer server;
        manager.register_routes(server);
        const std::string address = "203.0.113.1";
        const std::string valid = "Authorization: Bearer remote-test-key\r\n";
        for (const auto* headers : {"", "Authorization: Bearer wrong\r\n", "Authorization: Bearer remote-test-key\r\n"}) {
            for (const auto* op : {"status", "members", "peers/gpu/api/v1/chat/completions"}) {
                check(server.request(address, "GET", std::string("/api/v1/nexus/") + op, "", headers).status == 403,
                      "Public remote requests cannot borrow the server identity using an API key");
            }
        }
        for (const auto* operation : {"enable", "passkey/unlock", "key/unlock"}) {
            check(server.request(address, "POST", std::string("/api/v1/nexus/") + operation, "{}", valid).status == 403,
                  "Public clients cannot enable or unlock Nexus");
        }
        check(server.request(address, "GET", "/api/v1/nexus/key/handoff", "", valid).status == 403,
              "Public clients cannot start an identity hand-off");
        const std::string relay = "/api/v1/nexus/peers/gpu/api/v1/chat/completions";
        check(server.request(address, "POST", relay, "{}", valid).status == 403,
              "An API key cannot authorize the remote relay");
        check(server.request(address, "POST", relay, "{}").status == 403, "Public remote relay requires a direct mesh connection instead");
        set_environment("LEMONADE_API_KEY", "");
        check(server.request(address, "GET", "/api/v1/nexus/members", "", valid).status == 403,
              "Public management is refused without a configured API key");
        check(server.request(address, "POST", relay, "{}", valid).status == 403,
              "Public relay is refused without a configured API key");

        manager.stop();
    }
    set_environment("LEMONADE_API_KEY", saved_key);
    set_environment("LEMONADE_ADMIN_API_KEY", saved_admin);
}

void check_access_rules(const std::function<void(bool, const char*)>& check) {
    const std::string admin_key = "ed25519:YWRt+aW4/bg==";
    const std::string member_key = "ed25519:bWVtYmVy";
    const std::string laptop_key = "ed25519:bGFwdG9w";
    check(access::same_key(admin_key, "ed25519:YWRt-aW4_bg"), "Keys match across base64 spellings");
    check(!access::same_key("", ""), "Empty keys never match");
    check(access::grants(json::array({"admin"}), "delete_node"), "Admin grants every permission");
    check(!access::grants(json::array({"read"}), "connect_private"), "Read does not grant connect");

    const json group = {{"id", "customer-admin"}, {"mgmt_pubkey", admin_key}, {"assignments", {
        {{"management_pubkey", admin_key}, {"permissions", {"read", "write", "add_child", "delete_node", "edit_node", "admin"}}},
        {{"management_pubkey", member_key}, {"permissions", {"read", "write"}}},
        {{"management_pubkey", laptop_key}, {"permissions", {"read"}}}}}};
    const json children = json::array({
        {{"id", "server"}, {"type", "endpoint"}, {"hostname", "Server"}, {"tunnel_ip", "10.8.0.2/32"}, {"mgmt_pubkey", admin_key},
         {"is_inference", true}, {"assignments", {{{"management_pubkey", admin_key}, {"permissions", {"read", "write", "add_child", "delete_node", "edit_node"}}}}}},
        {{"id", "workstation"}, {"type", "endpoint"}, {"hostname", "Workstation"}, {"tunnel_ip", "10.8.0.3/32"}, {"mgmt_pubkey", member_key},
         {"is_inference", true}, {"assignments", {
            {{"management_pubkey", member_key}, {"permissions", {"read", "write", "edit_node"}}},
            {{"management_pubkey", admin_key}, {"permissions", {"read", "write", "add_child", "delete_node", "edit_node"}}},
            {{"management_pubkey", laptop_key}, {"permissions", {"read", "connect_private"}}}}}},
        {{"id", "laptop"}, {"type", "endpoint"}, {"hostname", "Laptop"}, {"tunnel_ip", "10.8.0.4/32"}, {"mgmt_pubkey", laptop_key},
         {"is_inference", false}, {"assignments", {{{"management_pubkey", laptop_key}, {"permissions", {"read", "write", "edit_node"}}}}}},
        {{"id", "group-relay"}, {"type", "relay"}, {"hostname", "Relay"}}});
    const json peers = json::array({{{"node_id", "workstation"}, {"is_online", true}, {"latency_ms", 12.5}}});

    const auto members = access::merge_members(group, children, peers, admin_key, "server");
    check(members.size() == 3, "Members list only endpoint devices");
    check(members[0].at("tier") == 3 && members[1].at("tier") == 3 && members[2].at("tier") == 4, "Tier follows is_inference");
    check(members[0].at("is_admin") && members[0].at("is_owner"), "Group creator is admin");
    check(!members[1].at("is_admin") && members[1].at("permissions") == json::array({"read", "write"}), "Member keeps group permissions");
    check(members[1].at("is_online") && members[1].at("latency_ms") == 12.5, "Peer state is merged");
    auto candidates = peers;
    candidates[0]["endpoint"] = "192.0.2.20:51820";
    candidates[0]["relay_endpoint"] = "192.0.2.30:9103";
    check(access::merge_members(group, children, candidates, admin_key, "server")[1].at("connection_type") == "unknown",
          "Discovery candidates never establish an active connection path");
    json connections = json::array({{{"node_id", "workstation"}, {"is_online", true},
        {"endpoint", "192.0.2.20:51820"}, {"last_rx", 123}}});
    const auto direct = access::merge_members(group, children, candidates, admin_key, "server", connections);
    check(direct[0].at("connection_type") == "local" && direct[1].at("connection_type") == "p2p",
          "The local device stays local and authenticated direct traffic reports P2P");
    connections[0]["relay_endpoint"] = "192.0.2.30:9103";
    check(access::merge_members(group, children, candidates, admin_key, "server", connections)[1].at("connection_type") == "relay",
          "An active relay overrides a discovered direct address");
    connections[0]["relay_endpoint"] = "";
    check(access::merge_members(group, children, candidates, admin_key, "server", connections)[1].at("connection_type") == "p2p",
          "Leaving the relay restores the P2P label");
    connections[0]["relay_endpoint"] = "192.0.2.30:9103";
    for (const auto* path : {"lan", "direct"}) {
        connections[0]["path"] = path;
        check(access::merge_members(group, children, candidates, admin_key, "server", connections)[1].at("connection_type") == "p2p",
              "The SDK's current direct path overrides a stale relay endpoint");
    }
    connections[0]["relay_endpoint"] = "";
    connections[0]["path"] = "relay";
    check(access::merge_members(group, children, candidates, admin_key, "server", connections)[1].at("connection_type") == "relay",
          "The SDK's current relay path overrides a stale direct endpoint");
    for (const auto* path : {"", "unrecognized"}) {
        connections[0]["path"] = path;
        check(access::merge_members(group, children, candidates, admin_key, "server", connections)[1].at("connection_type") == "unknown",
              "A missing or unrecognized SDK path never falls back to a stale endpoint");
    }
    connections[0]["path"] = "direct";
    connections[0]["is_online"] = false;
    check(access::merge_members(group, children, candidates, admin_key, "server", connections)[1].at("connection_type") == "unknown",
          "An offline tunnel never retains a path label");
    connections[0]["is_online"] = true;
    connections[0]["last_rx"] = 0;
    check(access::merge_members(group, children, candidates, admin_key, "server", connections)[1].at("connection_type") == "unknown",
          "A tunnel awaiting authenticated traffic does not claim P2P");
    check(members[1].at("can_remove") && !members[2].at("can_remove") && !members[0].at("can_remove"), "Removal follows delete_node on the device");
    check(members[1].at("access_known") && !members[2].at("access_known"), "Device access is known only with edit_node on the device");

    const auto workstation = access::device_access(members, "workstation", "server");
    check(workstation.at("can_manage") && workstation.at("access_known"), "Admin manages visible device access");
    check(workstation.at("members").size() == 1 && workstation.at("members")[0].at("node_id") == "laptop", "Access rows exclude the device and this device");
    check(workstation.at("members")[0].at("see") && workstation.at("members")[0].at("connect"), "Access maps read and connect_private");
    check(workstation.at("members")[0].at("editable"), "Visible access is editable by an admin");
    check(access::device_access(members, "missing", "server").is_null(), "Unknown device has no access view");
    check(!access::device_access(members, "laptop", "server").at("members")[0].at("editable"), "Hidden access is not editable");

    const json redacted = {{"id", "customer-admin"}, {"mgmt_pubkey", admin_key}, {"assignments", {{{"management_pubkey", laptop_key}, {"permissions", {"read"}}}}}};
    const auto member_view = access::merge_members(redacted, children, peers, laptop_key, "laptop");
    check(member_view[0].at("is_admin") && !member_view[0].at("permissions_known"), "Members see the creator as admin without the full list");
    check(!member_view[1].at("is_admin") && member_view[1].at("permissions").empty(), "Members do not guess other permissions");
    check(member_view[2].at("permissions_known") && member_view[2].at("permissions") == json::array({"read"}), "Members see their own permissions");
    check(!access::device_access(member_view, "workstation", "laptop").at("can_manage"), "Members cannot manage access");

    check(access::role_permissions("admin").back() == "svc:management" && access::role_permissions("admin")[2] == "admin", "Promotion grants the administrator preset");
    check(!access::grants(access::role_permissions("member"), "write"), "Demotion leaves the user preset");
    check(access::group_permissions(json::array({"add_child"}), false) == json::array({"read", "add_child"}), "Group permissions always include read");
    check(access::group_permissions(json::array({"write"}), true) == json::array({"read", "write", "admin"}), "Group permissions keep the admin role");
    check(throws([&] { access::group_permissions(json::array({"admin"}), false); }), "Admin is granted only through the role");
    check(throws([&] { access::group_permissions(json::array({"superuser"}), false); }), "Unknown permissions are rejected");
    check(throws([&] { access::group_permissions(json::array({1}), false); }), "Permission names must be strings");
    check(access::device_permissions(json::array({"read", "write", "edit_node"}), false, true) == json::array({"read", "write", "edit_node", "connect_private"}), "Connect implies see and keeps other permissions");
    check(access::device_permissions(json::array({"read", "connect_private"}), false, false).empty(), "Clearing access removes the entry");
    check(access::device_permissions(json::array({"read", "connect_private"}), true, false) == json::array({"read"}), "See without connect keeps read only");

    const auto error = [](const std::string& reason) { return access::assignment_error(reason); };
    check(error("only an admin of this node or an ancestor may change assignments").first == 403, "Non-admin refusal maps to 403");
    check(error("an admin cannot change its own entry on the node that grants it") == std::make_pair(403, std::string("You cannot change your own admin access")), "Self refusal is explained");
    check(error("the identity has not joined this group").second == "That device has not joined your group", "Outside identity refusal is explained");
    check(error("unknown permission 'x'").first == 400, "Unknown permission maps to 400");
    check(error("node not found").first == 404, "Missing node maps to 404");
    check(error("server unreachable").first == 502 && error("").first == 502, "Transport failures map to 502");
}

// The identity source follows the process role alone; the account files follow the key.
void check_identity_rules(const std::function<void(bool, const char*)>& check) {
    using lemon::IdentitySource;
    using Role = lemon::ProcessRole;
    const auto source = [](Role::Os os, bool root, bool state_directory) {
        return access::identity_source(Role{os, root, state_directory});
    };
    check(source(Role::Os::Windows, false, false) == IdentitySource::User && source(Role::Os::Windows, true, true) == IdentitySource::User,
          "Windows always uses the user store");
    check(source(Role::Os::MacOS, true, false) == IdentitySource::Handoff && source(Role::Os::MacOS, true, true) == IdentitySource::Handoff,
          "A root lemond on macOS waits for a hand-off");
    check(source(Role::Os::MacOS, false, false) == IdentitySource::User && source(Role::Os::MacOS, false, true) == IdentitySource::User,
          "A user's lemond on macOS uses the user store");
    check(source(Role::Os::Linux, false, true) == IdentitySource::System && source(Role::Os::Linux, true, false) == IdentitySource::System &&
          source(Role::Os::Linux, true, true) == IdentitySource::System, "The Linux system unit and root keep the system key");
    check(source(Role::Os::Linux, false, false) == IdentitySource::User, "A Linux user's lemond uses the user store");
    check(std::string(access::identity_source_name(IdentitySource::System)) == "system" &&
          std::string(access::identity_source_name(IdentitySource::User)) == "user" &&
          std::string(access::identity_source_name(IdentitySource::Handoff)) == "handoff", "Sources have their status names");
    {
        const auto saved = lemon::utils::get_environment_variable_utf8("STATE_DIRECTORY");
        set_environment("STATE_DIRECTORY", "/var/lib/lemonade");
        check(access::current_process_role().state_directory, "The process role reads STATE_DIRECTORY");
        set_environment("STATE_DIRECTORY", "");
        check(!access::current_process_role().state_directory, "The process role sees an unset STATE_DIRECTORY");
        set_environment("STATE_DIRECTORY", saved);
    }
    check(access::identity_source(kSystemRole) == IdentitySource::System && access::identity_source(kUserRole) == IdentitySource::User,
          "The test roles resolve as intended on every host");

    check(access::account_file_tag("ed25519:YWRt+aW4/bg==") == "YWRt-aW4_bg", "An account tag is URL-safe base64 without padding");
    check(access::account_file_tag("ed25519:ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstu=").size() == 16, "An account tag is 16 characters");
    check(access::account_file_tag("").empty() && access::account_file_tag("ed25519:../x").empty(), "A malformed key has no tag");
    const json previous = {{"server", "sn01.example"}, {"genesis_pubkey", "g"}, {"tier1_epoch", 7}, {"device_name", "My laptop"},
        {"user_id", "u1"}, {"unlock", "device_key"}, {"credential_id", "c1"}, {"public_key", "ed25519:a"}};
    const auto fresh = access::account_for_key(previous, nullptr, "ed25519:b");
    check(fresh == json({{"server", "sn01.example"}, {"genesis_pubkey", "g"}, {"tier1_epoch", 7}, {"device_name", "My laptop"}, {"public_key", "ed25519:b"}}),
          "A new key's account carries the network and machine facts only");
    const auto restored = access::account_for_key(previous, json{{"device_name", "Other"}, {"user_id", "u2"}, {"unlock", "device_key"}}, "ed25519:b");
    check(restored.at("device_name") == "Other" && restored.at("user_id") == "u2" && restored.at("server") == "sn01.example" &&
          restored.at("genesis_pubkey") == "g" && restored.at("public_key") == "ed25519:b" && !restored.contains("credential_id"),
          "An archived account keeps its own fields and gains the missing network facts");

    // The federation's user_id decides whose a stale account is; a key is never refused
    // for a file that is not recorded as its own.
    using access::AccountMatch;
    check(access::account_match(json::object(), "ed25519:a", "u1") == AccountMatch::Own &&
          access::account_match(json{{"user_id", "u1"}}, "ed25519:a", "u1") == AccountMatch::Own &&
          access::account_match(json{{"user_id", "u1"}, {"public_key", "ed25519:a"}}, "ed25519:a", "u1") == AccountMatch::Own,
          "An account without a user or with the same user is the key's own");
    check(access::account_match(json{{"user_id", "u2"}}, "ed25519:a", "u1") == AccountMatch::Stale &&
          access::account_match(json{{"user_id", "u2"}, {"public_key", "ed25519:b"}}, "ed25519:a", "u1") == AccountMatch::Stale,
          "Another user's account that is not recorded as this key's is stale");
    check(access::account_match(json{{"user_id", "u2"}, {"public_key", "ed25519:a"}}, "ed25519:a", "u1") == AccountMatch::Conflict,
          "Another user on an account recorded as this key's is a conflict");
    check(access::stale_account_file(json{{"user_id", "u2"}, {"public_key", "ed25519:YWRt+aW4/bg=="}}) == "account-YWRt-aW4_bg.json" &&
          access::stale_account_file(json{{"user_id", "0123456789abcdef0123"}}) == "account-user-0123456789abcdef.json" &&
          access::stale_account_file(json{{"user_id", "a b"}}) == "account-user-unknown.json",
          "A stale account archives by its key, else by its user");

    // An invite without a role or list grants the user preset, reduced to what the
    // inviter holds on the target; a name it lacks drops instead of failing the invite.
    const json preset = json::array({"read", "connect_private", "svc:inference"});
    check(access::default_invite_permissions(json::array({"read", "connect_private", "admin"}), true) == std::make_pair(preset, json::array()) &&
          access::default_invite_permissions(json::array(), true) == std::make_pair(preset, json::array()), "An admin grants the whole user preset");
    check(access::default_invite_permissions(preset, false) == std::make_pair(preset, json::array()), "An inviter that holds the preset grants it");
    check(access::default_invite_permissions(json::array({"read", "write", "add_child"}), false) ==
          std::make_pair(json::array({"read"}), json::array({"connect_private", "svc:inference"})), "Names the inviter lacks drop from the default");
    check(access::default_invite_permissions(json::array(), false) == std::make_pair(json(nullptr), preset), "Nothing held leaves the federation default");
    check(access::invite_permissions(access::default_invite_permissions(json::array({"read", "svc:inference"}), false).first) ==
          json::array({"read", "svc:inference"}), "The reduced default is a valid invite list");

    namespace user_key = lemon::nexus_user_key;
    check(user_key::wants_device_key(json{{"available", true}, {"enabled", true}, {"needs_user_key", true}}), "An enabled mesh without an identity asks for the key");
    check(!user_key::wants_device_key(json{{"available", true}, {"enabled", false}, {"needs_user_key", true}}) &&
          !user_key::wants_device_key(json{{"available", false}, {"enabled", true}, {"needs_user_key", true}}) &&
          !user_key::wants_device_key(json{{"available", true}, {"enabled", true}, {"needs_user_key", false}}) &&
          !user_key::wants_device_key(json{{"status", "disabled"}}) && !user_key::wants_device_key(json::array()),
          "A disabled, unavailable or keyed mesh never asks");
    check(user_key::controller_port(json{{"controller_url", "http://127.0.0.1:4567"}}) == 4567, "The controller port comes from the status URL");
    check(user_key::controller_port(json{{"controller_url", "http://10.0.0.1:4567"}}) == 0 && user_key::controller_port(json{{"controller_url", "http://127.0.0.1:"}}) == 0 &&
          user_key::controller_port(json{{"controller_url", "http://127.0.0.1:70000"}}) == 0 && user_key::controller_port(json::object()) == 0,
          "A nonloopback or malformed controller URL yields no port");
#ifdef LEMONADE_WITH_NEXUS
    check(access::handoff_failure(LN_ERR_PARSE).first == 400 && access::handoff_failure(LN_ERR_NULL_ARG).first == 400, "A malformed envelope is 400");
    check(access::handoff_failure(LN_ERR_AUTH).first == 403 && access::handoff_failure(LN_ERR_REJECTED).first == 403, "An envelope that does not open is 403");
    check(access::handoff_failure(LN_ERR_INTERNAL).first == 500, "Other hand-off failures are 500");
    check(!user_key::store_failure_text(LN_ERR_AUTH).empty() && user_key::store_failure_text(LN_ERR_AUTH) != user_key::store_failure_text(LN_ERR_UNSUPPORTED),
          "Store failures have distinct texts");
#endif
}

#ifdef LEMONADE_WITH_NEXUS
// The hand-off routes in-process: this test is the user's session (ln_identity_generate
// stands in for the user store) and the management listener is lemond. The federation
// is a closed port, so every unlock ends as a retryable outage after the identity took
// effect; the public key in status shows which identity lemond holds.
void check_handoff(httplib::Client& management, const std::filesystem::path& root, const std::function<void(bool, const char*)>& check) {
    using IdentityPtr = std::unique_ptr<ln_identity_t, decltype(&ln_identity_destroy)>;
    const auto status = [&] {
        auto result = management.Get("/api/v1/nexus/status");
        return result && result->status == 200 ? json::parse(result->body) : json::object();
    };
    // Key names, not substrings: the algorithm name itself contains "sealed".
    const auto no_key_material = [](const json& value) {
        const auto text = value.dump();
        return text.find("\"private_key\"") == std::string::npos && text.find("\"seed\"") == std::string::npos;
    };
    const auto no_secret = [&](const json& value) {
        const auto text = value.dump();
        return no_key_material(value) && text.find("\"sealed\"") == std::string::npos && text.find("\"identity_envelope\"") == std::string::npos;
    };
    const auto post_unlock = [&](const json& body) {
        auto result = management.Post("/api/v1/nexus/key/unlock", body.dump(), "application/json");
        return std::make_pair(result ? result->status : 0, result ? json::parse(result->body, nullptr, false) : json());
    };
    const auto seal = [&](const ln_identity_t* identity, const json& request) {
        char* envelope = ln_identity_handoff_seal(identity, request.dump().c_str());
        std::string text = envelope ? envelope : "";
        ln_free(envelope);
        return text;
    };
    const auto pubkey_of = [](const ln_identity_t* identity) {
        char* key = ln_identity_pubkey(identity);
        std::string text = key ? key : "";
        ln_free(key);
        return text;
    };
    const auto begin = [&] {
        auto result = management.Get("/api/v1/nexus/key/handoff");
        check(result && result->status == 200, "key/handoff answers on the management listener");
        return result && result->status == 200 ? json::parse(result->body) : json::object();
    };

    const auto before = status();
    const auto system_key = before.value("public_key", std::string());
    check(before.at("identity_source") == "system" && before.at("needs_user_key") == false && !system_key.empty() && no_secret(before),
          "A system service reports its own key and no need for a user key");
    check(post_unlock(json{{"identity_envelope", 5}}).first == 400, "An envelope must be a string");
    check(post_unlock(json{{"identity_envelope", "x"}}).first == 409, "An envelope without an outstanding hand-off is refused");

    const auto request = begin();
    check(request.value("alg", "") == "x25519-sealedbox-v1" && request.contains("pubkey") && request.contains("nonce") &&
          request.contains("expires_at") && no_secret(request), "The hand-off request is the SDK's sealed-box request");

    IdentityPtr first(ln_identity_generate(), ln_identity_destroy);
    const auto first_key = pubkey_of(first.get());
    const auto envelope = seal(first.get(), request);
    check(!envelope.empty() && json::parse(envelope).value("alg", "") == "x25519-sealedbox-v1" && json::parse(envelope).contains("sealed") &&
          no_key_material(json::parse(envelope)), "The sender seals an envelope without plaintext");
    auto delivered = post_unlock(json{{"identity_envelope", envelope}, {"defer_join", true}});
    check(delivered.first == 503 && delivered.second.value("retryable", false), "A delivered key is adopted, then the unreachable federation is a retryable outage");
    auto after = status();
    check(after.value("public_key", "") == first_key && after.at("needs_user_key") == false && after.at("locked") == true && no_secret(after),
          "lemond holds the delivered identity");
    check(after.at("unlock") == "device_key", "A delivered key marks the device key-unlocked, so the supervisor retries by itself");
    check(post_unlock(json{{"identity_envelope", envelope}}).first == 409, "A second envelope for the same hand-off is refused");
    {
        std::ifstream input(root / "nexus/account.json");
        const auto account = json::parse(input, nullptr, false);
        std::ifstream archive_input(root / ("nexus/account-" + access::account_file_tag(system_key) + ".json"));
        const auto archive = json::parse(archive_input, nullptr, false);
        check(archive.is_object() && archive.value("public_key", "") == system_key && archive.value("unlock", "") == "device_key",
              "The system key's account is archived as it was");
        check(account.is_object() && account.value("public_key", "") == first_key && account.value("device_name", "") == "My laptop" &&
              archive.is_object() && account.value("server", "") == archive.value("server", "-") && !account.contains("user_id"),
              "The new key's account carries the device name and the server, and nothing of the other account");
    }

    const auto second_request = begin();
    check(post_unlock(json{{"identity_envelope", envelope}}).first == 403, "An envelope sealed for an earlier request does not open");
    check(post_unlock(json{{"identity_envelope", seal(first.get(), second_request)}}).first == 409, "A failed open still consumes the hand-off");
    check(status().value("public_key", "") == first_key, "A refused envelope leaves the identity as it was");

    IdentityPtr second(ln_identity_generate(), ln_identity_destroy);
    const auto second_key = pubkey_of(second.get());
    auto swapped = post_unlock(json{{"identity_envelope", seal(second.get(), begin())}, {"defer_join", true}});
    check(swapped.first == 503 && status().value("public_key", "") == second_key, "Another user's key replaces the active one");
    check(std::filesystem::exists(root / ("nexus/account-" + access::account_file_tag(first_key) + ".json")), "The replaced key's account is archived");

    const auto identity_path = lemon::utils::path_to_utf8(root / "nexus/identity.json");
    IdentityPtr system_identity(ln_identity_load(identity_path.c_str()), ln_identity_destroy);
    check(system_identity != nullptr && pubkey_of(system_identity.get()) == system_key, "The system key is still on disk");
    auto returned = post_unlock(json{{"identity_envelope", seal(system_identity.get(), begin())}, {"defer_join", true}});
    auto restored = status();
    check(returned.first == 503 && restored.value("public_key", "") == system_key && restored.at("registered") == true &&
          restored.at("device_name") == "My laptop", "A key that comes back gets its archived account");
    check(!std::filesystem::exists(root / ("nexus/account-" + access::account_file_tag(system_key) + ".json")), "A restored account leaves the archive");
    auto same = post_unlock(json{{"identity_envelope", seal(system_identity.get(), begin())}, {"defer_join", true}});
    check(same.first == 503 && status().value("public_key", "") == system_key, "The same key again changes nothing");
    check(no_secret(delivered.second) && no_secret(swapped.second) && no_secret(returned.second) && no_secret(status()),
          "No reply carries a seed, private key or envelope");

    // An account.json from before public_key was recorded (a device-key enrolment by an
    // earlier build) belongs to an unknown key. No key adopts it as its own, and a
    // delivered key is never refused for it: the federation's user_id decides at the
    // first authentication (nexus_access::account_match), which archives a stale file.
    const auto read_account = [&] {
        std::ifstream input(root / "nexus/account.json");
        return json::parse(input, nullptr, false);
    };
    management.Post("/api/v1/nexus/disable", "{}", "application/json");
    {
        auto legacy = read_account();
        legacy.erase("public_key");
        legacy["user_id"] = "legacy-user";
        std::ofstream output(root / "nexus/account.json");
        output << legacy.dump();
    }
    auto enabled = management.Post("/api/v1/nexus/enable", "{}", "application/json");
    check(enabled && enabled->status == 200 && json::parse(enabled->body).value("public_key", "") == system_key,
          "A system service adopts its key over a legacy account");
    const auto unbound = read_account();
    check(unbound.is_object() && !unbound.contains("public_key") && unbound.value("user_id", "") == "legacy-user",
          "A legacy account is not bound to a key before the federation names its user");
    IdentityPtr third(ln_identity_generate(), ln_identity_destroy);
    auto over_legacy = post_unlock(json{{"identity_envelope", seal(third.get(), begin())}, {"defer_join", true}});
    const auto after_legacy = read_account();
    check(over_legacy.first == 503 && over_legacy.second.value("retryable", false) && status().value("public_key", "") == pubkey_of(third.get()) &&
          after_legacy.is_object() && !after_legacy.contains("public_key") && after_legacy.value("user_id", "") == "legacy-user" &&
          after_legacy.value("device_name", "") == "My laptop", "A delivered key is never refused because of a stale account file");
    check(!std::filesystem::exists(root / "nexus/account-user-legacy-user.json"), "A legacy account is archived only once the federation names another user");
}

#ifdef __linux__
// User mode fails closed: without a usable keystore lemond has no identity, writes no
// identity file and refuses a key unlock. Linux only, and only where the SDK reports no
// keystore for this process: on macOS and Windows even the status call reaches the
// keychain or DPAPI, and a developer's real keystore is never touched from a test. The
// probe runs on its own thread with a time bound, so a Secret Service that does not
// answer cannot stall the test.
void check_user_mode_fails_closed(const std::filesystem::path& root, const std::function<void(bool, const char*)>& check) {
    auto probe = std::make_shared<std::promise<json>>();
    auto answer = probe->get_future();
    std::thread([probe] {
        char* out = nullptr;
        json store = ln_identity_user_status(&out) == LN_OK && out ? json::parse(out, nullptr, false) : json();
        ln_free(out);
        probe->set_value(std::move(store));
    }).detach();
    if (answer.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        std::cout << "Skipping the user-mode check: the keystore did not answer within 5 s\n";
        return;
    }
    const auto store = answer.get();
    if (!store.is_object() || store.value("backend", "") != "none") {
        std::cout << "Skipping the user-mode check: this process has a keystore (" << store.value("backend", "?") << ")\n";
        return;
    }
    const auto user_dir = std::filesystem::path(store.value("dir", std::string()));
    const bool stored_before = !user_dir.empty() && std::filesystem::exists(user_dir / "identity.v2.json");
    {
        lemon::NexusManager manager((root / "user-mode").string(), listener_policy, listener_policy, 0, nullptr, kUserRole);
        std::string error;
        check(manager.enable(error), "Enable succeeds without a usable keystore");
        const auto state = manager.status();
        check(state.at("identity_source") == "user" && state.at("needs_user_key") == true && state.at("locked") == true &&
              state.value("public_key", "x").empty(), "User mode without a keystore waits for a key");
        check(state.contains("identity_store") && state.at("identity_store").value("backend", "") == "none", "Status reports the SDK store state");
        check(!state.value("error", std::string()).empty(), "Status names the store failure");
        check(!std::filesystem::exists(root / "user-mode/nexus/identity.json"), "No identity file is written in user mode");
        httplib::Client management("127.0.0.1", std::stoi(state.at("controller_url").get<std::string>().substr(17)));
        const char* api_key = std::getenv("LEMONADE_API_KEY");
        if (api_key && *api_key) management.set_default_headers({{"Authorization", std::string("Bearer ") + api_key}});
        auto unlock = management.Post("/api/v1/nexus/key/unlock", "{}", "application/json");
        check(unlock && unlock->status == 409, "A key unlock without an identity is refused");
        auto challenge = management.Get("/api/v1/nexus/passkey/challenge");
        check(challenge && challenge->status == 409, "A passkey challenge without an identity is refused");
        manager.disable();
        manager.stop();
    }
    check((!user_dir.empty() && std::filesystem::exists(user_dir / "identity.v2.json")) == stored_before, "The user store is left as it was");
}
#endif

// The SDK's C ABI reports a federation 401 and 403 alike as LN_ERR_AUTH, and no
// transport, a timeout and a 5xx alike as LN_ERR_CONNECT.
void check_sdk_failures(const std::function<void(bool, const char*)>& check) {
    const auto limited = access::sdk_failure(LN_ERR_REJECTED, json{{"error", "rate limited"}}, 409);
    check(limited.status == 429 && limited.retryable, "Rate limiting is retryable, not lost account membership");
    const auto refused = access::sdk_failure(LN_ERR_AUTH, json{{"error", "session expired"}}, 409);
    check(refused.status == 403 && !refused.retryable && refused.message == "session expired", "A refused session keeps its auth status");
    const auto outage = access::sdk_failure(LN_ERR_CONNECT, json{{"error", "connection error"}}, 409);
    check(outage.status == 503 && outage.retryable && outage.message == "connection error", "A connection failure, timeout or 5xx is a retryable 503");
    const auto directory = access::sdk_failure(LN_ERR_REJECTED, json{{"error", "no verified Tier 1 server is reachable"}}, 409);
    check(directory.status == 503 && directory.retryable, "An unreachable Tier 1 directory is a retryable 503");
    const auto unverified = access::sdk_failure(LN_ERR_AUTH, json{{"error", "no server returned a verifiable authority chain"}}, 409);
    check(unverified.status == 503 && unverified.retryable, "An auth attempt that reached no server is a retryable 503");
    check(access::sdk_failure(LN_ERR_REJECTED, json{{"error", "invalid move"}}, 409).status == 409, "A refused request keeps the caller's status");
    check(access::sdk_failure(LN_ERR_NOT_FOUND, json(), 409).status == 404, "A missing node is 404");
    const auto internal = access::sdk_failure(LN_ERR_INTERNAL, json(), 409);
    check(internal.status == 502 && !internal.retryable && internal.message.find("SDK error") != std::string::npos, "Other failures are 502 and name the code");
}
#endif
}

int main() {
    int failures = 0;
    const auto check = [&](bool condition, const char* description) {
        if (!condition) { std::cerr << description << '\n'; ++failures; }
    };
    const auto root = std::filesystem::temp_directory_path() / ("lemonade-nexus-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto cleanup = [&] { std::error_code ec; std::filesystem::remove_all(root, ec); };
    // Point the SDK at a closed loopback port: the manager's supervisor must never reach a
    // real federation from a test, and every federation call fails fast and locally.
    set_environment("LEMONADE_NEXUS_SERVER", "127.0.0.1");
    set_environment("LEMONADE_NEXUS_PORT", "9");
    try {
        const json group = {{"id", "root"}, {"parent_id", "outside"}, {"type", "customer"}, {"hostname", "Team"},
            {"assignments", json::array({{{"management_pubkey", "self"}, {"permissions", json::array({"admin"})}}})}};
        const json child = {{"id", "child"}, {"parent_id", "root"}, {"type", "customer"}, {"wrapped_mgmt_privkey", "must-not-leave-server"}};
        const json leaf = {{"id", "device"}, {"parent_id", "child"}, {"type", "endpoint"}};
        const json tree = {{"root", group}, {"child", child}, {"device", leaf}};
        const auto fetch = [&](const std::string& id) { return tree.value(id, json{{"id", id}, {"parent_id", ""}, {"type", "customer"}}); };
        check(access::tree_path("device", "root", fetch).size() == 3, "Recursive tree paths retain every group ancestor");
        check(throws([&] { access::tree_path("outside", "root", fetch); }), "Tree reads cannot escape the current group");
        check(throws([&] { access::tree_path("../root", "root", fetch); }), "Tree IDs cannot contain path traversal");
        check(throws([&] { access::tree_path("cycle", "root", [](const auto& id) { return json{{"id", id}, {"parent_id", id}, {"type", "customer"}}; }); }), "Cyclic trees fail without looping");
        check(throws([&] { access::tree_path("child", "root", [](const auto& id) { return json{{"id", id}, {"parent_id", "root"}, {"type", "endpoint"}}; }); }), "Device nodes cannot contain children");
        check(throws([&] { access::tree_path("root", "root", [](const auto&) { return json{{"id", "other"}}; }); }), "Tree replies must match the requested identity");
        check(access::tree_path("child", std::vector<std::string>{"elsewhere", "child"}, fetch).size() == 1, "A membership group is a tree root");
        check(access::tree_path("device", std::vector<std::string>{"elsewhere", "root"}, fetch).size() == 3, "Several roots still keep the whole path");
        check(throws([&] { access::tree_path("outside", std::vector<std::string>{"elsewhere", "root"}, fetch); }), "Tree reads cannot escape every membership");
        check(access::invite_permissions(nullptr).is_null(), "An invite without permissions uses the federation default");
        check(access::invite_permissions(json::array({"connect_private", "read", "read"})) == json::array({"read", "connect_private"}), "Invite permissions are ordered and unique");
        check(throws([&] { access::invite_permissions(json::array({"admin"})); }), "An invite cannot grant admin");
        check(throws([&] { access::invite_permissions(json::array({"delete_node"})); }), "An invite grants only invite permissions");
        check(throws([&] { access::invite_permissions(json::array()); }) && throws([&] { access::invite_permissions("read"); }), "Invite permissions are a nonempty list");
        check(access::invite_code("12345678") && access::invite_code("lnk_" + std::string(64, 'a')), "Invite codes and full tokens are accepted");
        check(!access::invite_code("1234567") && !access::invite_code("lnk_" + std::string(64, 'G')) && !access::invite_code(""), "Malformed invite codes are refused");
        const auto readonly = access::tree_entry(child, "self");
        check(!readonly.at("can_add_group") && !readonly.at("can_rename") && !readonly.at("can_manage"), "Read-only groups have no mutation capabilities");
        check(!readonly.contains("wrapped_mgmt_privkey"), "Tree responses omit wrapped private material");
        const auto admin_group = access::tree_entry(group, "self");
        check(admin_group.at("can_add_group") && admin_group.at("can_rename") && admin_group.at("can_manage"), "Group admins can manage known assignments");
        const auto inherited = access::tree_entry(child, "self", true);
        check(inherited.at("can_add_group") && inherited.at("can_rename") && !inherited.at("can_manage"), "Ancestor admins cannot edit an undisclosed assignment list");
        check(!access::tree_entry(leaf, "self", true).at("can_add_group"), "Nodes remain leaves even for admins");
        const json verified_members = json::array({{{"node_id", "device"}, {"permissions_known", true}, {"is_admin", true}, {"session_token", "private"}}});
        const auto owner_account = access::account_summary(verified_members, "cluster", "device", "identity");
        check(owner_account.at("role") == "admin" && owner_account.at("id") == "cluster", "Account role and identity come from verified group membership");
        check(!owner_account.contains("session_token"), "Account summary exposes no SDK credentials");
        auto ordinary_members = verified_members;
        ordinary_members[0]["is_admin"] = false;
        check(access::account_summary(ordinary_members, "cluster", "device", "identity").at("role") == "user", "Joining an account does not imply administrator access");
        ordinary_members[0]["permissions"] = access::role_permissions("moderator");
        check(access::account_summary(ordinary_members, "cluster", "device", "identity").at("role") == "moderator", "The verified moderator preset is reported as moderator");
        ordinary_members[0]["permissions"].push_back("svc:future_service");
        check(access::account_summary(ordinary_members, "cluster", "device", "identity").at("role") == "moderator", "Additional grants do not hide a complete moderator preset");
        ordinary_members[0]["permissions"] = json::array({"read", "write", "add_child", "connect_private", "svc:inference"});
        check(access::account_summary(ordinary_members, "cluster", "device", "identity").at("role") == "user", "Partial grants do not imply the moderator preset");
        ordinary_members[0]["permissions"] = access::role_permissions("moderator");
        ordinary_members[0]["is_admin"] = true;
        check(access::account_summary(ordinary_members, "cluster", "device", "identity").at("role") == "admin", "Administrator status takes precedence over moderator grants");
        ordinary_members[0]["is_admin"] = false;
        check(throws([&] { access::account_summary(verified_members, "cluster", "missing", "identity"); }), "Account access requires the current device to be a group member");
        check(throws([&] { access::account_summary(verified_members, "", "device", "identity"); }), "An account requires a group identity");
        check(throws([&] { access::account_summary(verified_members, "cluster", "device", ""); }), "An account requires an authenticated identity");
        ordinary_members[0]["permissions_known"] = false;
        check(throws([&] { access::account_summary(ordinary_members, "cluster", "device", "identity"); }), "Unknown group permissions cannot produce an account role");
        const json profile = {{"group_id", "cluster"}, {"revision", 1}, {"image", nullptr}, {"can_write", true}};
        check(access::account_avatar(profile, "cluster").at("state") == "ready", "Account image metadata is bound to the verified group");
        auto waiting_profile = profile;
        waiting_profile["pending"] = true;
        check(access::account_avatar(waiting_profile, "cluster").at("pending"), "An offline device image stays pending without becoming a removal");
        waiting_profile["pending"] = "yes";
        check(throws([&] { access::account_avatar(waiting_profile, "cluster"); }), "Pending image state requires a boolean");
        check(throws([&] { access::account_avatar(profile, "other"); }), "Another account's profile image is rejected");
        auto invalid_profile = profile;
        invalid_profile["image"] = "https://external.example/photo.png";
        check(throws([&] { access::account_avatar(invalid_profile, "cluster"); }), "Profile images cannot load external tracking URLs");
        invalid_profile = profile; invalid_profile["revision"] = -1;
        check(throws([&] { access::account_avatar(invalid_profile, "cluster"); }), "Negative image revisions are rejected");
        // The saved Tier 1 epoch feeds ln_set_genesis_key as the lowest epoch a restart
        // accepts, so it follows a verified advance and never goes back.
        check(access::newer_tier1_epoch(json{{"epoch", 7}, {"members", json::array()}}, 3) == 7, "A verified membership that advanced is saved");
        check(access::newer_tier1_epoch(json{{"epoch", 3}}, 3) == 0 && access::newer_tier1_epoch(json{{"epoch", 2}}, 3) == 0, "An equal or older membership is not saved");
        check(access::newer_tier1_epoch(json{{"epoch", -1}}, 0) == 0 && access::newer_tier1_epoch(json{{"epoch", "7"}}, 0) == 0 &&
              access::newer_tier1_epoch(json::array(), 0) == 0, "A malformed membership saves nothing");
        // The SDK drops a session whose Tier 1 server left only when it cannot move the
        // session itself; the mesh can stay up, and the refusal texts say nothing.
        check(access::session_dropped(json{{"needs_rejoin", true}, {"peer_count", 0}}), "The SDK's rejoin flag starts a join");
        check(!access::session_dropped(json{{"needs_rejoin", false}}) && !access::session_dropped(json::object()) &&
              !access::session_dropped(json::array()), "Without the flag, or with a malformed status, the session stays");
        check_access_rules(check);
        check_identity_rules(check);
        check_role_presets(check);
        check_service_catalog(check);
        check_peer_headers(check);
#ifdef LEMONADE_WITH_NEXUS
        check_sdk_failures(check);
#endif
        check_route_access(check);
        check_private_management(root, check);
        check_relay_stream(check);
        check_mesh_ingress_routes(check);
        check_listener_isolation(root, check);
        check_status_fields(root, check);
#if defined(LEMONADE_WITH_NEXUS) && defined(__linux__)
        check_user_mode_fails_closed(root, check);
#endif
        {
            lemon::NexusManager manager(root.string(), listener_policy, listener_policy, 0, nullptr, kSystemRole);
            httplib::Server server;
            manager.register_routes(server);
            const int port = server.bind_to_any_port("127.0.0.1");
            if (port < 0) { cleanup(); return 1; }
            std::thread worker([&] { server.listen_after_bind(); });
            server.wait_until_ready();
            httplib::Client client("127.0.0.1", port);
            const char* api_key = std::getenv("LEMONADE_API_KEY");
            if (api_key && *api_key) client.set_default_headers({{"Authorization", std::string("Bearer ") + api_key}});
            const int control_port = std::stoi(manager.status().at("controller_url").get<std::string>().substr(17));
            httplib::Client forbidden("127.0.0.1", control_port);
            auto denied = forbidden.Get("/api/v1/nexus/status", httplib::Headers{{"Origin", "https://untrusted.example"}});
            check(denied && denied->status == 403, "Management rejects untrusted origins");
            if (api_key && *api_key) {
                auto unauthenticated = forbidden.Get("/api/v1/nexus/status");
                check(unauthenticated && unauthenticated->status == 401, "Management enforces API key");
            }
            auto redirect = client.Post("/api/v1/nexus/enable", "{}", "application/json");
            check(redirect && redirect->status == 307, "Public listener redirects management away from mesh service");
            for (const auto* path : {"members/peer/role", "members/peer/permissions", "devices/peer/access"}) {
                auto moved = client.Post(std::string("/api/v1/nexus/") + path, "{}", "application/json");
                check(moved && moved->status == 307 && moved->get_header_value("Location").find("127.0.0.1:" + std::to_string(control_port)) != std::string::npos,
                    "Public listener redirects access management to the loopback listener");
            }
            auto members_redirect = client.Get("/api/v1/nexus/members");
            check(members_redirect && members_redirect->status == 307, "Public listener does not serve the member list");
            auto handoff_redirect = client.Get("/api/v1/nexus/key/handoff");
            check(handoff_redirect && handoff_redirect->status == 307, "Public listener redirects the hand-off to the management listener");
            auto denied_access = forbidden.Post("/api/v1/nexus/members/peer/role", httplib::Headers{{"Origin", "https://untrusted.example"}}, R"({"role":"admin"})", "application/json");
            check(denied_access && denied_access->status == 403, "Access management rejects untrusted origins");
            httplib::Client management("127.0.0.1", control_port);
            if (api_key && *api_key) management.set_default_headers({{"Authorization", std::string("Bearer ") + api_key}});
            const auto request = [&](const std::string& path, const std::string& body, int expected) {
                auto result = management.Post("/api/v1/nexus/" + path, body, "application/json");
                check(result && result->status == expected, path.c_str());
                return result ? nlohmann::json::parse(result->body) : nlohmann::json::object();
            };
            for (const auto* prefix : {"/api/v0/", "/api/v1/", "/v0/", "/v1/", "/api/"}) {
                auto result = client.Get(std::string(prefix) + "nexus/status");
                check(result && result->status == 200, "Status prefix registered");
                if (result) check(nlohmann::json::parse(result->body).at("locked") == true, "Starts locked");
                for (const auto* path : {"tree", "tree/root/children", "account", "memberships"}) {
                    const auto redirected = client.Get(std::string(prefix) + "nexus/" + path);
                    check(redirected && redirected->status == 307, "Tree reads use the management listener on every prefix");
                }
                for (const auto* path : {"tree/root/groups", "tree/root/rename", "tree/root/permissions", "account/avatar",
                                         "tree/root/invites", "tree/device/move", "invites/accept", "memberships/root/leave", "account/delete"}) {
                    const auto redirected = client.Post(std::string(prefix) + "nexus/" + path, "{}", "application/json");
                    check(redirected && redirected->status == 307, "Tree writes use the management listener on every prefix");
                }
            }
            request("enable", "[]", 400);
            request("enable", "{", 400);
#ifdef LEMONADE_WITH_NEXUS
            request("selection", R"({"target":"local"})", 409);
            auto disabled_handoff = management.Get("/api/v1/nexus/key/handoff");
            check(disabled_handoff && disabled_handoff->status == 409, "A hand-off needs the mesh enabled");
#endif
#ifdef LEMONADE_WITH_NEXUS
            auto enabled = request("enable", "{}", 200);
            request("selection", R"({"target":"local"})", 409);
            request("selection", R"({"target":5})", 400);
            check(enabled.contains("mesh_port") && enabled.contains("genesis_key_set"), "Status reports the SDK mesh port and Genesis key state");
            check(enabled.at("available") && enabled.at("enabled") && enabled.at("locked"), "SDK enabled without unlocking");
            const bool system_identity = enabled.value("identity_source", "") == "system";
            check(system_identity == std::filesystem::exists(root / "nexus/identity.json"), "Only a system service persists an identity file");
            check(enabled.contains("needs_user_key") && enabled.contains("public_key") &&
                  (enabled.at("identity_source") == "user") == enabled.contains("identity_store"), "Status reports the identity source");
            if (!system_identity && enabled.value("needs_user_key", false)) {
                std::cout << "Skipping the key unlock checks: this process has no user device key ("
                          << enabled.value("error", std::string()) << ")\n";
            }
            request("join", R"({"code":"12345678"})", 423);
            request("egress", R"({"node_id":"another-device"})", 423);
            request("invites", R"({"device_name":"New device"})", 423);
            request("device/check", R"({"name":"Work PC"})", 423);
            request("passkey/unlock", R"({"passkey_assertion":{"credential_id":"wrong"}})", 403);
            request("members/peer/role", R"({"role":"admin"})", 423);
            request("members/peer/role", R"({"role":"moderator"})", 423);
            request("members/peer/permissions", R"({"permissions":["read"]})", 423);
            request("devices/peer/access", R"({"member_node_id":"other","see":true,"connect":false})", 423);
            request("tree/root/groups", R"({"name":"Team"})", 423);
            request("tree/root/rename", R"({"name":"Team"})", 423);
            request("tree/root/permissions", R"({"member_node_id":"peer","admin":true,"permissions":["read"]})", 423);
            request("account/avatar", R"({"account_id":"cluster","image":null,"revision":0})", 423);
            request("tree/root/invites", R"({"permissions":["read"]})", 423);
            request("tree/device/move", R"({"parent_id":"root"})", 423);
            request("invites/accept", R"({"code":"12345678"})", 423);
            request("memberships/root/leave", "{}", 423);
            request("account/delete", R"({"account_id":"cluster","confirm":"delete"})", 423);
            auto locked_memberships = management.Get("/api/v1/nexus/memberships");
            check(locked_memberships && locked_memberships->status == 423, "Memberships require an unlocked network");
            auto locked_tree = management.Get("/api/v1/nexus/tree");
            check(locked_tree && locked_tree->status == 423, "Tree requires an unlocked network");
            auto locked_account = management.Get("/api/v1/nexus/account");
            check(locked_account && locked_account->status == 423, "Account details require passkey unlock");
            auto locked_members = management.Get("/api/v1/nexus/members");
            check(locked_members && locked_members->status == 423, "Member list requires an unlocked network");
            auto locked_access = management.Get("/api/v1/nexus/devices/peer/access");
            check(locked_access && locked_access->status == 423, "Device access requires an unlocked network");
            request("device", "{}", 400);
            request("device", R"({"device_name":""})", 400);
            {
                auto initial = management.Get("/api/v1/nexus/status");
                const auto first_name = initial && initial->status == 200
                    ? nlohmann::json::parse(initial->body).value("device_name", std::string{}) : std::string{};
                check(!first_name.empty() && first_name != "My Lemonade device" && first_name.size() <= 128,
                      "An unnamed device carries the machine's host name");
            }
            auto named = request("device", R"({"device_name":"My laptop"})", 200);
            check(named.at("device_name") == "My laptop", "Device name updated");
            request("lock", "{}", 200);
            // A device that unlocks with its device key reports itself registered, refuses the
            // passkey routes, and stays locked after an explicit lock even though the supervisor
            // would unlock it again by itself.
            request("disable", "{}", 200);
            {
                std::ifstream saved_input(root / "nexus/account.json");
                auto saved = nlohmann::json::parse(saved_input, nullptr, false);
                saved_input.close();
                if (!saved.is_object()) saved = nlohmann::json::object();
                saved["unlock"] = "device_key";
                std::ofstream saved_output(root / "nexus/account.json");
                saved_output << saved.dump();
            }
            auto keyed = request("enable", "{}", 200);
            check(keyed.at("registered") == true && keyed.at("unlock") == "device_key", "Device key unlock counts as registered");
            request("passkey/register", R"({"credential_id":"c","public_key_x":"x","public_key_y":"y"})", 409);
            request("passkey/unlock", R"({"passkey_assertion":{"credential_id":"c"}})", 409);
            request("lock", "{}", 200);
            std::this_thread::sleep_for(std::chrono::seconds(6));
            auto held = management.Get("/api/v1/nexus/status");
            check(held && held->status == 200 && nlohmann::json::parse(held->body).at("locked") == true, "An explicit lock holds against key unlock");
            // Without a configured server, enable discovers one by DNS. A domain that cannot
            // resolve fails with the region named, and a remembered server needs no network.
            request("disable", "{}", 200);
            set_environment("LEMONADE_NEXUS_SERVER", "");
            set_environment("LEMONADE_NEXUS_DOMAIN", "nexus.invalid");
            auto undiscovered = request("enable", "{}", 503);
            check(undiscovered.value("error", std::string{}).find("us-west.seip.nexus.invalid") != std::string::npos,
                  "A discovery failure names the region");
            {
                std::ifstream saved_input(root / "nexus/account.json");
                auto saved = nlohmann::json::parse(saved_input, nullptr, false);
                saved_input.close();
                if (!saved.is_object()) saved = nlohmann::json::object();
                saved["server"] = "sn01.us-west.seip.nexus.invalid";
                std::ofstream saved_output(root / "nexus/account.json");
                saved_output << saved.dump();
            }
            auto remembered = request("enable", "{}", 200);
            check(remembered.at("server") == "sn01.us-west.seip.nexus.invalid", "A remembered server skips discovery");
            set_environment("LEMONADE_NEXUS_DOMAIN", "");
            set_environment("LEMONADE_NEXUS_SERVER", "127.0.0.1");
            // With a pinned Genesis key, the closed port is a federation that cannot be
            // reached: an outage, not a refusal. The route answers 503 with retryable and
            // the device state stays as it was.
            request("disable", "{}", 200);
            set_environment("LEMONADE_NEXUS_GENESIS_PUBKEY", std::string(43, 'A') + "=");
            auto pinned = request("enable", "{}", 200);
            if (pinned.value("needs_user_key", false)) {
                request("key/unlock", "{}", 409);
            } else {
                auto outage = request("key/unlock", "{}", 503);
                check(outage.value("retryable", false) && !outage.value("error", std::string{}).empty(), "An unreachable federation is a retryable 503");
                auto after_outage = management.Get("/api/v1/nexus/status");
                check(after_outage && after_outage->status == 200 && nlohmann::json::parse(after_outage->body).at("locked") == true,
                      "An outage leaves the lock state as it was");
            }
            if (pinned.value("identity_source", "") == "system") check_handoff(management, root, check);
            else std::cout << "Skipping the hand-off round trip: it needs the system identity file\n";
            set_environment("LEMONADE_NEXUS_GENESIS_PUBKEY", "");
#else
            request("enable", "{}", 503);
            request("selection", R"({"target":"local"})", 503);
            request("members/peer/role", R"({"role":"admin"})", 503);
#endif
            auto events = management.Get("/api/v1/nexus/events?after=0&once=1");
            check(events && events->status == 200 && events->get_header_value("Content-Type") == "text/event-stream", "Events use SSE format");
            server.stop(); worker.join(); manager.stop();
        }
#ifdef LEMONADE_WITH_NEXUS
        {
            lemon::NexusManager restored(root.string(), listener_policy, listener_policy, 0, nullptr, kSystemRole);
            const auto status = restored.status();
            check(status.at("enabled") && status.at("locked") && !status.at("mesh_up"), "Restarts enabled and locked");
            check(status.at("device_name") == "My laptop", "Name survives restart");
            restored.disable();
        }
        {
            lemon::NexusManager disabled(root.string(), listener_policy, listener_policy, 0, nullptr, kSystemRole);
            check(!disabled.status().at("enabled"), "Disable survives restart");
        }
#endif
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; ++failures; }
    cleanup();
    return failures ? 1 : 0;
}
