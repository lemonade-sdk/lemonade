#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include <iostream>

#include <httplib.h>
#include <lemon/streaming_proxy.h>

using namespace std::chrono_literals;

static int g_failures = 0;

static void check(bool condition, const char* name) {
    if (condition) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++g_failures;
    }
}

// Custom data sink to capture the transformed output
struct CaptureSink : public httplib::DataSink {
    std::string data;
    bool is_done = false;

    CaptureSink() {
        write = [this](const char* buf, size_t length) {
            data.append(buf, length);
            return true;
        };
        done = [this]() {
            is_done = true;
        };
        is_writable = []() { return true; };
    }
};

static void run_normalization_test(
    const std::string& test_name,
    const std::vector<std::string>& chunks,
    bool normalize_chat_roles,
    const std::string& expected_output
) {
    httplib::Server backend;
    backend.Post("/v1/chat/completions",
        [&](const httplib::Request&, httplib::Response& res) {
            res.set_chunked_content_provider(
                "text/event-stream",
                [&](size_t, httplib::DataSink& sink) {
                    for (const auto& chunk : chunks) {
                        sink.write(chunk.data(), chunk.size());
                        std::this_thread::sleep_for(5ms);
                    }
                    sink.done();
                    return false;
                });
        });

    const int port = backend.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        std::printf("[FAIL] %s: failed to bind mock backend\n", test_name.c_str());
        ++g_failures;
        return;
    }

    std::thread bg([&]() { backend.listen_after_bind(); });

    CaptureSink sink;
    lemon::StreamingProxy::forward_sse_stream(
        "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions",
        "{}",
        sink,
        nullptr,
        300,
        nullptr,
        0, // heartbeat
        normalize_chat_roles
    );

    backend.stop();
    bg.join();

    if (sink.data != expected_output) {
        std::printf("[FAIL] %s\n", test_name.c_str());
        std::cout << "Expected:\n" << expected_output << "\nGot:\n" << sink.data << "\n";
        ++g_failures;
    } else {
        std::printf("[PASS] %s\n", test_name.c_str());
    }
}

int main() {
    // 1. Null initial role replaced by "assistant"
    run_normalization_test(
        "Null initial role replaced by assistant",
        { "data: {\"choices\":[{\"delta\":{\"role\":null,\"content\":\"Hi\"}}]}\n\n",
          "data: [DONE]\n\n" },
        true,
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\",\"role\":\"assistant\"}}]}\n\n"
        "data: [DONE]\n\n"
    );

    // 2. Valid initial role left untouched
    run_normalization_test(
        "Valid initial role left untouched",
        { "data: {\"choices\":[{\"delta\":{\"role\":\"system\",\"content\":\"Hi\"}}]}\n\n",
          "data: [DONE]\n\n" },
        true,
        "data: {\"choices\":[{\"delta\":{\"role\":\"system\",\"content\":\"Hi\"}}]}\n\n"
        "data: [DONE]\n\n"
    );

    // 3. Later null role inside content chunks (e.g. thinking models) also normalized to assistant
    run_normalization_test(
        "Later null role normalized to assistant",
        { "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"A\"}}]}\n\n",
          "data: {\"choices\":[{\"delta\":{\"role\":null,\"content\":\"B\"}}]}\n\n",
          "data: [DONE]\n\n" },
        true,
        "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"A\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"B\",\"role\":\"assistant\"}}]}\n\n"
        "data: [DONE]\n\n"
    );

    // 4. Fragmented transport (chunks split across write callbacks)
    run_normalization_test(
        "Fragmented transport",
        { "data: {\"choices\":",
          "[{\"delta\":{\"role\":null",
          ",\"content\":\"Hi\"}}]}\n\n",
          "data: [DONE]\n\n" },
        true,
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\",\"role\":\"assistant\"}}]}\n\n"
        "data: [DONE]\n\n"
    );

    // 5. Non-chat passthrough (normalize_chat_roles = false preserves null role)
    run_normalization_test(
        "Non-chat passthrough preserves null role",
        { "data: {\"choices\":[{\"delta\":{\"role\":null,\"content\":\"Hi\"}}]}\n\n",
          "data: [DONE]\n\n" },
        false,
        "data: {\"choices\":[{\"delta\":{\"role\":null,\"content\":\"Hi\"}}]}\n\n"
        "data: [DONE]\n\n"
    );

    // 6. Protocol preservation (: ping comments, [DONE], usage chunks, malformed events)
    run_normalization_test(
        "Protocol preservation",
        { ": ping\n\n",
          "data: {\"usage\":{\"prompt_tokens\":5}}\n\n",
          "data: {\"choices\":[{\"delta\":{\"role\":null,\"content\":\"Hi\"}}]}\n\n",
          "data: malformed\n\n",
          "data: [DONE]\n\n" },
        true,
        ": ping\n\n"
        "data: {\"usage\":{\"prompt_tokens\":5}}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\",\"role\":\"assistant\"}}]}\n\n"
        "data: malformed\n\n"
        "data: [DONE]\n\n"
    );

    // 7. Multi-choice stream (multiple choices in first delta with null role all replaced)
    run_normalization_test(
        "Multi-choice stream",
        { "data: {\"choices\":[{\"delta\":{\"role\":null,\"content\":\"A\"}},{\"delta\":{\"role\":null,\"content\":\"B\"}}]}\n\n",
          "data: [DONE]\n\n" },
        true,
        "data: {\"choices\":[{\"delta\":{\"content\":\"A\",\"role\":\"assistant\"}},{\"delta\":{\"content\":\"B\",\"role\":\"assistant\"}}]}\n\n"
        "data: [DONE]\n\n"
    );

    // 8. Tool-call passthrough (unrelated tool-call fragments untouched)
    run_normalization_test(
        "Tool-call passthrough",
        { "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"id\":\"call_1\"}]}}]}\n\n",
          "data: [DONE]\n\n" },
        true,
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"id\":\"call_1\"}]}}]}\n\n"
        "data: [DONE]\n\n"
    );

    return g_failures == 0 ? 0 : 1;
}
