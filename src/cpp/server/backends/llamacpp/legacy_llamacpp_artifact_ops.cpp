#include "legacy_llamacpp_artifact_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/llamacpp/llamacpp_gguf.h"
#include "lemon/gguf_capabilities.h"
#include "lemon/gguf_reader.h"
#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace lemon::backends::llamacpp {

void LegacyLlamaCppArtifactOps::populate_metadata(ModelInfo& info, const BackendOpsContext&) const {
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
    if (info.type == ModelType::LLM) {
        apply_gguf_capability_labels(info.labels, info.gguf.caps);
    }
}

std::string LegacyLlamaCppArtifactOps::resolve_checkpoint_path(const ModelInfo& info,
                                    const CheckpointResolveContext& ctx) const {
    if (ctx.type == "main") {
        return resolve_gguf_path(ctx.model_cache_path, ctx.variant);
    }
    return BackendOps::resolve_checkpoint_path(info, ctx);
}

std::string LegacyLlamaCppArtifactOps::find_imported_checkpoint(const std::string& import_dir) const {
    return resolve_gguf_path(import_dir, "");
}

std::string LegacyLlamaCppArtifactOps::validate_registration_checkpoint(const std::string& checkpoint) const {
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

std::string LegacyLlamaCppArtifactOps::validate_checkpoint_file(const std::string& resolved_path) const {
    std::error_code ec;
    std::filesystem::path p = lemon::utils::path_from_utf8(resolved_path);
    if (std::filesystem::is_directory(p, ec)) {
        return "";
    }
    std::string ext = resolved_path.size() >= 5 ? resolved_path.substr(resolved_path.size() - 5) : "";
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != ".gguf") {
        return "";
    }
    std::ifstream in(p, std::ios::binary);
    char magic[4] = {};
    in.read(magic, sizeof(magic));
    bool ok = in.gcount() == static_cast<std::streamsize>(sizeof(magic)) &&
              magic[0] == 'G' && magic[1] == 'G' && magic[2] == 'U' && magic[3] == 'F';
    return ok ? "" : "Invalid GGUF cache file";
}

const BackendOps* legacy_llamacpp_artifact_ops() {
    static const LegacyLlamaCppArtifactOps ops;
    return &ops;
}

}
