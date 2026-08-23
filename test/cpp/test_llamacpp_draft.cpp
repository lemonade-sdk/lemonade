// Test for lemon::backends::compute_draft_activation() — the pure decision
// behind --model-draft / --spec-type emission in llama-server launches.
// Build with: cmake --build --preset default --target test_llamacpp_draft
// Run with: ctest --test-dir build -R llamacpp_draft --output-on-failure

#include "lemon/backends/llamacpp/llamacpp_draft.h"

#include <cstdio>
#include <string>
#include <vector>

using lemon::backends::DraftActivation;
using lemon::backends::compute_draft_activation;
using lemon::backends::is_dflash_draft_checkpoint;

// Defaults that keep the GLM4-MOE mmproj guard out of the way for the
// non-GLM4 cases below.
static DraftActivation act(const std::vector<std::string>& labels,
                           const std::string& checkpoint,
                           bool draft_present,
                           const std::string& arch = "qwen3",
                           const std::string& mmproj_path = "",
                           bool hf_load = false) {
    return compute_draft_activation(labels, checkpoint, draft_present,
                                    arch, mmproj_path, hf_load);
}

static bool expect_activation(const char* name,
                              const DraftActivation& actual,
                              bool want_use_draft,
                              const std::string& want_spec_type) {
    bool ok = actual.use_draft_checkpoint == want_use_draft &&
              actual.spec_type == want_spec_type;
    std::printf("[%s] %s\n  got:  use_draft=%s spec_type=\"%s\"\n"
                "  want: use_draft=%s spec_type=\"%s\"\n",
                ok ? "PASS" : "FAIL",
                name,
                actual.use_draft_checkpoint ? "true" : "false",
                actual.spec_type.c_str(),
                want_use_draft ? "true" : "false",
                want_spec_type.c_str());
    return ok;
}

static bool expect_dflash_checkpoint(const char* name,
                                     const std::string& checkpoint,
                                     bool want) {
    bool ok = is_dflash_draft_checkpoint(checkpoint) == want;
    std::printf("[%s] %s\n  got:  is_dflash(%s)=%s\n  want: %s\n",
                ok ? "PASS" : "FAIL",
                name,
                checkpoint.c_str(),
                is_dflash_draft_checkpoint(checkpoint) ? "true" : "false",
                want ? "true" : "false");
    return ok;
}

int main() {
    int failures = 0;

    // The three activation cases called out in review of PR #3253.
    failures += !expect_activation(
        "draft present + no mtp label -> inactive",
        act({}, "mtp-drafter.gguf", /*draft_file_present=*/true),
        false, "");

    failures += !expect_activation(
        "draft missing + mtp label -> inactive",
        act({"mtp"}, "mtp-drafter.gguf", /*draft_file_present=*/false),
        false, "");

    failures += !expect_activation(
        "draft present + mtp label -> --model-draft + --spec-type draft-mtp",
        act({"mtp"}, "mtp-drafter.gguf", /*draft_file_present=*/true),
        true, "draft-mtp");

    // DFlash stays gated on its own label.
    failures += !expect_activation(
        "dflash draft present + dflash label -> draft-dflash",
        act({"dflash"}, "dflash-v2.gguf", /*draft_file_present=*/true),
        true, "draft-dflash");

    failures += !expect_activation(
        "dflash draft present + mtp label only -> inactive",
        act({"mtp"}, "dflash.gguf", /*draft_file_present=*/true),
        false, "");

    failures += !expect_activation(
        "dflash draft present + no label -> inactive",
        act({}, "dflash.gguf", /*draft_file_present=*/true),
        false, "");

    // A plain (non-dflash) drafter with a dflash label but no mtp label must
    // not activate MTP either: the label has to match the drafter kind.
    failures += !expect_activation(
        "mtp draft present + dflash label only -> inactive",
        act({"dflash"}, "mtp-drafter.gguf", /*draft_file_present=*/true),
        false, "");

    // Unrelated labels must not trigger activation.
    failures += !expect_activation(
        "draft present + unrelated labels -> inactive",
        act({"chat", "vision"}, "mtp-drafter.gguf", /*draft_file_present=*/true),
        false, "");

    // GLM4-MOE without mmproj must stay MTP-inactive (crash guard, #2451).
    failures += !expect_activation(
        "glm4-moe + mtp label + no mmproj -> inactive (crash guard)",
        act({"mtp"}, "mtp-drafter.gguf", /*draft_file_present=*/true,
            /*arch=*/"glm4-moe"),
        false, "");

    failures += !expect_activation(
        "chatglm4 + mtp label + no mmproj -> inactive (crash guard)",
        act({"mtp"}, "mtp-drafter.gguf", /*draft_file_present=*/true,
            /*arch=*/"chatglm4"),
        false, "");

    // GLM4-MOE with an mmproj present is fine: MTP activates.
    failures += !expect_activation(
        "glm4-moe + mtp label + mmproj present -> draft-mtp",
        act({"mtp"}, "mtp-drafter.gguf", /*draft_file_present=*/true,
            /*arch=*/"glm4-moe", /*mmproj_path=*/"/models/mmproj-f16.gguf"),
        true, "draft-mtp");

    // GLM4-MOE loaded via -hf: llama-server resolves mmproj itself -> MTP ok.
    failures += !expect_activation(
        "glm4-moe + mtp label + hf_load -> draft-mtp",
        act({"mtp"}, "mtp-drafter.gguf", /*draft_file_present=*/true,
            /*arch=*/"glm4-moe", /*mmproj_path=*/"", /*hf_load=*/true),
        true, "draft-mtp");

    // Case-insensitive architecture matching (GLM4 vs glm4).
    failures += !expect_activation(
        "GLM4 (uppercase) + mtp label + no mmproj -> inactive",
        act({"mtp"}, "mtp-drafter.gguf", /*draft_file_present=*/true,
            /*arch=*/"GLM4"),
        false, "");

    // Non-GLM4 architectures are unaffected by the crash guard.
    failures += !expect_activation(
        "gemma-4 + mtp label + no mmproj -> draft-mtp (text-only ok)",
        act({"mtp"}, "mtp-drafter.gguf", /*draft_file_present=*/true,
            /*arch=*/"gemma4"),
        true, "draft-mtp");

    // DFlash checkpoint-name detection (filename only, path separators ignored).
    failures += !expect_dflash_checkpoint("dflash.gguf", "dflash.gguf", true);
    failures += !expect_dflash_checkpoint("dflash-v2.gguf", "dflash-v2.gguf", true);
    failures += !expect_dflash_checkpoint("mtp.gguf", "mtp.gguf", false);
    failures += !expect_dflash_checkpoint("path/to/dflash.gguf", "path/to/dflash.gguf", true);
    failures += !expect_dflash_checkpoint("backslash dflash-v2", "dir\\dflash-v2.gguf", true);
    failures += !expect_dflash_checkpoint("case-insensitive", "Dflash-V2.gguf", true);
    failures += !expect_dflash_checkpoint("prefix but not match", "mydflash.gguf", false);

    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
