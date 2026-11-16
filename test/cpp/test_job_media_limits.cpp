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
    check("base64: empty", base64_decoded_size("") == 0);
    check("base64: complete groups", base64_decoded_size("YWJjYWJj") == 6);
    check("base64: incomplete group", base64_decoded_size("YWJjYWJ") == 3);
    check("base64: one padding byte", base64_decoded_size("YWJjZGU=") == 5);
    check("base64: two padding bytes", base64_decoded_size("YWJjZA==") == 4);
    check("base64: single character", base64_decoded_size("Y") == 0);
    check("base64: short padding", base64_decoded_size("=") == 0);
    check("base64: two padding characters only", base64_decoded_size("==") == 0);
    check("output: exact limit", media_output_within_limit(kMaxJobMediaOutputBytes));
    check("output: over limit", !media_output_within_limit(kMaxJobMediaOutputBytes + 1));
    check("budget: exact limit", job_media_within_budget(kMaxJobMediaStoreBytes - 10, 10));
    check("budget: over limit", !job_media_within_budget(kMaxJobMediaStoreBytes - 10, 11));
    check("budget: overflow", !job_media_within_budget(std::numeric_limits<size_t>::max(), 1));
    json media = {{"mime_type", "audio/wav"}, {"data", "YWJjZA=="}, {"size", 4}};
    check("media: explicit size", media_result_bytes(media) == 4);
    media.erase("size");
    check("media: base64 estimate", media_result_bytes(media) == 4);
    media["size"] = -1;
    check("media: negative size falls back", media_result_bytes(media) == 4);
    media["size"] = 1.5;
    check("media: fractional size falls back", media_result_bytes(media) == 4);
    check("media: ordinary output", media_result_bytes(json{{"size", 100}}) == 0);
    check("media: non-object", media_result_bytes("ordinary") == 0);
    const json image = {{"data", json::array({{{"b64_json", "YWJj"}}})}, {"size", 3}};
    check("media: image size", media_result_bytes(image) == 3);
    check("media: image estimate",
          media_result_bytes(json{{"data", json::array({{{"b64_json", "YWJj"}}})}}) == 3);
    check("media: image URL", media_result_bytes(json{{"data", json::array({{{"url", "image.png"}}})}}) == 0);
    std::string padded(((kMaxJobMediaOutputBytes + 2) / 3) * 4, 'A');
    padded.back() = '=';
    const json boundary_image = {{"data", json::array({{{"b64_json", padded}}})}};
    check("image: padded exact limit size",
          media_result_bytes(boundary_image) == kMaxJobMediaOutputBytes);
    check("image: padded exact limit accepted",
          media_output_within_limit(base64_decoded_size(padded)));
    const json boundary_media = {{"mime_type", "audio/wav"}, {"data", padded}};
    check("media: padded exact limit size",
          media_result_bytes(boundary_media) == kMaxJobMediaOutputBytes);
    check("media: padded exact limit accepted",
          media_output_within_limit(media_result_bytes(boundary_media)));
    padded.back() = 'A';
    const json over_image = {{"data", json::array({{{"b64_json", padded}}})}};
    check("image: one byte over limit size",
          media_result_bytes(over_image) == kMaxJobMediaOutputBytes + 1);
    check("image: one byte over limit rejected",
          !media_output_within_limit(base64_decoded_size(padded)));
    const json over_media = {{"mime_type", "audio/wav"}, {"data", padded}};
    check("media: one byte over limit size",
          media_result_bytes(over_media) == kMaxJobMediaOutputBytes + 1);
    check("media: one byte over limit rejected",
          !media_output_within_limit(media_result_bytes(over_media)));
    check("extract: exact cap accepted",
          payload_bytes(std::string(kMaxJobExtractValueBytes, 'x')) <= kMaxJobExtractValueBytes);
    check("extract: one byte over cap rejected",
          payload_bytes(std::string(kMaxJobExtractValueBytes + 1, 'x')) > kMaxJobExtractValueBytes);
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
    check("retained: extract cannot hide media", retained_media_bytes(job) == 4);
    Job aliased_job;
    aliased_job.steps.push_back(audio_step);
    aliased_job.context["a"] = media;
    check("retained: media context counted once", retained_media_bytes(aliased_job) == 4);
    aliased_job.context["copy1"] = media["data"];
    aliased_job.context["copy2"] = media["data"];
    aliased_job.context["copy3"] = media["data"];
    check("retained: context aliases ignored", retained_media_bytes(aliased_job) == 4);
    aliased_job.context["inputs"] = nested;
    check("retained: deeply nested context ignored", retained_media_bytes(aliased_job) == 4);
    Job context_only_job;
    context_only_job.context = aliased_job.context;
    check("retained: context without media ignored", retained_media_bytes(context_only_job) == 0);
    StepRecord ordinary_step;
    ordinary_step.output = {{"tps", 42}};
    job.steps.push_back(ordinary_step);
    StepRecord image_step;
    image_step.output = image;
    job.steps.push_back(image_step);
    check("retained: mixed output sum", retained_media_bytes(job) == 7);
    const json large = {{"mime_type", "audio/wav"}, {"data", ""},
                        {"size", std::numeric_limits<size_t>::max()}};
    const json extracts = {{"copy", "YWJj"}, {"other", "abc"}};
    check("projected: output and extracts count in full",
          projected_media_bytes(image, extracts) == 3 + 4 + 3);
    const size_t projected = projected_media_bytes(image, extracts);
    job.context["copy"] = "old";
    job.context["other"] = media;
    check("projected: existing context does not change result",
          projected_media_bytes(image, extracts) == projected);
    job.context["copy"] = "a longer replacement";
    check("projected: context replacement does not change result",
          projected_media_bytes(image, extracts) == projected);
    check("projected: output without extracts", projected_media_bytes(image, json::object()) == 3);
    check("projected: overflow saturates",
          projected_media_bytes(large, json{{"copy", "YWJj"}}) ==
              std::numeric_limits<size_t>::max());
    check("projected: extract overflow saturates",
          projected_media_bytes(image, json{{"copy", nested}}) ==
              std::numeric_limits<size_t>::max());
    job.steps[0].output = large;
    job.steps[2].output = large;
    check("retained: overflow saturates",
          retained_media_bytes(job) == std::numeric_limits<size_t>::max());
    std::printf("\n%s (%d failures)\n", g_failures ? "FAIL" : "ALL PASS", g_failures);
    return g_failures ? 1 : 0;
}
