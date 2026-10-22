#include "lemon/server/gateway_conversion.h"

#include <cstdio>
#include <string>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static int failures = 0;

static void check(bool ok, const char* message) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failures;
}

static json first_message_start(const std::string& sse) {
    const auto event = sse.find("event: message_start");
    if (event == std::string::npos) return nullptr;
    const auto data = sse.find("data: ", event);
    if (data == std::string::npos) return nullptr;
    const auto end = sse.find('\n', data);
    const std::string payload = sse.substr(
        data + 6, end == std::string::npos ? std::string::npos : end - (data + 6));
    return json::parse(payload, nullptr, false);
}

static void run_case(bool upstream_has_chunk) {
    std::string written;
    int finished = 0;
    httplib::DataSink client;
    client.is_writable = [] { return true; };
    client.write = [&](const char* data, size_t size) {
        written.append(data, size);
        return true;
    };
    client.done = [&] { ++finished; };

    const std::string model = "user.Fallback-Test-GGUF";
    lemon::stream_openai_sse_to_anthropic_sse(
        "{}", client, model, {},
        [upstream_has_chunk](const std::string&, httplib::DataSink& upstream) {
            if (upstream_has_chunk) {
                const std::string frame =
                    "data: {\"id\":\"chatcmpl-test\",\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n";
                upstream.write(frame.data(), frame.size());
            }
            upstream.done();
        });

    const json start = first_message_start(written);
    const bool has_model = start.is_object() && start.contains("message") &&
                           start["message"].is_object() &&
                           start["message"].contains("model") &&
                           start["message"]["model"].is_string() &&
                           start["message"]["model"].get<std::string>() == model;
    check(has_model, upstream_has_chunk
              ? "regular Anthropic message_start includes requested model"
              : "fallback Anthropic message_start includes requested model");
    check(written.find("event: message_stop") != std::string::npos,
          "stream includes message_stop");
    check(finished == 1, "client stream finalized exactly once");
    const auto start_at = written.find("event: message_start");
    check(start_at != std::string::npos &&
              written.find("event: message_start", start_at + 1) == std::string::npos,
          "message_start appears exactly once");
}

int main() {
    run_case(false);  // No usable upstream frame: reproduces issue #3928 (3).
    run_case(true);   // Normal chunk path must not regress.
    return failures ? 1 : 0;
}
