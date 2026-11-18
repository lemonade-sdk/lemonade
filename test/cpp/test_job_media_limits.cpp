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
    check("payload: string", payload_bytes("YWJjZA==") == 8);
    check("payload: nested array and object",
          payload_bytes(json{{"a", json::array({"abc", json{{"b", "de"}}, 1, true})},
                             {"c", "f"}}) == 6 + 2 * sizeof(json));
    check("payload: number", payload_bytes(42) == sizeof(json));
    check("payload: fraction", payload_bytes(1.5) == sizeof(json));
    check("payload: bool", payload_bytes(true) == sizeof(json));
    check("payload: null", payload_bytes(nullptr) == sizeof(json));
    check("payload: numeric array", payload_bytes(json::array({1, 2, 3})) == 3 * sizeof(json));
    json nested = "abc";
    for (size_t depth = 0; depth < 32; ++depth) nested = json::array({nested});
    check("payload: bounded nesting", payload_bytes(nested) == 3);
    nested = json::array({nested});
    check("payload: excessive nesting saturates",
          payload_bytes(nested) == std::numeric_limits<size_t>::max());
    check("payload: nested overflow saturates",
          payload_bytes(json::array({"abc", nested})) == std::numeric_limits<size_t>::max());
    check("stored: ordinary output", media_stored_bytes(json{{"size", 100}}) == 0);
    check("stored: non-object", media_stored_bytes("ordinary") == 0);
    check("stored: zero decoded size", media_stored_bytes(json{{"mime_type", "audio/wav"},
                                                           {"data", ""}, {"size", 0}}) == 0);
    check("stored: binary includes base64 and both copies",
          media_stored_bytes(media) == 2 * (8 + 9 + sizeof(json)));
    check("stored: image includes both copies", media_stored_bytes(image) == 2 * (4 + sizeof(json)));
    check("stored: decoded cap does not imply store cap",
          media_stored_bytes(boundary_media) > kMaxJobMediaStoreBytes);
    json deep_media = image;
    deep_media["extra"] = nested;
    check("stored: overflow saturates",
          media_stored_bytes(deep_media) == std::numeric_limits<size_t>::max());

    Job job;
    check("budget: empty job", MediaStoreBudget(job, json::object(), "step").bytes == 0);
    StepRecord audio_step;
    audio_step.id = "a";
    audio_step.output = media;
    job.steps.push_back(audio_step);
    StepRecord ordinary_step;
    ordinary_step.output = {{"tps", 42}};
    job.steps.push_back(ordinary_step);
    StepRecord image_step;
    image_step.output = image;
    job.steps.push_back(image_step);
    const size_t output_bytes = media_stored_bytes(media) + media_stored_bytes(image);
    check("budget: mixed stored outputs", MediaStoreBudget(job, json::object(), "step").bytes == output_bytes);
    MediaStoreBudget budget(job, image, "step");
    const size_t initial_bytes = output_bytes + media_stored_bytes(image);
    check("budget: existing and new stored outputs", budget.bytes == initial_bytes);
    budget.resolve_extract("copy", "YWJj", true);
    budget.resolve_extract("other", "abc", true);
    check("extracts: media adds stored string bytes",
          budget.extracts == std::map<std::string, size_t>{{"copy", 4}, {"other", 3}} &&
              budget.bytes == initial_bytes + 7);
    budget.resolve_extract("copy", "ab", true);
    check("extracts: media replacement updates size",
          budget.extracts.at("copy") == 2 && budget.bytes == initial_bytes + 5);
    budget.resolve_extract("copy", "new", false);
    check("extracts: non-media replacement removes key",
          budget.extracts == std::map<std::string, size_t>{{"other", 3}} &&
              budget.bytes == initial_bytes + 3);
    budget.resolve_extract("numbers", json::array({1, 2}), true);
    check("extracts: numeric array counts", budget.extracts.at("numbers") == 2 * sizeof(json));
    job.media_extract_bytes = {{"copy", 4}, {"other", 3}};
    MediaStoreBudget collision(job, image, "copy");
    check("extracts: step output drops colliding key without extracts",
          collision.extracts == std::map<std::string, size_t>{{"other", 3}} &&
              collision.bytes == initial_bytes + 3);
    collision.resolve_extract("copy", "ab", true);
    check("extracts: own step id charged after dropping old entry",
          collision.extracts == std::map<std::string, size_t>{{"copy", 2}, {"other", 3}} &&
              collision.bytes == initial_bytes + 5);
    check("extracts: dropping step id leaves current tally unchanged",
          job.media_extract_bytes == std::map<std::string, size_t>{{"copy", 4}, {"other", 3}});

    Job clobbered;
    StepRecord audio;
    audio.output = {{"mime_type", "audio/wav"}, {"data", std::string(2u * 1024u * 1024u, 'A')}};
    clobbered.steps.push_back(audio);
    clobbered.media_extract_bytes = {{"pause", 2u * 1024u * 1024u}};
    check("clobbered: audio and extract store 6 MiB plus two MIME strings",
          MediaStoreBudget(clobbered, json::object(), "audio").bytes ==
              6u * 1024u * 1024u + 18);
    MediaStoreBudget pause(clobbered, ordinary_step.output, "pause");
    check("clobbered: pause drops its old 2 MiB extract",
          pause.extracts.empty() && pause.bytes == 4u * 1024u * 1024u + 18);
    clobbered.media_extract_bytes = pause.extracts;
    check("clobbered: next audio stores 8 MiB plus four MIME strings",
          MediaStoreBudget(clobbered, audio.output, "final").bytes ==
              8u * 1024u * 1024u + 36);

    const json small_media = {{"mime_type", "audio/wav"}, {"data", "YWJj"}, {"size", 3}};
    const std::string three_mib(3u * 1024u * 1024u, 'a');
    MediaStoreBudget first(Job{}, small_media, "first");
    for (const auto* key : {"a", "b", "c"}) first.resolve_extract(key, three_mib, true);
    check("cumulative: first step fits", first.bytes <= kMaxJobMediaStoreBytes);
    MediaStoreBudget alone(Job{}, small_media, "second");
    for (const auto* key : {"d", "e", "f"}) alone.resolve_extract(key, three_mib, true);
    check("cumulative: second step alone fits", alone.bytes <= kMaxJobMediaStoreBytes);
    Job cumulative;
    StepRecord first_step;
    first_step.output = small_media;
    cumulative.steps.push_back(first_step);
    cumulative.media_extract_bytes = first.extracts;
    MediaStoreBudget combined(cumulative, small_media, "second");
    check("cumulative: retained first extracts counted",
          combined.bytes == 2 * media_stored_bytes(small_media) + 9u * 1024u * 1024u);
    size_t resolved = 0;
    for (const auto* key : {"d", "e", "f", "g"}) {
        combined.resolve_extract(key, three_mib, true);
        ++resolved;
        if (combined.bytes > kMaxJobMediaStoreBytes) break;
    }
    check("cumulative: combined steps exceed budget and stop resolving",
          combined.bytes > kMaxJobMediaStoreBytes && resolved == 3);
    MediaStoreBudget replaced(cumulative, small_media, "second");
    for (const auto* key : {"a", "b", "c"}) replaced.resolve_extract(key, three_mib, true);
    check("cumulative: replacing keys fits", replaced.bytes ==
          2 * media_stored_bytes(small_media) + 9u * 1024u * 1024u);

    const json large_chat = {{"response", std::string(kMaxJobMediaStoreBytes + 1, 'x')}};
    MediaStoreBudget chat(Job{}, large_chat, "step");
    chat.resolve_extract("response", large_chat["response"], false);
    check("extracts: non-media response beyond budget is unrestricted",
          chat.extracts.empty() && chat.bytes == 0);
    Job boundary;
    boundary.media_extract_bytes = {{"copy", kMaxJobMediaStoreBytes - media_stored_bytes(image)}};
    MediaStoreBudget exact(boundary, image, "step");
    check("budget: exact stored limit", exact.bytes == kMaxJobMediaStoreBytes);
    exact.resolve_extract("extra", "x", true);
    check("budget: one stored byte over", exact.bytes == kMaxJobMediaStoreBytes + 1);
    exact.resolve_extract("copy", "new", false);
    check("budget: non-media removal reduces total before verdict",
          exact.bytes == media_stored_bytes(image) + 1);
    Job overflowing;
    overflowing.media_extract_bytes = {{"a", std::numeric_limits<size_t>::max()}, {"b", 1}};
    MediaStoreBudget overflow(overflowing, image, "step");
    check("budget: tally overflow saturates", overflow.bytes == std::numeric_limits<size_t>::max());
    overflow.resolve_extract("a", "x", true);
    check("budget: saturated replacement cannot undercount", overflow.bytes == std::numeric_limits<size_t>::max());
    MediaStoreBudget deep(Job{}, image, "step");
    deep.resolve_extract("copy", nested, true);
    check("budget: extract overflow saturates", deep.bytes == std::numeric_limits<size_t>::max());
    check("budget: output overflow saturates",
          MediaStoreBudget(Job{}, deep_media, "step").bytes == std::numeric_limits<size_t>::max());
    std::printf("\n%s (%d failures)\n", g_failures ? "FAIL" : "ALL PASS", g_failures);
    return g_failures ? 1 : 0;
}
