#include "lemon/nexus_manager.h"
#include "lemon/server/route_registry.h"
#include "lemon/utils/origin_utils.h"
#include "lemon/utils/path_utils.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#ifdef LEMONADE_WITH_NEXUS
#include <LemonadeNexusSDK/lemonade_nexus.h>
#endif

namespace {
using nlohmann::json;
namespace access = lemon::nexus_access;

bool throws(const std::function<void()>& action) {
    try { action(); } catch (const std::runtime_error&) { return true; }
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

// The address this host uses toward a nonlocal network, so a test client can reach a test
// listener without arriving from loopback. Empty when the host has no such route.
std::string non_loopback_ipv4() {
    const auto sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) return "";
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(9);
    inet_pton(AF_INET, "192.0.2.1", &target.sin_addr);
    std::string result;
    if (::connect(sock, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0) {
        sockaddr_in local{};
        socklen_t length = sizeof(local);
        char text[INET_ADDRSTRLEN] = {};
        if (::getsockname(sock, reinterpret_cast<sockaddr*>(&local), &length) == 0 &&
            inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text))) {
            const std::string address = text;
            if (address != "0.0.0.0" && address.rfind("127.", 0) != 0) result = address;
        }
    }
    httplib::detail::close_socket(sock);
    return result;
}

