#include <lemon/hf_variants.h>

#include <cstdio>
#include <string>
#include <vector>

struct TestResult {
    int passed = 0;
    int failed = 0;

    void ok(const std::string& name) {
        printf("[PASS] %s\n", name.c_str());
        ++passed;
    }

    void fail(const std::string& name, const std::string& detail) {
        printf("[FAIL] %s: %s\n", name.c_str(), detail.c_str());
        ++failed;
    }

    void expect(const std::string& name, bool cond, const std::string& detail) {
        cond ? ok(name) : fail(name, detail);
    }
};

namespace {

std::string join_names(const lemon::GgufVariantSet& set) {
    std::string out;
    for (size_t i = 0; i < set.variants.size(); ++i) {
        if (i) out += ", ";
        out += set.variants[i].name;
    }
    return out;
}

}  // namespace

int main() {
    TestResult result;

    {
        const nlohmann::json valid = {
            {"pipeline_tag", "text-generation"},
            {"gguf", {{"architecture", "llama"}, {"context_length", 4096}}},
        };
        result.expect("text GGUF metadata is accepted",
                      lemon::llamacpp_gguf_incompatibility(valid, "huggingface").empty(),
                      "known-good metadata was rejected");

        const nlohmann::json newer_backend_architecture = {
            {"pipeline_tag", "text-generation"},
            {"gguf", {{"architecture", "bailingmoe3"}}},
        };
        result.expect("architecture supported by all shipped backends is accepted",
                      lemon::llamacpp_gguf_incompatibility(
                          newer_backend_architecture, "huggingface").empty(),
                      "common architecture was rejected");
    }

    {
        const nlohmann::json missing_architecture = {
            {"gguf", {{"total", 12895570508ULL}, {"totalFileSize", 5544100160ULL}}},
        };
        const std::string error = lemon::llamacpp_gguf_incompatibility(
            missing_architecture, "huggingface");
        result.expect("GGUF metadata without architecture is rejected",
                      error.find("architecture") != std::string::npos,
                      "error was: " + error);
    }

    {
        const nlohmann::json image = {
            {"pipeline_tag", "text-to-image"},
            {"gguf", {{"architecture", "qwen_image"}}},
        };
        const std::string error = lemon::llamacpp_gguf_incompatibility(
            image, "huggingface");
        result.expect("provider-declared image GGUF is rejected",
                      error.find("different backend") != std::string::npos,
                      "error was: " + error);
    }

    {
        const nlohmann::json untagged_image = {
            {"gguf", {{"architecture", "krea2"}, {"total", 1}}},
        };
        const std::string error = lemon::llamacpp_gguf_incompatibility(
            untagged_image, "huggingface");
        result.expect("unsupported GGUF architecture is rejected",
                      error.find("krea2") != std::string::npos &&
                          error.find("not supported") != std::string::npos,
                      "error was: " + error);
    }

    {
        const nlohmann::json malformed = {
            {"gguf", {{"architecture", 7}, {"context_length", "4096"}}},
        };
        result.expect("malformed parsed architecture is rejected",
                      !lemon::llamacpp_gguf_incompatibility(
                           malformed, "huggingface").empty(),
                      "malformed metadata was accepted");
    }

    {
        const nlohmann::json blank_architecture = {
            {"gguf", {{"architecture", " \t"}, {"context_length", 4096}}},
        };
        result.expect("blank parsed architecture is rejected",
                      !lemon::llamacpp_gguf_incompatibility(
                           blank_architecture, "huggingface").empty(),
                      "blank architecture was accepted");

        const nlohmann::json supported_without_context = {
            {"gguf", {{"architecture", "llama"}}},
        };
        result.expect("supported architecture does not require heuristic metadata",
                      lemon::llamacpp_gguf_incompatibility(
                          supported_without_context, "huggingface").empty(),
                      "supported architecture was rejected");
    }

    {
        const nlohmann::json unavailable = {{"siblings", nlohmann::json::array()}};
        result.expect("unavailable parsed metadata remains compatible",
                      lemon::llamacpp_gguf_incompatibility(
                          unavailable, "huggingface").empty(),
                      "metadata-unaware mirrors must remain usable");
        result.expect("ModelScope is unchanged",
                      lemon::llamacpp_gguf_incompatibility(
                          nlohmann::json{{"gguf", nlohmann::json::object()}},
                          "modelscope").empty(),
                      "Hugging Face metadata policy leaked into ModelScope");
        result.expect("unknown registries are unchanged",
                      lemon::llamacpp_gguf_incompatibility(
                          nlohmann::json{{"gguf", nlohmann::json::object()}},
                          "custom-registry").empty(),
                      "Hugging Face metadata policy leaked into an unknown registry");
    }

    // Regression for the review on PR #2107: when two files share a quant token
    // their names widen to the full file stem for uniqueness. Sorting used to
    // read that widened name, which is not a key in the quant priority table, so
    // both Q4 variants fell into the "everything else" bucket and Q8_0 sorted
    // ahead of them. `pull --yes` takes variants[0], so it downloaded Q8_0 —
    // roughly twice the bytes — instead of the intended Q4_K_M default.
    {
        auto set = lemon::enumerate_gguf_variants({
            "Model-Q4_K_M.gguf",
            "Model-Q4_K_M-imatrix.gguf",
            "Model-Q8_0.gguf",
        });

        result.expect("Q4/Q4-imatrix/Q8: three separate variants",
                      set.variants.size() == 3,
                      "got " + std::to_string(set.variants.size()) + ": " + join_names(set));

        if (set.variants.size() == 3) {
            // Q4_K_M outranks Q8_0, so both Q4 variants must precede it.
            result.expect("Q4 sorts ahead of Q8 despite the widened name",
                          set.variants[0].quant == "Q4_K_M" &&
                              set.variants[1].quant == "Q4_K_M" &&
                              set.variants[2].quant == "Q8_0",
                          "order was " + join_names(set));

            // What `pull --yes` actually takes.
            result.expect("--yes picks the Q4_K_M base variant",
                          set.variants[0].name == "Model-Q4_K_M",
                          "variants[0].name = " + set.variants[0].name);

            // The collision-safety fix from the previous review round must hold:
            // names stay distinct, because callers pull `checkpoint:name`.
            result.expect("shared-quant variants keep distinct names",
                          set.variants[0].name != set.variants[1].name,
                          "both named " + set.variants[0].name);

            // Widening the name must not lose the quant used for ordering.
            result.expect("quant is tracked separately from the display name",
                          set.variants[0].quant == "Q4_K_M" &&
                              set.variants[0].name != set.variants[0].quant,
                          "name=" + set.variants[0].name + " quant=" + set.variants[0].quant);

            result.expect("imatrix file is not merged into the base variant",
                          set.variants[0].files.size() == 1 &&
                              set.variants[1].files.size() == 1 &&
                              !set.variants[0].sharded,
                          "base variant has " +
                              std::to_string(set.variants[0].files.size()) + " file(s)");
        }
    }

    // A quant appearing once keeps the bare token as its name — the common case,
    // and the shape the ordering table is written against.
    {
        auto set = lemon::enumerate_gguf_variants({
            "Model-Q8_0.gguf",
            "Model-Q4_K_M.gguf",
        });
        result.expect("unique quants are named by the bare token, Q4 first",
                      set.variants.size() == 2 && set.variants[0].name == "Q4_K_M" &&
                          set.variants[1].name == "Q8_0",
                      "got " + join_names(set));
    }

    // A real shard series is still one variant, and still sorts by its quant.
    {
        auto set = lemon::enumerate_gguf_variants({
            "Model-Q8_0.gguf",
            "Model-Q4_K_M-00001-of-00002.gguf",
            "Model-Q4_K_M-00002-of-00002.gguf",
        });
        bool shaped = set.variants.size() == 2 && set.variants[0].name == "Q4_K_M" &&
                      set.variants[0].sharded && set.variants[0].files.size() == 2 &&
                      set.variants[1].name == "Q8_0";
        result.expect("a genuine shard series stays one variant and sorts by quant",
                      shaped, "got " + join_names(set));
    }

    // An unrecognized quant sorts into the trailing bucket rather than jumping
    // ahead of known quants.
    {
        auto set = lemon::enumerate_gguf_variants({
            "Model-Q2_K.gguf",
            "Model-Q4_K_M.gguf",
        });
        result.expect("unranked quant sorts after a ranked one",
                      set.variants.size() == 2 && set.variants[0].name == "Q4_K_M",
                      "got " + join_names(set));
    }

    {
        auto set = lemon::enumerate_gguf_variants({
            "Muse-Glimmer-30B-Q8_0.gguf",
            "mmproj-Muse-Glimmer-30B-BF16.gguf",
            "dflash-kquant.gguf",
        });
        result.expect("DFlash companion is not exposed as a main variant",
                      set.variants.size() == 1 && set.variants[0].name == "Q8_0",
                      "got " + join_names(set));
        result.expect("DFlash companion is reported separately",
                      set.draft_files.size() == 1 &&
                          set.draft_files[0] == "dflash-kquant.gguf",
                      "draft count = " + std::to_string(set.draft_files.size()));
        result.expect("mmproj remains reported separately",
                      set.mmproj_files.size() == 1 &&
                          set.mmproj_files[0] == "mmproj-Muse-Glimmer-30B-BF16.gguf",
                      "mmproj count = " + std::to_string(set.mmproj_files.size()));
    }

    {
        auto set = lemon::enumerate_gguf_variants({
            "model.gguf",
            "dflash-kquant.gguf",
        });
        result.expect("DFlash does not break unquantized model fallback",
                      set.variants.size() == 1 && set.variants[0].name == "model.gguf" &&
                          set.draft_files.size() == 1,
                      "got " + join_names(set));
    }

    {
        auto set = lemon::enumerate_gguf_variants({
            "Model-Q4_K_M.gguf",
            "mtp-Model.gguf",
        });
        result.expect("MTP companion is reported as a draft",
                      set.variants.size() == 1 && set.variants[0].name == "Q4_K_M" &&
                          set.draft_files.size() == 1 && set.draft_files[0] == "mtp-Model.gguf",
                      "got " + join_names(set));
        result.expect("single draft is associated with the main variant",
                      set.variants.size() == 1 && set.variants[0].draft_file == "mtp-Model.gguf",
                      set.variants.empty() ? "no main variant" :
                          "draft=" + set.variants[0].draft_file);
    }

    // Multiple MTP precisions are resolved generically. The main Q4_K_M has no
    // exact sidecar tag, so nearest bit width selects Q4_0; Q8_0 matches exactly.
    {
        auto set = lemon::enumerate_gguf_variants({
            "Model-Q4_K_M.gguf",
            "Model-Q8_0.gguf",
            "mtp-Model-Q4_0.gguf",
            "mtp-Model-Q8_0.gguf",
        });
        result.expect("Q4 target selects nearest-bit MTP companion",
                      set.variants.size() == 2 && set.variants[0].quant == "Q4_K_M" &&
                          set.variants[0].draft_file == "mtp-Model-Q4_0.gguf",
                      set.variants.empty() ? "no variants" :
                          "draft=" + set.variants[0].draft_file);
        result.expect("Q8 target selects exact MTP companion",
                      set.variants.size() == 2 && set.variants[1].quant == "Q8_0" &&
                          set.variants[1].draft_file == "mtp-Model-Q8_0.gguf",
                      set.variants.size() < 2 ? "missing Q8 variant" :
                          "draft=" + set.variants[1].draft_file);
    }

    // A quant-named directory must not mask the draft file's own quant tag.
    // Both drafts have the same directory depth; the deliberately earlier
    // Q8 filename would win lexicographically if extraction read Q4_K_M from
    // the parent directory instead of Q8_0 from the filename.
    {
        auto set = lemon::enumerate_gguf_variants({
            "Q4_K_M/Model-Q4_K_M-00001-of-00002.gguf",
            "Q4_K_M/Model-Q4_K_M-00002-of-00002.gguf",
            "Q4_K_M/mtp-A-Model-Q8_0.gguf",
            "Q4_K_M/mtp-Z-Model-Q4_0.gguf",
        });
        result.expect("draft quant comes from filename, not parent directory",
                      set.variants.size() == 1 &&
                          set.variants[0].draft_file == "Q4_K_M/mtp-Z-Model-Q4_0.gguf",
                      set.variants.empty() ? "no variant" :
                          "draft=" + set.variants[0].draft_file);
        result.expect("draft association preserves repository-relative path",
                      set.variants.size() == 1 &&
                          set.variants[0].draft_file.find("Q4_K_M/") == 0,
                      set.variants.empty() ? "no variant" :
                          "draft=" + set.variants[0].draft_file);
    }

    // Without a quantized main there is no meaningful bit-distance comparison.
    // Stable path ordering wins instead of silently choosing the smallest-bit
    // draft (the old target_bits == 0 behavior).
    {
        auto set = lemon::enumerate_gguf_variants({
            "model.gguf",
            "mtp-A-Model-Q8_0.gguf",
            "mtp-Z-Model-Q2_0.gguf",
        });
        result.expect("unquantized main does not prefer smallest-bit draft",
                      set.variants.size() == 1 &&
                          set.variants[0].draft_file == "mtp-A-Model-Q8_0.gguf",
                      set.variants.empty() ? "no variant" :
                          "draft=" + set.variants[0].draft_file);
    }

    {
        auto set = lemon::enumerate_gguf_variants({
            "Model-Q8_0.gguf",
            "dflash-kquant.gguf",
            "mtp-Model.gguf",
        });
        result.expect("Multiple draft companions are all reported",
                      set.draft_files.size() == 2,
                      "draft count = " + std::to_string(set.draft_files.size()));
        result.expect("different speculative mechanisms stay ambiguous",
                      set.variants.size() == 1 && set.variants[0].draft_file.empty(),
                      set.variants.empty() ? "no variant" :
                          "draft=" + set.variants[0].draft_file);
    }

    {
        auto set = lemon::enumerate_gguf_variants({"My-MTP-Model-Q8_0.gguf"});
        result.expect("MTP in a main-model name is not treated as a draft",
                      set.variants.size() == 1 && set.variants[0].name == "Q8_0" &&
                          set.draft_files.empty(),
                      "got " + join_names(set));
    }

    printf("\n%d passed, %d failed\n", result.passed, result.failed);
    return result.failed == 0 ? 0 : 1;
}
