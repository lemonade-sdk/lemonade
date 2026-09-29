#include "lemon/nexus_manager.h"
#include <chrono>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>

int main() {
    int failures = 0;
    const auto check = [&](bool condition, const char* description) {
        if (!condition) { std::cerr << description << '\n'; ++failures; }
    };
    const auto root = std::filesystem::temp_directory_path() / ("lemonade-nexus-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto cleanup = [&] { std::error_code ec; std::filesystem::remove_all(root, ec); };
    try {
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
            request("device", "{}", 400);
            request("device", R"({"device_name":""})", 400);
            auto named = request("device", R"({"device_name":"My laptop"})", 200);
            check(named.at("device_name") == "My laptop", "Device name updated");
            request("lock", "{}", 200);
#else
            request("enable", "{}", 503);
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
