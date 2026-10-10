#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>

#include <lemon/live_generation_stats.h>
#include <lemon/streaming_proxy.h>

using namespace std::chrono_literals;

namespace {

int failures = 0;

void check(const char* name, bool condition) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) {
        ++failures;
    }
}

bool record_semantic_event(lemon::LiveGenerationStats::Request& request,
                           const std::string& event_data,
                           lemon::LiveGenerationStats::Clock::time_point now) {
    if (event_data == "[DONE]") {
        return false;
    }

    try {
        const auto event = nlohmann::json::parse(event_data);
        if (!lemon::StreamingProxy::is_semantic_generation_delta(event)) {
            return false;
        }
        request.record_semantic_chunk(now);
        return true;
    } catch (...) {
        return false;
    }
}

void test_complete_sse_event_filtering() {
    std::string line_buffer;
    std::string event_data;
    bool has_data_field = false;
    std::vector<std::string> events;
    auto process = [&](const std::string& fragment, bool end_of_stream = false) {
        line_buffer.append(fragment);
        lemon::StreamingProxy::process_sse_events(
            line_buffer, event_data, has_data_field,
            [&](const std::string& complete_event) { events.push_back(complete_event); },
            end_of_stream);
    };

    process("data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"think\",\r");
    process("\n: heartbeat\r\ndata: \"content\":\"A\"}}]}\r");
    process("\n\r");
    process("\ndata: [DONE]\r\n\r\n");
    process("data: not-json\r\n\r\n");
    process("data: {\"choices\":[{\"delta\":{\"content\":\"last\"}}]}\r\r");
    process("", true);

    int semantic_events = 0;
    for (const auto& event : events) {
        try {
            if (lemon::StreamingProxy::is_semantic_generation_delta(nlohmann::json::parse(event))) {
                ++semantic_events;
            }
        } catch (...) {
        }
    }

    check("split CRLF and multiline data form one semantic event", events.size() == 4 && semantic_events == 2);
    check("DONE and malformed events are not semantic generation", semantic_events == 2);
}

