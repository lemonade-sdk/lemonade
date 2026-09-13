#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "lemon/backends/backend_ops.h"
#include "lemon/backends/llamacpp/llamacpp_gguf.h"
#include "lemon/gguf_capabilities.h"
#include "lemon/gguf_reader.h"
#include "lemon/model_manager.h"
#include "lemon/utils/path_utils.h"

namespace lemon {
namespace backends {

// llama.cpp model-management behavior: context window and capability labels
// read out of the GGUF, sharded/variant checkpoint resolution, registration
// validation, the truncated-download check, and the runtime args a model
// implies.
//
// Templated on its base only so llamacpp-toolbox, which runs the same
// llama-server out of a container, can layer it over ContainerBackendOps and
// get both halves. Plain `LlamaCppOps` is the ordinary non-container case.
template <typename Base = BackendOps>
class LlamaCppOps : public Base {
public:
    using Base::Base;

    // The descriptor option this backend's custom llama-server arguments live
    // in. llamacpp-toolbox names its own.
    virtual std::string args_option_name() const { return "llamacpp_args"; }

    void resolve_runtime_options(const ModelInfo& info, RecipeOptions& options) const override {
        const nlohmann::json merge_args_value = options.get_option("merge_args");
        const bool merge_args =
            merge_args_value.is_boolean() ? merge_args_value.get<bool>() : true;
        const std::string option = args_option_name();
        const nlohmann::json custom_args_value = options.get_option(option);
        const std::string custom_args =
            custom_args_value.is_string() ? custom_args_value.get<std::string>() : "";
        options.set_option(
            option, llamacpp::resolve_llamacpp_runtime_args(info, custom_args, merge_args));
    }

    void populate_metadata(ModelInfo& info, const BackendOpsContext&) const override {
        const std::string gguf_path = info.resolved_path();
        if (gguf_path.size() < 5) {
            return;
        }
        std::string ext = gguf_path.substr(gguf_path.size() - 5);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".gguf") {
            return;
        }
        std::error_code ec;
        if (!std::filesystem::exists(lemon::utils::path_from_utf8(gguf_path), ec)) {
            return;
        }
        GgufMetadata meta;
        if (!read_gguf_metadata(meta, gguf_path)) {
            return;
        }
        info.max_context_window = meta.context_length;
        info.gguf = std::move(meta);
        // GGUF vision/tool metadata are LLM capabilities. Don't apply them to
        // embedding/reranking models, or labels like tool-calling would
        // reclassify the model away from its endpoint type.
        if (info.type == ModelType::LLM) {
            apply_gguf_capability_labels(info.labels, info.gguf.caps);
        }
    }

    std::string resolve_checkpoint_path(const ModelInfo& info,
                                        const CheckpointResolveContext& ctx) const override {
        // The main checkpoint is a GGUF file (with sharding/variant resolution);
        // auxiliary checkpoints (mmproj, …) use the shared default.
        if (ctx.type == "main") {
            return llamacpp::resolve_gguf_path(ctx.model_cache_path, ctx.variant);
        }
        return Base::resolve_checkpoint_path(info, ctx);
    }

    std::string find_imported_checkpoint(const std::string& import_dir) const override {
        // The primary artifact is the (non-mmproj) GGUF file.
        return llamacpp::resolve_gguf_path(import_dir, "");
    }

    std::string validate_registration_checkpoint(const std::string& checkpoint) const override {
        // A GGUF checkpoint must name its quant via CHECKPOINT:VARIANT.
        std::string lower = checkpoint;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find("gguf") != std::string::npos &&
            checkpoint.find(':') == std::string::npos) {
            return "You are required to provide a 'variant' in the checkpoint field when "
                   "registering a GGUF model. The variant is provided as CHECKPOINT:VARIANT. "
                   "For example: Qwen/Qwen2.5-Coder-3B-Instruct-GGUF:Q4_0 or "
                   "Qwen/Qwen2.5-Coder-3B-Instruct-GGUF:qwen2.5-coder-3b-instruct-q4_0.gguf";
        }
        return "";
    }

    std::string validate_checkpoint_file(const std::string& resolved_path) const override {
        // A .gguf file in the cache must start with the GGUF magic, else it's a
        // truncated/corrupt download and the model is not really present.
        std::error_code ec;
        std::filesystem::path p = lemon::utils::path_from_utf8(resolved_path);
        if (std::filesystem::is_directory(p, ec)) {
            return "";
        }
        std::string ext =
            resolved_path.size() >= 5 ? resolved_path.substr(resolved_path.size() - 5) : "";
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".gguf") {
            return "";
        }
        std::ifstream in(p, std::ios::binary);
        char magic[4] = {};
        in.read(magic, sizeof(magic));
        const bool ok = in.gcount() == static_cast<std::streamsize>(sizeof(magic)) &&
                        magic[0] == 'G' && magic[1] == 'G' && magic[2] == 'U' && magic[3] == 'F';
        return ok ? "" : "Invalid GGUF cache file";
    }

    // The two below concern the PATH-installed "system" llama-server, which only
    // the llamacpp recipe exposes. llamacpp-toolbox has no such variant and
    // overrides both back to its container behavior (digest, image pulled).

    std::string resolve_version(const std::string& backend,
                                const std::string& file_version) const override {
        // The PATH-installed "system" llama-server has no version.txt; query it.
        if (backend == "system") {
            return llamacpp::system_llamacpp_version();
        }
        return file_version;
    }

    typename Base::InstallCheck check_install(const std::string& backend,
                                              bool binary_found) const override {
        // The system llama-server also needs the ggml HIP plugin for ROCm GPU
        // acceleration when an AMD GPU (KFD) is present.
        if (binary_found && backend == "system") {
#ifdef __linux__
            if (std::filesystem::exists("/sys/class/kfd") &&
                !llamacpp::is_ggml_hip_plugin_available()) {
                return {false, "HIP plugin libggml-hip.so not installed"};
            }
#endif
        }
        return {binary_found, ""};
    }
};

}  // namespace backends
}  // namespace lemon
