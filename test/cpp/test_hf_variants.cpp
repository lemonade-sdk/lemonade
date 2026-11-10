#include <lemon/backends/backend_registry.h>
#include <lemon/hf_variants.h>

#include <algorithm>
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

lemon::backends::RepositoryAnalysis analyze(
    const std::string& recipe, const std::vector<std::string>& files,
    const nlohmann::json& metadata,
    const std::vector<std::pair<std::string, uint64_t>>& file_sizes = {}) {
    lemon::backends::RepositoryView view;
    view.checkpoint = "owner/repo";
    view.registry_source = "huggingface";
    view.files = files;
    view.file_sizes = file_sizes;
    view.metadata = metadata;
    return lemon::backends::ops_for(recipe)->analyze_repository(view);
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
        auto set = lemon::enumerate_gguf_variants({
            "Qwen3.8-Flash-Next-UD-Q3_K_XL.gguf",
            "Qwen3.8-Flash-Next-MTP-UD-Q3_K_XL.gguf",
        });
        result.expect("model-prefixed MTP-UD is a draft, not another main variant",
                      set.variants.size() == 1 && set.draft_files.size() == 1 &&
                          set.variants[0].draft_file ==
                              "Qwen3.8-Flash-Next-MTP-UD-Q3_K_XL.gguf",
                      "got " + join_names(set));
    }
    {
        auto set = lemon::enumerate_gguf_variants({"My-MTP-Model-Q8_0.gguf"});
        result.expect("MTP in a main-model name is not treated as a draft",
                      set.variants.size() == 1 && set.variants[0].name == "Q8_0" &&
                          set.draft_files.empty(),
                      "got " + join_names(set));
    }

    {
        auto analysis = analyze("kokoro", {"model.gguf", "ggml-tiny.bin"}, nlohmann::json::object());
        result.expect("a backend without the hook declines every repository",
                      !analysis.match && analysis.refusal.empty(),
                      analysis.refusal);
    }

    {
        auto analysis = analyze("llamacpp",
                                {"model-Q4_K_M.gguf", "model-Q8_0.gguf", "mmproj-model-f16.gguf"},
                                {{"pipeline_tag", "image-text-to-text"},
                                 {"gguf", {{"architecture", "llama"}}}});
        const bool vision = analysis.match &&
            std::find(analysis.match->labels.begin(), analysis.match->labels.end(), "vision") !=
                analysis.match->labels.end();
        result.expect("llama.cpp offers the GGUF variants of a vision repository",
                      analysis.match && analysis.match->repo_kind == "gguf" &&
                          analysis.match->variants.size() == 2 &&
                          analysis.match->mmproj_files.size() == 1 && vision,
                      analysis.refusal);
    }
    {
        auto analysis = analyze("llamacpp", {"sd-v1-5-Q8_0.gguf"},
                                {{"pipeline_tag", "text-to-image"}});
        result.expect("llama.cpp refuses GGUF files made for another task",
                      !analysis.match &&
                          analysis.refusal.find("not compatible with llama.cpp") !=
                              std::string::npos,
                      analysis.refusal);
    }
    {
        auto analysis = analyze("llamacpp", {"ggml-tiny.bin"}, nlohmann::json::object());
        result.expect("llama.cpp declines a repository without GGUF files",
                      !analysis.match && analysis.refusal.empty(), analysis.refusal);
    }

    {
        auto analysis = analyze("ryzenai-llm",
                                {"genai_config.json", "model.onnx", "model.onnx.data"},
                                nlohmann::json::object());
        result.expect("Ryzen AI takes an OGA repository as one default variant",
                      analysis.match && analysis.match->repo_kind == "onnx-ryzenai" &&
                          analysis.match->variants.size() == 1 &&
                          analysis.match->variants[0].name == "default" &&
                          analysis.match->variants[0].files.size() == 3,
                      analysis.refusal);
    }
    {
        auto with_gguf = analyze("ryzenai-llm", {"genai_config.json", "model.onnx", "model.gguf"},
                                 nlohmann::json::object());
        auto no_config = analyze("ryzenai-llm", {"model.onnx"}, nlohmann::json::object());
        result.expect("Ryzen AI declines repositories without an OGA layout",
                      !with_gguf.match && !no_config.match, "");
    }

    {
        auto analysis = analyze(
            "whispercpp",
            {".gitattributes", "README.md", "ggml-base.en-q5_1.bin", "ggml-base.en.bin",
             "ggml-tiny.bin"},
            {{"pipeline_tag", "automatic-speech-recognition"}}, {{"ggml-tiny.bin", 77691713}});
        std::string names;
        if (analysis.match) {
            for (const auto& v : analysis.match->variants) names += v.name + " ";
        }
        result.expect("whisper.cpp offers one single-file variant per ggml-*.bin",
                      analysis.match && analysis.match->repo_kind == "single-file" &&
                          analysis.match->variants.size() == 3 &&
                          analysis.match->variants[0].name == "ggml-base.en-q5_1.bin" &&
                          analysis.match->variants[2].primary_file == "ggml-tiny.bin" &&
                          analysis.match->variants[2].size_bytes == 77691713,
                      names);
    }
    {
        auto analysis = analyze(
            "whispercpp", {"pytorch_model.bin", "config.json"},
            {{"pipeline_tag", "automatic-speech-recognition"}, {"library_name", "transformers"}});
        result.expect("whisper.cpp declines a transformers speech repository", !analysis.match, "");
    }
    {
        auto analysis = analyze(
            "whispercpp",
            {"ggml-base.en.bin", "ggml-base.en-encoder-openvino.bin",
             "ggml-base.en-encoder-openvino.xml", "ggml-base.en-encoder.mlmodelc.zip",
             "ggml-silero-v5.1.2.bin", "models/ggml-small.bin"},
            {{"pipeline_tag", "automatic-speech-recognition"}});
        result.expect("whisper.cpp encoders, VAD models and nested files are not variants",
                      analysis.match && analysis.match->variants.size() == 1 &&
                          analysis.match->variants[0].name == "ggml-base.en.bin",
                      "");
    }
    {
        auto vad_only = analyze("whispercpp", {"ggml-silero-v6.2.0.bin"},
                                {{"pipeline_tag", "automatic-speech-recognition"}});
        auto no_task = analyze("whispercpp", {"ggml-model.bin"}, nlohmann::json::object());
        result.expect("whisper.cpp declines a VAD-only repository and ggml files without the task",
                      !vad_only.match && !no_task.match, "");
    }

    {
        auto analysis = analyze(
            "sd-cpp",
            {"sd_xl_turbo_1.0.safetensors", "sd_xl_turbo_1.0_fp16.safetensors",
             "unet/diffusion_pytorch_model.safetensors", "model_index.json"},
            {{"pipeline_tag", "text-to-image"},
             {"tags", {"diffusers", "diffusers:StableDiffusionXLPipeline"}}});
        result.expect("stable-diffusion.cpp offers root checkpoints, not the diffusers parts",
                      analysis.match && analysis.match->repo_kind == "single-file" &&
                          analysis.match->variants.size() == 2 &&
                          analysis.match->variants[1].name == "sd_xl_turbo_1.0_fp16.safetensors",
                      "");
    }
    {
        auto analysis = analyze(
            "sd-cpp",
            {"v1-5-pruned-emaonly.ckpt", "v1-5-pruned-emaonly.safetensors", "v1-5-pruned.ckpt"},
            {{"pipeline_tag", "text-to-image"}, {"tags", {"diffusers", "stable-diffusion"}}});
        result.expect("a checkpoint in both formats is offered once, as .safetensors",
                      analysis.match && analysis.match->variants.size() == 2 &&
                          analysis.match->variants[0].primary_file ==
                              "v1-5-pruned-emaonly.safetensors" &&
                          analysis.match->variants[1].primary_file == "v1-5-pruned.ckpt",
                      "");
    }
    {
        auto analysis = analyze("sd-cpp", {"sd_turbo-f16-q8_0.gguf", "sd_turbo-f16.gguf", "README.md"},
                                {{"tags", {"gguf", "sd.cpp", "stable-diffusion.cpp", "sd2"}}});
        result.expect("stable-diffusion.cpp takes a GGUF repository tagged for it without a task",
                      analysis.match && analysis.match->variants.size() == 2 &&
                          analysis.match->variants[0].name == "sd_turbo-f16-q8_0.gguf",
                      "");
    }
    {
        auto analysis = analyze(
            "sd-cpp",
            {"v1-5-pruned-emaonly.safetensors", "vae-ft-mse-840000.safetensors"},
            {{"pipeline_tag", "text-to-image"}, {"tags", {"stable-diffusion"}}});
        result.expect("a VAE next to an SD checkpoint is not offered as a model",
                      analysis.match && analysis.match->variants.size() == 1 &&
                          analysis.match->variants[0].name == "v1-5-pruned-emaonly.safetensors",
                      "");
    }
    {
        auto sd3 = analyze(
            "sd-cpp", {"sd3.5_medium.safetensors"},
            {{"pipeline_tag", "text-to-image"},
             {"tags", {"diffusers", "stable-diffusion", "diffusers:StableDiffusion3Pipeline"}}});
        auto quantized_sd3 = analyze(
            "sd-cpp",
            {"clip_g-Q8_0.gguf", "clip_l-Q8_0.gguf", "sd3.5_medium-Q8_0.gguf", "t5xxl-Q8_0.gguf"},
            {{"pipeline_tag", "text-to-image"},
             {"tags", {"stable-diffusion",
                       "base_model:quantized:stabilityai/stable-diffusion-3.5-medium"}}});
        auto flux = analyze(
            "sd-cpp", {"ae.safetensors", "flux1-schnell.safetensors"},
            {{"pipeline_tag", "text-to-image"},
             {"tags", {"diffusers", "flux", "diffusers:FluxPipeline", "stable-diffusion"}}});
        result.expect("FLUX and SD3 repositories, which need separate encoders, are declined",
                      !sd3.match && !quantized_sd3.match && !flux.match, "");
    }
    {
        auto untagged = analyze("sd-cpp", {"model.safetensors"},
                                {{"pipeline_tag", "text-to-image"}, {"tags", {"diffusers"}}});
        auto llm = analyze("sd-cpp", {"Qwen3-0.6B-Q4_K_M.gguf"},
                           {{"pipeline_tag", "text-generation"}});
        result.expect("stable-diffusion.cpp declines repositories without stable-diffusion signals",
                      !untagged.match && !llm.match, "");
    }

    {
        auto analysis = analyze(
            "sd-cpp",
            {"sd_xl_base_1.0.safetensors", "sd_xl_offset_example-lora_1.0.safetensors",
             "taesdxl.safetensors", "controlnet-canny-sdxl.safetensors"},
            {{"pipeline_tag", "text-to-image"},
             {"tags", {"diffusers", "diffusers:StableDiffusionXLPipeline"}}});
        result.expect("LoRA, TAESD and ControlNet files are not offered as models",
                      analysis.match && analysis.match->variants.size() == 1 &&
                          analysis.match->variants[0].name == "sd_xl_base_1.0.safetensors",
                      "");
    }

    printf("\n%d passed, %d failed\n", result.passed, result.failed);
    return result.failed == 0 ? 0 : 1;
}
