#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include <httplib.h>

#include <lemon/backends/thenoise/thenoise_server.h>
#include <lemon/utils/json_utils.h>

namespace {
using lemon::json;
int failures = 0;
void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

class FakeTheNoise : public lemon::backends::TheNoiseServer {
public:
    explicit FakeTheNoise(int port) : TheNoiseServer("error", nullptr, nullptr) {
        port_ = port;
        set_state(lemon::ModelState::READY);
    }
    bool is_backend_alive() const override { return true; }
    void options(const json& values) {
        recipe_options_ = lemon::RecipeOptions("thenoise", values);
    }
};

const std::string clip_bytes("\x1a\x45\xdf\xa3\0video", 10);

struct FakeBackend {
    httplib::Server server;
    std::thread thread;
    std::atomic<int> submits{0}, polls{0}, cancels{0}, downloads{0}, after_cancel{0};
    std::atomic<bool> fail_first_poll{false}, hold_running{false}, cancel_on_submit{false};
    std::atomic<bool> complete_on_cancel{false}, fail_all_polls{false};
    std::atomic<bool> stall_content{false}, release_content{false};
    std::atomic<bool>* cancel_flag = nullptr;
    std::mutex mutex;
    json last_body;
    int port = 0;

    explicit FakeBackend(std::atomic<bool>* flag = nullptr) : cancel_flag(flag) {
        server.Post("/jobs/video", [this](const httplib::Request& req, httplib::Response& res) {
            ++submits;
            polls = 0;
            after_cancel = 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                last_body = json::parse(req.body);
            }
            if (cancel_on_submit && cancel_flag) cancel_flag->store(true);
            res.status = 202;
            res.set_content(R"({"id":"job_123","kind":"video","status":"queued"})", "application/json");
        });
        server.Get("/jobs/job_123", [this](const httplib::Request&, httplib::Response& res) {
            ++polls;
            if (fail_all_polls || fail_first_poll.exchange(false)) {
                res.status = 503;
                res.set_content("busy", "text/plain");
                return;
            }
            if (hold_running) {
                if (cancels == 0 && cancel_flag) cancel_flag->store(true);
                // Native cancellation returns before the worker stops. Keep
                // reporting running for two polls after acknowledging it.
                if (cancels == 0 || ++after_cancel <= 2) {
                    res.set_content(R"({"status":"running"})", "application/json");
                    return;
                }
                if (!complete_on_cancel) {
                    res.set_content(R"({"status":"canceled"})", "application/json");
                    return;
                }
            }
            std::string format;
            {
                std::lock_guard<std::mutex> lock(mutex);
                format = last_body.value("output_format", "webm");
            }
            json status = {{"status", "completed"}, {"resolved", {
                {"frame_count", 33}, {"fps", 24}, {"output_format", format}}}};
            res.set_content(status.dump(), "application/json");
        });
        server.Post("/jobs/job_123/cancel", [this](const httplib::Request&, httplib::Response& res) {
            ++cancels;
            res.status = complete_on_cancel ? 409 : 200;
            res.set_content(R"({"status":"canceling"})", "application/json");
        });
        server.Get("/jobs/job_123/content", [this](const httplib::Request&, httplib::Response& res) {
            ++downloads;
            if (stall_content && cancel_flag) {
                cancel_flag->store(true);
                // Bound the simulated stall even if cancellation regresses.
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
                while (!release_content && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            std::lock_guard<std::mutex> lock(mutex);
            const bool mp4 = last_body.value("output_format", "webm") == "mp4";
            res.set_content(clip_bytes, mp4 ? "video/mp4" : "video/webm");
        });
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
    }
    ~FakeBackend() {
        release_content = true;
        server.stop();
        thread.join();
    }
    json body() {
        std::lock_guard<std::mutex> lock(mutex);
        return last_body;
    }
};

void test_shared_recipe_defaults() {
    const lemon::RecipeOptions sd_options("sd-cpp", json::object());
    check("sd-cpp keeps its 33-frame default", sd_options.get_option("video_frames") == 33);
    check("sd-cpp keeps its 16-fps default", sd_options.get_option("fps") == 16);
}

void test_success() {
    FakeBackend backend;
    FakeTheNoise model(backend.port);
    backend.fail_first_poll = true;
    check("unsupported format is refused without submission",
          model.video_generations({{"prompt", "a cat"}, {"output_format", "avi"}}).contains("error") &&
          backend.submits == 0);
    const auto result = model.video_generations({{"model", "alias"}, {"clip_file", "reserved"},
        {"prompt", "a cat"}, {"video_frames", 33}, {"duration", 2}, {"cfg_scale", 1.0},
        {"sampling_method", "euler"}, {"seed", -7}, {"fps", 24}, {"output_format", "mp4"}});
    const auto body = backend.body();
    check("request maps only video fields", body == json({{"prompt", "a cat"}, {"frames", 33},
        {"guidance_scale", 1.0}, {"sampler", "euler"}, {"seed", -1}, {"fps", 24}, {"output_format", "mp4"}}));
    check("transient poll failure retried, submit not repeated", backend.polls == 2 && backend.submits == 1);
    check("resolved metadata and MIME", result.value("frame_count", 0) == 33 &&
        result.value("fps", 0.0) == 24 && result.value("mime_type", "") == "video/mp4");
    check("binary content is preserved", lemon::utils::JsonUtils::base64_decode(result.value("b64_json", "")) == clip_bytes);

    const auto next = model.video_generations({{"prompt", "another cat"}, {"duration", 2.0},
        {"image", "aW1hZ2U="}});
    check("duration and start image forwarded, no image defaults", backend.body() == json({{"prompt", "another cat"},
        {"duration", 2.0}, {"image", "aW1hZ2U="}, {"output_format", "webm"}}));
    check("WebM content", next.value("mime_type", "") == "video/webm");

    model.options({{"video_frames", 65}, {"fps", 16}, {"width", 832}, {"height", 480},
                   {"steps", 12}, {"cfg_scale", 1.0}, {"sampler", "unipc"}, {"flow_shift", 5.0},
                   {"film_grain", 0.5}, {"qwen_vae_enhance", true}});
    model.video_generations({{"prompt", "recipe"}, {"duration", 2}, {"steps", 4}});
    check("explicit duration and steps override stored defaults", backend.body() == json({
        {"prompt", "recipe"}, {"duration", 2}, {"fps", 16}, {"width", 832}, {"height", 480},
        {"steps", 4}, {"guidance_scale", 1.0}, {"sampler", "unipc"}, {"flow_shift", 5.0},
        {"output_format", "webm"}}));
}

void test_cancel(bool during_submit, bool completion_wins) {
    std::atomic<bool> flag{true};
    FakeBackend backend(&flag);
    FakeTheNoise model(backend.port);
    const json request = {{"prompt", "a cat"}};
    check("pre-submit cancellation", model.video_generations(request, &flag).contains("error") && backend.submits == 0);
    flag = false;
    backend.hold_running = true;
    backend.cancel_on_submit = during_submit;
    backend.complete_on_cancel = completion_wins;
    const auto result = model.video_generations(request, &flag);
    check("cancel sent exactly once", backend.cancels == 1);
    check("cancel waits for terminal backend state", backend.after_cancel == 3);
    check("cancel does not download or publish a clip", backend.downloads == 0 && !result.contains("b64_json"));
    check("settled cancellation keeps model warm", result.contains("error") &&
        result["error"].contains("details") && result["error"]["details"].value("backend_job_settled", false));
    flag = false;
    backend.hold_running = false;
    backend.cancel_on_submit = false;
    const auto next = model.video_generations(request, &flag);
    check("next video uses the same backend", next.contains("b64_json") && backend.submits == 2 && backend.downloads == 1);
}

void test_lost_job() {
    FakeBackend backend;
    FakeTheNoise model(backend.port);
    backend.fail_all_polls = true;
    const auto result = model.video_generations({{"prompt", "a cat"}});
    check("persistent status loss fails without resubmission",
          result.contains("error") && backend.polls == 3 && backend.submits == 1);
}

void test_cancel_content() {
    std::atomic<bool> flag{false};
    FakeBackend backend(&flag);
    FakeTheNoise model(backend.port);
    backend.stall_content = true;
    const auto start = std::chrono::steady_clock::now();
    const auto result = model.video_generations({{"prompt", "a cat"}}, &flag);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    backend.release_content = true;
    check("cancel interrupts content transfer", elapsed < std::chrono::seconds(7));
    check("terminal job needs no backend cancel", backend.downloads == 1 && backend.cancels == 0);
    check("canceled content is not published and backend stays reusable",
          result.contains("error") && !result.contains("b64_json") &&
          result["error"].contains("details") && result["error"]["details"].value("backend_job_settled", false));
}
} // namespace

int main() {
    test_shared_recipe_defaults();
    test_success();
    test_cancel(false, false);
    test_cancel(true, false);
    test_cancel(false, true);
    test_lost_job();
    test_cancel_content();
    return failures ? 1 : 0;
}
