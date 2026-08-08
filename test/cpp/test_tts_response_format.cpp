#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lemon/tts_response_format.h"

namespace {

int failures = 0;

void expect(const std::string& actual, const std::string& expected, const std::string& name) {
    if (actual == expected) {
        std::cout << "ok: " << name << std::endl;
        return;
    }
    std::cerr << "FAIL: " << name << " (expected " << expected << ", got " << actual << ")"
              << std::endl;
    ++failures;
}

}  // namespace

int main() {
    using lemon::select_tts_response_format;
    using nlohmann::json;

    expect(select_tts_response_format(json::object(), {}), "mp3",
           "unrestricted backend keeps the OpenAI default");
    expect(select_tts_response_format(json::object(), {"wav"}), "wav",
           "restricted backend defaults to its native format");
    expect(select_tts_response_format(json{{"response_format", "opus"}}, {"wav", "opus"}), "opus",
           "explicit response format takes precedence");
    expect(select_tts_response_format(json{{"stream", true}}, {"wav"}), "wav",
           "streaming preserves a backend's native format");
    expect(select_tts_response_format(
               json{{"stream", true}, {"response_format", "wav"}}, {"wav"}),
           "wav", "streaming preserves an explicit response format");
    expect(select_tts_response_format(json{{"stream_format", "audio"}}, {"wav"}), "pcm",
           "stream_format audio selects raw PCM");

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
