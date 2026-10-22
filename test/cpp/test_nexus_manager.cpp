#include "lemon/nexus_manager.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>

namespace {
using nlohmann::json;
namespace access = lemon::nexus_access;

bool throws(const std::function<void()>& action) {
    try { action(); } catch (const std::runtime_error&) { return true; }
    return false;
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
}

int main() {
    int failures = 0;
    const auto check = [&](bool condition, const char* description) {
        if (!condition) { std::cerr << description << '\n'; ++failures; }
    };
    const auto root = std::filesystem::temp_directory_path() / ("lemonade-nexus-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto cleanup = [&] { std::error_code ec; std::filesystem::remove_all(root, ec); };
    try {
        check_access_rules(check);
        {
            lemon::NexusManager manager(root.string(), 13305);
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
            }
            request("enable", "[]", 400);
            request("enable", "{", 400);
#ifdef LEMONADE_WITH_NEXUS
            auto enabled = request("enable", "{}", 200);
            check(enabled.at("available") && enabled.at("enabled") && enabled.at("locked"), "SDK enabled without unlocking");
            check(std::filesystem::exists(root / "nexus/identity.json"), "Device identity persisted");
            request("join", R"({"code":"12345678"})", 423);
            request("egress", R"({"node_id":"another-device"})", 423);
            request("invites", R"({"device_name":"New device"})", 423);
            request("passkey/unlock", R"({"passkey_assertion":{"credential_id":"wrong"}})", 403);
            request("members/peer/role", R"({"role":"admin"})", 423);
            request("members/peer/permissions", R"({"permissions":["read"]})", 423);
            request("devices/peer/access", R"({"member_node_id":"other","see":true,"connect":false})", 423);
            auto locked_members = management.Get("/api/v1/nexus/members");
            check(locked_members && locked_members->status == 423, "Member list requires an unlocked network");
            auto locked_access = management.Get("/api/v1/nexus/devices/peer/access");
            check(locked_access && locked_access->status == 423, "Device access requires an unlocked network");
            request("device", "{}", 400);
            request("device", R"({"device_name":""})", 400);
            auto named = request("device", R"({"device_name":"My laptop"})", 200);
            check(named.at("device_name") == "My laptop", "Device name updated");
            request("lock", "{}", 200);
#else
            request("enable", "{}", 503);
            request("members/peer/role", R"({"role":"admin"})", 503);
#endif
            auto events = management.Get("/api/v1/nexus/events?after=0&once=1");
            check(events && events->status == 200 && events->get_header_value("Content-Type") == "text/event-stream", "Events use SSE format");
            server.stop(); worker.join(); manager.stop();
        }
#ifdef LEMONADE_WITH_NEXUS
        {
            lemon::NexusManager restored(root.string(), 13305);
            const auto status = restored.status();
            check(status.at("enabled") && status.at("locked") && !status.at("mesh_up"), "Restarts enabled and locked");
            check(status.at("device_name") == "My laptop", "Name survives restart");
            restored.disable();
        }
        {
            lemon::NexusManager disabled(root.string(), 13305);
            check(!disabled.status().at("enabled"), "Disable survives restart");
        }
#endif
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; ++failures; }
    cleanup();
    return failures ? 1 : 0;
}
