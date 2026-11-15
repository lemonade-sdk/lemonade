#include "lemon/jobs/job_media_limits.h"

#include <cstdio>
#include <limits>

using namespace lemon::jobs;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

int main() {
    check("base64: empty", base64_decoded_size(0) == 0);
    check("base64: complete groups", base64_decoded_size(8) == 6);
    check("base64: incomplete group", base64_decoded_size(7) == 3);
    check("output: exact limit", media_output_within_limit(kMaxJobMediaOutputBytes));
    check("output: over limit", !media_output_within_limit(kMaxJobMediaOutputBytes + 1));
    check("budget: exact limit", job_media_within_budget(kMaxJobMediaStoreBytes - 10, 10));
    check("budget: over limit", !job_media_within_budget(kMaxJobMediaStoreBytes - 10, 11));
    check("budget: overflow", !job_media_within_budget(std::numeric_limits<size_t>::max(), 1));
    json media = {{"mime_type", "audio/wav"}, {"data", "YWJjZA=="}, {"size", 4}};
    check("media: explicit size", media_result_bytes(media) == 4);
    media.erase("size");
    check("media: base64 estimate", media_result_bytes(media) == 6);
    media["size"] = -1;
    check("media: negative size falls back", media_result_bytes(media) == 6);
    media["size"] = 1.5;
    check("media: fractional size falls back", media_result_bytes(media) == 6);
    check("media: ordinary output", media_result_bytes(json{{"size", 100}}) == 0);
    check("media: non-object", media_result_bytes("ordinary") == 0);
    const json image = {{"data", json::array({{{"b64_json", "YWJj"}}})}, {"size", 3}};
    check("media: image size", media_result_bytes(image) == 3);
    check("media: image estimate",
          media_result_bytes(json{{"data", json::array({{{"b64_json", "YWJj"}}})}}) == 3);
    check("media: image URL", media_result_bytes(json{{"data", json::array({{{"url", "image.png"}}})}}) == 0);
    const json context = {{"audio", media}, {"image", image}, {"ordinary", {{"tps", 42}}}};
    check("context: media sum", context_media_bytes(context) == 9);
    const json large = {{"mime_type", "audio/wav"}, {"data", ""},
                        {"size", std::numeric_limits<size_t>::max()}};
    check("context: overflow saturates",
          context_media_bytes(json{{"a", large}, {"b", large}}) == std::numeric_limits<size_t>::max());
    std::printf("\n%s (%d failures)\n", g_failures ? "FAIL" : "ALL PASS", g_failures);
    return g_failures ? 1 : 0;
}