void test_fragmented_semantic_events_flow_through_streaming_proxy() {
    httplib::Server backend;
    std::atomic<bool> semantic_events_sent{false};
    std::atomic<bool> release_backend{false};

    backend.Post("/v1/chat/completions", [&](const httplib::Request&, httplib::Response& response) {
        response.set_chunked_content_provider("text/event-stream", [&](size_t, httplib::DataSink& sink) {
            const std::string role = "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n\n";
            const std::string usage = "data: {\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":40}}\n\n";
            const std::string mixed =
                "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"think\",\"content\":\"A\"}},"
                "{\"delta\":{\"content\":\"B\"}}]}\n\n";
            const std::string utf8 =
                "data: {\"choices\":[{\"delta\":{\"content\":\"caf\xC3\xA9\"}}]}\n\n";

            sink.write(role.data(), role.size());
            sink.write(usage.data(), usage.size());
            sink.write(mixed.data(), mixed.size());

            const auto split = utf8.find("\xC3") + 1;
            sink.write(utf8.data(), split);
            sink.write(utf8.data() + split, utf8.size() - split);
            semantic_events_sent.store(true, std::memory_order_release);

            while (!release_backend.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(5ms);
            }

            const std::string terminal =
                "data: {\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":40,"
                "\"decoding_speed_tps\":33.3}}\n\n";
            const std::string done = "data: [DONE]\n\n";
            sink.write(terminal.data(), terminal.size());
            sink.write(done.data(), done.size());
            sink.done();
            return false;
        });
    });

    const int port = backend.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        check("mock backend binds", false);
        return;
    }
    std::thread backend_thread([&backend]() { backend.listen_after_bind(); });
    backend.wait_until_ready();

    lemon::LiveGenerationStats stats;
    auto request = stats.track("qwen3-8b-FLM", lemon::LiveGenerationStats::Clock::now());
    std::string line_buffer;
    std::string event_data;
    bool has_data_field = false;
    std::atomic<int> semantic_events{0};
    lemon::StreamingProxy::TelemetryData terminal_telemetry;

    httplib::DataSink downstream;
    downstream.write = [&](const char* data, size_t length) {
        line_buffer.append(data, length);
        lemon::StreamingProxy::process_sse_events(
            line_buffer, event_data, has_data_field,
            [&](const std::string& complete_event) {
                if (record_semantic_event(request, complete_event,
                                          lemon::LiveGenerationStats::Clock::now())) {
                    semantic_events.fetch_add(1, std::memory_order_release);
                }
            });
        return true;
    };
    downstream.done = []() {};
    downstream.is_writable = []() { return true; };

    std::thread proxy_thread([&]() {
        lemon::StreamingProxy::forward_sse_stream(
            "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions",
            R"({"model":"qwen3-8b-FLM","stream":true})",
            downstream,
            [&](const lemon::StreamingProxy::TelemetryData& telemetry) {
                terminal_telemetry = telemetry;
            },
            10);
    });

    const auto deadline = lemon::LiveGenerationStats::Clock::now() + 2s;
    while ((!semantic_events_sent.load(std::memory_order_acquire) ||
            semantic_events.load(std::memory_order_acquire) != 2) &&
           lemon::LiveGenerationStats::Clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }

    const auto live = stats.snapshot(lemon::LiveGenerationStats::Clock::now());
    check("role and usage-only events do not count", semantic_events.load(std::memory_order_acquire) == 2);
    check("one event with reasoning and content counts once", live.generated_chunks == 2);
    check("semantic stream remains active before completion", live.active_requests == 1);
    check("semantic stream exposes a positive chunks-per-second estimate", live.generation_rate_estimate > 0.0);
    check("single active model is reported", live.model_name == std::optional<std::string>("qwen3-8b-FLM"));

    release_backend.store(true, std::memory_order_release);
    proxy_thread.join();
    request.reset();
    backend.stop();
    backend_thread.join();

    const auto idle = stats.snapshot(lemon::LiveGenerationStats::Clock::now());
    check("completion clears active live stream state", idle.active_requests == 0 && idle.generated_chunks == 0 && idle.generation_rate_estimate == 0.0);
    check("terminal output token count remains backend-reported", terminal_telemetry.output_tokens == 40);
    check("terminal token rate remains backend-reported", std::abs(terminal_telemetry.tokens_per_second - 33.3) < 0.001);
}

void record_and_return(lemon::LiveGenerationStats& stats) {
    auto request = stats.track("qwen3-8b-FLM");
    request.record_semantic_chunk();
}

void record_and_throw(lemon::LiveGenerationStats& stats) {
    auto request = stats.track("qwen3-8b-FLM");
    request.record_semantic_chunk();
    throw std::runtime_error("expected request failure");
}

void test_request_guard_cleans_up_on_return_and_exception() {
    lemon::LiveGenerationStats stats;

    record_and_return(stats);
    check("RAII cleanup after successful return", stats.snapshot().active_requests == 0);

    try {
        record_and_throw(stats);
    } catch (const std::runtime_error&) {
    }
    check("RAII cleanup after exception", stats.snapshot().active_requests == 0);
}

void test_downstream_abort_releases_request_guard() {
    httplib::Server backend;
    std::atomic<bool> release_backend{false};

    backend.Post("/v1/chat/completions", [&](const httplib::Request&, httplib::Response& response) {
        response.set_chunked_content_provider("text/event-stream", [&](size_t, httplib::DataSink& sink) {
            const std::string event = "data: {\"choices\":[{\"delta\":{\"content\":\"A\"}}]}\n\n";
            sink.write(event.data(), event.size());
            while (!release_backend.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(5ms);
            }
            return false;
        });
    });

    const int port = backend.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        check("abort mock backend binds", false);
        return;
    }
    std::thread backend_thread([&backend]() { backend.listen_after_bind(); });
    backend.wait_until_ready();

    lemon::LiveGenerationStats stats;
    {
        auto request = stats.track("qwen3-8b-FLM");
        std::string line_buffer;
        std::string event_data;
        bool has_data_field = false;

        httplib::DataSink downstream;
        downstream.write = [&](const char* data, size_t length) {
            line_buffer.append(data, length);
            lemon::StreamingProxy::process_sse_events(
                line_buffer, event_data, has_data_field,
                [&](const std::string& complete_event) {
                    record_semantic_event(request, complete_event,
                                          lemon::LiveGenerationStats::Clock::now());
                });
            return false;
        };
        downstream.done = []() {};
        downstream.is_writable = []() { return true; };

        lemon::StreamingProxy::forward_sse_stream(
            "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions",
            R"({"model":"qwen3-8b-FLM","stream":true})", downstream, nullptr, 10);
    }

    release_backend.store(true, std::memory_order_release);
    backend.stop();
    backend_thread.join();

    const auto idle = stats.snapshot();
    check("downstream abort releases the RAII request guard", idle.active_requests == 0 && idle.generated_chunks == 0);
}

