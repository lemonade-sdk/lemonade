// Standalone test for lemon GGUF capability detection (src/cpp/include/lemon/gguf_capabilities.h).
// Focus: the MTP (Multi-Token Prediction) label path added in PR #2176, plus the
// sibling vision / tool-calling detection that lives in the same header.
//
// Note on scope: the raw MTP *trigger* (scanning GGUF KV metadata for
// `nextn_predict_layers` and matching llama.cpp MTP/MAD tensor names) lives in
// model_manager.cpp and requires a parsed GGUF file. That sets
// GgufCapabilities::mtp. This test covers the header-level contract that turns
// that flag into the user-visible "mtp" model label (apply_gguf_capability_labels)
// and the dedup helper (add_label_once), which is the regression-prone seam.
//
// Compile with: cl /std:c++17 /EHsc /I src/cpp/include test/cpp/test_gguf_capabilities.cpp
// or:          g++ -std=c++17 -I src/cpp/include test/cpp/test_gguf_capabilities.cpp -o gguf_caps_test

#include "lemon/gguf_capabilities.h"
#include <cstdio>
#include <string>
#include <vector>

using lemon::GgufCapabilities;
using lemon::add_label_once;
using lemon::apply_gguf_capability_labels;
using lemon::inspect_gguf_string;

static int g_failures = 0;

static bool has_label(const std::vector<std::string>& labels, const std::string& label) {
    return std::find(labels.begin(), labels.end(), label) != labels.end();
}

static int count_label(const std::vector<std::string>& labels, const std::string& label) {
    int n = 0;
    for (const auto& l : labels) {
        if (l == label) ++n;
    }
    return n;
}

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

int main() {
    // --- MTP label application -------------------------------------------------

    // mtp flag set -> "mtp" label added, and the function reports a change.
    {
        GgufCapabilities caps;
        caps.mtp = true;
        std::vector<std::string> labels;
        bool changed = apply_gguf_capability_labels(labels, caps);
        check("mtp=true adds 'mtp' label", has_label(labels, "mtp"));
        check("mtp=true reports changed=true", changed);
        check("mtp=true adds exactly one 'mtp'", count_label(labels, "mtp") == 1);
    }

    // mtp flag unset -> no "mtp" label, no change.
    {
        GgufCapabilities caps;  // all false by default
        std::vector<std::string> labels;
        bool changed = apply_gguf_capability_labels(labels, caps);
        check("mtp=false does not add 'mtp'", !has_label(labels, "mtp"));
        check("all-false reports changed=false", !changed);
        check("all-false leaves labels empty", labels.empty());
    }

    // Idempotency: applying twice must not duplicate, and the second call is a no-op.
    {
        GgufCapabilities caps;
        caps.mtp = true;
        std::vector<std::string> labels;
        apply_gguf_capability_labels(labels, caps);
        bool changed_second = apply_gguf_capability_labels(labels, caps);
        check("re-applying mtp does not duplicate", count_label(labels, "mtp") == 1);
        check("re-applying mtp reports changed=false", !changed_second);
    }

    // Pre-existing "mtp" label is preserved without duplication.
    {
        GgufCapabilities caps;
        caps.mtp = true;
        std::vector<std::string> labels = {"reasoning", "mtp"};
        bool changed = apply_gguf_capability_labels(labels, caps);
        check("pre-existing 'mtp' not duplicated", count_label(labels, "mtp") == 1);
        check("pre-existing 'mtp' reports changed=false", !changed);
        check("pre-existing unrelated label preserved", has_label(labels, "reasoning"));
    }

    // --- add_label_once helper -------------------------------------------------
    {
        std::vector<std::string> labels;
        bool first = add_label_once(labels, "mtp");
        bool second = add_label_once(labels, "mtp");
        check("add_label_once first insert returns true", first);
        check("add_label_once second insert returns false", !second);
        check("add_label_once keeps a single copy", count_label(labels, "mtp") == 1);
    }

    // --- MTP combines with the other capabilities ------------------------------
    {
        GgufCapabilities caps;
        caps.vision = true;
        caps.tool_calling = true;
        caps.mtp = true;
        std::vector<std::string> labels;
        bool changed = apply_gguf_capability_labels(labels, caps);
        check("vision+tool_calling+mtp adds all three",
              has_label(labels, "vision") && has_label(labels, "tool-calling") &&
                  has_label(labels, "mtp"));
        check("combined apply reports changed=true", changed);
    }

    // --- inspect_gguf_string does NOT infer mtp (it is detected upstream) -------
    // mtp must come from the KV/tensor scan in model_manager.cpp, never from a
    // metadata-string heuristic, so a value mentioning "mtp" must not flip it.
    {
        GgufCapabilities caps;
        inspect_gguf_string("general.name", "some-model-mtp-v1", caps);
        check("inspect_gguf_string never sets mtp", !caps.mtp);
    }

    // Guard the sibling detection in the same header so this file fully covers it.
    {
        GgufCapabilities caps;
        inspect_gguf_string("general.architecture", "qwen3vl", caps);
        check("inspect_gguf_string detects vision", caps.vision);
    }
    {
        GgufCapabilities caps;
        inspect_gguf_string("tokenizer.chat_template", "{% if tools %}<tool_call>", caps);
        check("inspect_gguf_string detects tool-calling", caps.tool_calling);
    }
    {
        GgufCapabilities caps;
        // A non-stable key must not trigger vision even with a matching value.
        inspect_gguf_string("some.random.key", "this is a vision image model", caps);
        check("inspect_gguf_string ignores non-stable vision key", !caps.vision);
    }

    if (g_failures == 0) {
        std::printf("\nAll gguf_capabilities tests passed\n");
        return 0;
    }
    std::printf("\n%d gguf_capabilities test(s) FAILED\n", g_failures);
    return 1;
}
