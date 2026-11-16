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
    check("payload: string", payload_bytes("YWJjZA==") == 8);
    check("payload: nested array and object",
          payload_bytes(json{{"a", json::array({"abc", json{{"b", "de"}}, 1, true})},
                             {"c", "f"}}) == 6);
    check("payload: number", payload_bytes(42) == 0);
    check("payload: bool", payload_bytes(true) == 0);
    json nested = "abc";
    for (size_t depth = 0; depth < 32; ++depth) nested = json::array({nested});
    check("payload: bounded nesting", payload_bytes(nested) == 3);
    nested = json::array({nested});
    check("payload: excessive nesting saturates",
          payload_bytes(nested) == std::numeric_limits<size_t>::max());
    check("payload: nested overflow saturates",
          payload_bytes(json::array({"abc", nested})) == std::numeric_limits<size_t>::max());
    Job job;
    check("retained: empty job", retained_media_bytes(job) == 0);
    StepRecord audio_step;
    audio_step.id = "a";
    audio_step.output = media;
    audio_step.extract = {{"a", "size"}};
    job.steps.push_back(audio_step);
    job.context["a"] = 4;
    check("retained: extract cannot hide media", retained_media_bytes(job) == 6);
    Job aliased_job;
    aliased_job.steps.push_back(audio_step);
    aliased_job.context["a"] = media;
    check("retained: media context counted once", retained_media_bytes(aliased_job) == 6);
    aliased_job.context["copy1"] = media["data"];
    aliased_job.context["copy2"] = media["data"];
    aliased_job.context["copy3"] = media["data"];
    check("retained: every extracted payload copy counts",
          retained_media_bytes(aliased_job) == 6 + 3 * 8);
    StepRecord ordinary_step;
    ordinary_step.output = {{"tps", 42}};
    job.steps.push_back(ordinary_step);
    StepRecord image_step;
    image_step.output = image;
    job.steps.push_back(image_step);
    check("retained: mixed output sum", retained_media_bytes(job) == 9);
    const json large = {{"mime_type", "audio/wav"}, {"data", ""},
                        {"size", std::numeric_limits<size_t>::max()}};
    check("projected: new extract counts in full",
          projected_media_bytes(image, json{{"copy", "YWJj"}}, json::object()) == 3 + 4);
    check("projected: larger extract counts only growth",
          projected_media_bytes(image, json{{"copy", "YWJjZA=="}},
                                json{{"copy", "YWJj"}}) == 3 + 4);
    check("projected: smaller extract adds zero",
          projected_media_bytes(image, json{{"copy", "YWJj"}},
                                json{{"copy", "YWJjZA=="}}) == 3);
    check("projected: media context does not reduce extract",
          projected_media_bytes(image, json{{"copy", "YWJj"}},
                                json{{"copy", media}}) == 3 + 4);
    check("projected: overflow saturates",
          projected_media_bytes(large, json{{"copy", "YWJj"}}, json::object()) ==
              std::numeric_limits<size_t>::max());
    job.steps[0].output = large;
    job.steps[2].output = large;
    check("retained: overflow saturates",
          retained_media_bytes(job) == std::numeric_limits<size_t>::max());
    aliased_job.steps[0].output = large;
    check("retained: context overflow saturates",
          retained_media_bytes(aliased_job) == std::numeric_limits<size_t>::max());
    std::printf("\n%s (%d failures)\n", g_failures ? "FAIL" : "ALL PASS", g_failures);
    return g_failures ? 1 : 0;
}
