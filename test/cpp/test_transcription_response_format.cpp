// Transcription response_format interpretation (PR #2088 / issue #1833).
//
// Regression cover for the case where a transcript is itself valid JSON. The
// requested format, not a parse attempt, must decide how a successful backend
// body is read.

#include "lemon/audio_types.h"

#include <cstdio>
#include <string>

using lemon::audio::interpret_transcription_body;
using lemon::audio::is_plain_text_format;
using lemon::audio::is_supported_response_format;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

// A body that is both a plausible transcript and valid JSON.
static void json_looking_text_is_returned_verbatim(const char* body) {
    const nlohmann::json result = interpret_transcription_body(body, "text");
    check("json-looking body wraps as an object",
          result.is_object() && result.contains("text"));
    check("json-looking body keeps its exact bytes",
          result.value("text", std::string()) == body);
}

int main() {
    // Format classification.
    check("text is plain text", is_plain_text_format("text"));
    check("srt is plain text", is_plain_text_format("srt"));
    check("vtt is plain text", is_plain_text_format("vtt"));
    check("json is not plain text", !is_plain_text_format("json"));
    check("verbose_json is not plain text", !is_plain_text_format("verbose_json"));

    check("json is supported", is_supported_response_format("json"));
    check("verbose_json is supported", is_supported_response_format("verbose_json"));
    check("vtt is supported", is_supported_response_format("vtt"));
    check("xml is not supported", !is_supported_response_format("xml"));
    check("empty is not supported", !is_supported_response_format(""));

    // The regression: each of these parses cleanly as JSON, so a
    // parse-first implementation would silently skip the {"text": ...} wrapper
    // and return JSON to a client that asked for raw text.
    json_looking_text_is_returned_verbatim("true\n");
    json_looking_text_is_returned_verbatim("null");
    json_looking_text_is_returned_verbatim("123");
    json_looking_text_is_returned_verbatim("\"quoted\"");
    json_looking_text_is_returned_verbatim("{\"model\":\"m\",\"text\":\"hi\"}");

    // Ordinary transcripts are unaffected.
    {
        const nlohmann::json result =
            interpret_transcription_body(" ask not what your country can do for you", "text");
        check("ordinary transcript passes through",
              result.value("text", std::string()) ==
                  " ask not what your country can do for you");
    }

    // Subtitle bodies keep their markup rather than being parsed.
    {
        const std::string srt = "1\n00:00:00,000 --> 00:00:02,000\nhello\n";
        const nlohmann::json result = interpret_transcription_body(srt, "srt");
        check("srt body is preserved", result.value("text", std::string()) == srt);
    }

    // JSON formats still parse, and still throw on malformed input.
    {
        const nlohmann::json result =
            interpret_transcription_body("{\"text\":\"hi\"}", "json");
        check("json format parses", result.value("text", std::string()) == "hi");
    }
    {
        bool threw = false;
        try {
            interpret_transcription_body("not json", "verbose_json");
        } catch (const nlohmann::json::parse_error&) {
            threw = true;
        }
        check("malformed json still throws", threw);
    }

    if (g_failures == 0) {
        std::printf("All transcription response_format tests passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