void check_route_access(const std::function<void(bool, const char*)>& check) {
    using lemon::NexusListener;
    const auto access = [](NexusListener listener, bool loopback, const std::string& operation,
                           const std::string& authorization, const std::string& key, const std::string& admin = "") {
        return access::route_access(listener, loopback, operation, authorization, key, admin);
    };
    check(access::constant_time_equals("secret", "secret"), "Equal secrets match");
    check(!access::constant_time_equals("secre", "secret") && !access::constant_time_equals("secrets", "secret"), "Length differences never match");
    check(!access::constant_time_equals("secreT", "secret") && !access::constant_time_equals("", "secret"), "Different secrets never match");
    check(!access::bearer_matches("Bearer ", "", ""), "An empty bearer never matches an unset key");
    check(!access::bearer_matches("Basic secret", "secret", ""), "Only bearer credentials are accepted");

    for (const auto* operation : {"status", "members", "enable", "passkey/unlock", "members/role", "devices/access", "events", "tree", "tree/children", "tree/groups", "tree/rename", "tree/permissions", "account", "account/avatar"}) {
        check(access(NexusListener::MeshIngress, true, operation, "Bearer key", "key").first == 403, "Mesh ingress refuses every Nexus route");
    }
    check(access(NexusListener::Public, true, "status", "", "").first == 0, "Local clients read status on the public listener");
    check(access(NexusListener::Public, true, "members", "Bearer key", "key").first == 307, "Local clients are redirected to the management listener");
    check(access(NexusListener::Management, true, "members", "", "").first == 0, "The management listener serves loopback clients");
    check(access(NexusListener::Management, false, "members", "Bearer key", "key").first == 403, "The management listener refuses other clients");

    const auto off = access(NexusListener::Public, false, "members", "Bearer key", "");
    check(off.first == 403 && off.second.find("LEMONADE_API_KEY") != std::string::npos, "Remote management is off without an API key");
    check(access(NexusListener::Public, false, "status", "Bearer admin", "", "admin").first == 403, "An admin key alone does not turn on remote management");
    check(access(NexusListener::Public, false, "members", "", "key").first == 401, "Remote management requires a bearer key");
    check(access(NexusListener::Public, false, "members", "Bearer wrong", "key").first == 401, "Remote management rejects a wrong key");
    check(access(NexusListener::Public, false, "members", "Bearer keyx", "key").first == 401, "Remote management rejects a longer key");
    check(access(NexusListener::Public, false, "members", "Bearer admin", "key", "admin").first == 0, "The admin key also authorizes remote management");
    for (const auto* operation : {"status", "join/status", "devices", "members", "members/role", "members/permissions",
                                  "devices/access", "devices/remove", "invites", "invites/cancel", "join", "tree", "tree/children", "tree/groups", "tree/rename", "tree/permissions",
                                  "tree/invites", "tree/move", "invites/accept", "memberships", "memberships/leave"}) {
        check(access(NexusListener::Public, false, operation, "Bearer key", "key").first == 0, "Group operations are available remotely");
    }
    for (const auto* operation : {"tree/invites", "tree/move", "invites/accept", "memberships", "memberships/leave", "account/delete"}) {
        check(access(NexusListener::MeshIngress, true, operation, "Bearer key", "key").first == 403, "Mesh ingress refuses invite, membership, move and deletion routes");
    }
    for (const auto* operation : {"passkey/challenge", "passkey/register", "passkey/unlock", "key/unlock", "enable", "disable", "lock", "device", "events", "egress", "account", "account/delete"}) {
        check(access(NexusListener::Public, false, operation, "Bearer key", "key").first == 403, "Local device operations stay local");
        check(access(NexusListener::MeshIngress, true, operation, "Bearer key", "key").first == 403, "Mesh ingress refuses local device operations");
    }
    check(access(NexusListener::MeshIngress, true, "relay", "Bearer key", "key").first == 403, "Mesh peers cannot use the relay");
    check(access(NexusListener::Public, true, "relay", "", "").first == 0, "Local clients use the relay under the local rules");
    check(access(NexusListener::Public, false, "relay", "Bearer key", "").first == 403, "Remote relay is off without an API key");
    check(access(NexusListener::Public, false, "relay", "Bearer wrong", "key").first == 401, "Remote relay rejects a wrong key");
    check(access(NexusListener::Public, false, "relay", "Bearer key", "key").first == 0, "Remote relay accepts the API key");

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
class StubRoute : public lemon::ApiRoute {
public:
    StubRoute(lemon::ServerContext& ctx, std::string id, std::string path, lemon::Prefixes prefixes)
        : lemon::ApiRoute(ctx), id_(std::move(id)), path_(std::move(path)), prefixes_(prefixes) {}
    lemon::RouteSpec spec() const override {
        lemon::RouteSpec s;
        s.id = id_;
        s.methods = {"GET"};
        s.paths = {path_};
        s.prefixes = prefixes_;
        return s;
    }
    void handle(lemon::RouteRequest&, httplib::Response& res) override { res.set_content("{}", "application/json"); }

private:
    std::string id_, path_;
    lemon::Prefixes prefixes_;
};

// server.cpp installs the route registry on the mesh ingress listener without the
// /internal/* control routes: a peer arrives on loopback, where the admin key is usually unset.
void check_mesh_ingress_routes(const std::function<void(bool, const char*)>& check) {
    lemon::ServerContext ctx;
    lemon::RouteRegistry registry;
    registry.add(std::make_unique<StubRoute>(ctx, "test.health", "health", lemon::Prefixes::Quad));
    registry.add(std::make_unique<StubRoute>(ctx, "test.shutdown", "shutdown", lemon::Prefixes::Internal));

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

void check_listener_isolation(const std::filesystem::path& root, const std::function<void(bool, const char*)>& check) {
    const auto saved_key = lemon::utils::get_environment_variable_utf8("LEMONADE_API_KEY");
    const auto saved_admin = lemon::utils::get_environment_variable_utf8("LEMONADE_ADMIN_API_KEY");
    set_environment("LEMONADE_ADMIN_API_KEY", "");
    set_environment("LEMONADE_API_KEY", "remote-test-key");
    {
        lemon::NexusManager manager((root / "isolation").string(), listener_policy, listener_policy, 0);
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
        mesh.stop(); mesh_worker.join();

        const auto address = non_loopback_ipv4();
        if (address.empty()) {
            std::cout << "Skipping remote listener checks: no nonloopback IPv4 address\n";
        } else {
            httplib::Server server;
            manager.register_routes(server);
            const int port = server.bind_to_any_port(address);
            check(port > 0, "Remote test listener binds");
            std::thread worker([&] { server.listen_after_bind(); });
            server.wait_until_ready();
            httplib::Client remote(address, port);
            auto missing = remote.Get("/api/v1/nexus/members");
            check(missing && missing->status == 401, "Remote members need the API key");
            auto wrong = remote.Get("/api/v1/nexus/members", httplib::Headers{{"Authorization", "Bearer wrong"}});
            check(wrong && wrong->status == 401, "Remote members reject a wrong key");
            const httplib::Headers valid = {{"Authorization", "Bearer remote-test-key"}};
            auto status = remote.Get("/api/v1/nexus/status", valid);
            check(status && status->status == 200, "Remote status is available with the API key");
            if (status && status->status == 200) {
                const auto body = nlohmann::json::parse(status->body);
                check(body.at("remote_management") == true && !body.contains("controller_url"), "Remote status hides the loopback controller");
            }
            auto members = remote.Get("/api/v1/nexus/members", valid);
#ifdef LEMONADE_WITH_NEXUS
            check(members && members->status == 409, "Remote members reach the network controller");
#else
            check(members && members->status == 503, "Remote members reach the network controller");
#endif
            auto unlock = remote.Post("/api/v1/nexus/passkey/unlock", valid, "{}", "application/json");
            check(unlock && unlock->status == 403, "Remote clients cannot use the passkey");
            auto relayed = remote.Post("/api/v1/nexus/peers/gpu/api/v1/chat/completions", valid, "{}", "application/json");
#ifdef LEMONADE_WITH_NEXUS
            check(relayed && relayed->status == 409, "Remote relay needs the mesh turned on");
#else
            check(relayed && relayed->status == 503, "Remote relay needs the Nexus SDK");
#endif
            auto unkeyed_relay = remote.Post("/api/v1/nexus/peers/gpu/api/v1/chat/completions", "{}", "application/json");
            check(unkeyed_relay && unkeyed_relay->status == 401, "Remote relay needs the API key");
            auto enable_remote = remote.Post("/api/v1/nexus/enable", valid, "{}", "application/json");
            check(enable_remote && enable_remote->status == 403 && !manager.status().at("enabled"), "Remote clients cannot enable Nexus");
            set_environment("LEMONADE_API_KEY", "");
            auto refused = remote.Get("/api/v1/nexus/members", valid);
            check(refused && refused->status == 403 && refused->body.find("LEMONADE_API_KEY") != std::string::npos,
                "Remote management is refused without a configured API key");
            auto refused_relay = remote.Post("/api/v1/nexus/peers/gpu/api/v1/chat/completions", valid, "{}", "application/json");
            check(refused_relay && refused_relay->status == 403, "Remote relay is refused without a configured API key");
            server.stop(); worker.join();
        }
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

    check(access::role_permissions(json::array({"write", "read"}), true) == json::array({"read", "write", "admin"}), "Promotion adds admin");
    check(access::role_permissions(json::array({"admin", "write"}), false) == json::array({"read", "write"}), "Demotion keeps read");
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

#ifdef LEMONADE_WITH_NEXUS
// The SDK's C ABI reports a federation 401 and 403 alike as LN_ERR_AUTH, and no
// transport, a timeout and a 5xx alike as LN_ERR_CONNECT.
void check_sdk_failures(const std::function<void(bool, const char*)>& check) {
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
        check_access_rules(check);
#ifdef LEMONADE_WITH_NEXUS
        check_sdk_failures(check);
#endif
        check_route_access(check);
        check_relay_stream(check);
        check_mesh_ingress_routes(check);
        check_listener_isolation(root, check);
        {
            lemon::NexusManager manager(root.string(), listener_policy, listener_policy, 0);
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
#endif
#ifdef LEMONADE_WITH_NEXUS
            auto enabled = request("enable", "{}", 200);
            request("selection", R"({"target":"local"})", 409);
            request("selection", R"({"target":5})", 400);
            check(enabled.contains("mesh_port") && enabled.contains("genesis_key_set"), "Status reports the SDK mesh port and Genesis key state");
            check(enabled.at("available") && enabled.at("enabled") && enabled.at("locked"), "SDK enabled without unlocking");
            check(std::filesystem::exists(root / "nexus/identity.json"), "Device identity persisted");
            request("join", R"({"code":"12345678"})", 423);
            request("egress", R"({"node_id":"another-device"})", 423);
            request("invites", R"({"device_name":"New device"})", 423);
            request("passkey/unlock", R"({"passkey_assertion":{"credential_id":"wrong"}})", 403);
            request("members/peer/role", R"({"role":"admin"})", 423);
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
            request("enable", "{}", 200);
            auto outage = request("key/unlock", "{}", 503);
            check(outage.value("retryable", false) && !outage.value("error", std::string{}).empty(), "An unreachable federation is a retryable 503");
            auto after_outage = management.Get("/api/v1/nexus/status");
            check(after_outage && after_outage->status == 200 && nlohmann::json::parse(after_outage->body).at("locked") == true,
                  "An outage leaves the lock state as it was");
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
            lemon::NexusManager restored(root.string(), listener_policy, listener_policy, 0);
            const auto status = restored.status();
            check(status.at("enabled") && status.at("locked") && !status.at("mesh_up"), "Restarts enabled and locked");
            check(status.at("device_name") == "My laptop", "Name survives restart");
            restored.disable();
        }
        {
            lemon::NexusManager disabled(root.string(), listener_policy, listener_policy, 0);
            check(!disabled.status().at("enabled"), "Disable survives restart");
        }
#endif
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; ++failures; }
    cleanup();
    return failures ? 1 : 0;
}