void test_stats_contract_serialization() {
    lemon::LiveGenerationStats stats;
    const auto start = lemon::LiveGenerationStats::Clock::now();
    auto request = stats.track("qwen3-8b-FLM", start);
    request.record_semantic_chunk(start);

    nlohmann::json combined_stats{{"tokens_per_second", 33.3}};
    combined_stats.update(stats.snapshot(start + 1s).to_json());

    check("live stats contract reports active FLM stream", combined_stats["inference_active"] == true &&
          combined_stats["live_active_requests"] == 1 &&
          combined_stats["live_generated_chunks"] == 1 &&
          combined_stats["live_generation_recipe"] == "flm" &&
          combined_stats["live_generation_device"] == "npu" &&
          combined_stats["live_generation_model"] == "qwen3-8b-FLM");
    check("live stats contract uses the frozen semantic chunk unit and rate",
          combined_stats["live_generation_rate_unit"] == "semantic_sse_chunks_per_second" &&
          combined_stats["live_generation_rate_estimate"] == 1.0);
    check("live stats serialization leaves completed token rate unchanged",
          std::abs(combined_stats["tokens_per_second"].get<double>() - 33.3) < 0.001);

    request.reset();
    const auto idle = stats.snapshot(start + 1s).to_json();
    check("inactive contract clears live counters and model", idle["inference_active"] == false &&
          idle["live_active_requests"] == 0 && idle["live_generated_chunks"] == 0 &&
          idle["live_generation_rate_estimate"] == 0.0 && !idle.contains("live_generation_model"));
}

void test_concurrent_requests_and_stalled_output() {
    lemon::LiveGenerationStats stats;
    const auto start = lemon::LiveGenerationStats::Clock::now();
    auto first = stats.track("qwen3-8b-FLM", start);
    auto second = stats.track("gemma4-it-e4b-FLM", start);

    first.record_semantic_chunk(start);
    second.record_semantic_chunk(start);
    const auto concurrent = stats.snapshot(start + 1s);
    check("concurrent requests aggregate active count", concurrent.active_requests == 2);
    check("concurrent requests aggregate chunks", concurrent.generated_chunks == 2);
    check("concurrent requests sum one-event rates", concurrent.generation_rate_estimate == 2.0);
    check("mixed active models omit ambiguous model tag", !concurrent.model_name.has_value());

    first.reset();
    const auto after_one_reset = stats.snapshot(start + 1s);
    check("one request reset leaves concurrent request active",
          after_one_reset.active_requests == 1 && after_one_reset.generated_chunks == 1 &&
              after_one_reset.model_name == std::optional<std::string>("gemma4-it-e4b-FLM"));

    const auto stalled = stats.snapshot(start + 6s);
    check("stalled semantic output decays to zero", stalled.generation_rate_estimate == 0.0);

    second.reset();
    const auto idle = stats.snapshot(start + 6s);
    check("abort or error cleanup clears all active stream state", idle.active_requests == 0 && idle.generated_chunks == 0);
}

} // namespace

int main() {
    test_complete_sse_event_filtering();
    test_fragmented_semantic_events_flow_through_streaming_proxy();
    test_request_guard_cleans_up_on_return_and_exception();
    test_downstream_abort_releases_request_guard();
    test_stats_contract_serialization();
    test_concurrent_requests_and_stalled_output();

    if (failures == 0) {
        std::printf("All live generation stats tests passed.\n");
    } else {
        std::printf("%d live generation stats test(s) failed.\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
