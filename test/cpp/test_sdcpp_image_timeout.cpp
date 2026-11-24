#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <httplib.h>

#include <lemon/backends/sdcpp/sdcpp_server.h>
#include <lemon/utils/http_client.h>

namespace {

int g_failures = 0;

void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

constexpr long kGlobalTimeoutSeconds = 1;
constexpr auto kGenerationTime = std::chrono::seconds(3);

class FakeSdServer : public lemon::backends::SDServer {
public:
    explicit FakeSdServer(int port) : lemon::backends::SDServer("error", nullptr, nullptr) {
        port_ = port;
        set_state(lemon::ModelState::READY);
    }

    bool is_backend_alive() const override { return true; }
};

// Takes longer than the global timeout to answer, like a real generation.
struct SlowBackend {
    httplib::Server server;
    std::thread thread;
    int port = 0;

    SlowBackend() {
        auto generate = [](const httplib::Request&, httplib::Response& res) {
            std::this_thread::sleep_for(kGenerationTime);
            res.set_content(R"({"created":1,"data":[{"b64_json":"AAAA"}]})", "application/json");
        };
        server.Post("/v1/images/generations", generate);
        server.Post("/v1/images/edits", generate);
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
    }

    ~SlowBackend() {
        server.stop();
        thread.join();
    }
};

bool has_image(const lemon::json& result) {
    return !result.contains("error") && result.contains("data") && result["data"].is_array() &&
           result["data"].size() == 1;
}

void test_generation_outlives_the_global_timeout() {
    SlowBackend backend;
    FakeSdServer sd(backend.port);
    const lemon::json result = sd.image_generations({{"prompt", "a lemon"}});
    check("generations: the image comes back after the global timeout", has_image(result));
}

void test_edit_outlives_the_global_timeout() {
    SlowBackend backend;
    FakeSdServer sd(backend.port);
    const lemon::json result = sd.image_edits({{"prompt", "a lemon"}});
    check("edits: the image comes back after the global timeout", has_image(result));
}

void test_variation_outlives_the_global_timeout() {
    SlowBackend backend;
    FakeSdServer sd(backend.port);
    const lemon::json result = sd.image_variations(lemon::json::object());
    check("variations: the image comes back after the global timeout", has_image(result));
}

} // namespace

int main() {
    lemon::utils::HttpClient::set_default_timeout(kGlobalTimeoutSeconds);
    test_generation_outlives_the_global_timeout();
    test_edit_outlives_the_global_timeout();
    test_variation_outlives_the_global_timeout();
    if (g_failures) {
        std::printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("ALL PASS (0 failures)\n");
    return 0;
}
