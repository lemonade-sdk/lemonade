#include "../backend_typed_hook_registry_internal.h"
#include "lemon/hf_variants.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <stdexcept>

namespace lemon::backends::typed::llamacpp {
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool gguf(const std::string& value) {
    return value.size() >= 5 && lower(value.substr(value.size() - 5)) == ".gguf";
}

std::string filename(const std::string& value) {
    const auto separator = value.find_last_of("/\\");
    return separator == std::string::npos ? value : value.substr(separator + 1);
}

bool confined(const std::string& value) {
    if (value.empty() || value.front() == '/' || value.find('\\') != std::string::npos ||
        value.find(':') != std::string::npos || value.find('\0') != std::string::npos) return false;
    std::size_t start = 0;
    do {
        const auto end = value.find('/', start);
        const auto part = value.substr(start, end == std::string::npos ? end : end - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (end == std::string::npos) break;
        start = end + 1;
    } while (true);
    return true;
}

std::string select(std::vector<ArtifactEntry> entries, const std::string& variant, const ArtifactRootInfo& root) {
    if (!root.exists) return root.path;
    if (root.snapshots && !root.active_snapshot) return "";
    entries.erase(std::remove_if(entries.begin(), entries.end(), [](const auto& entry) {
        return !entry.complete || !confined(entry.relative_path) || !gguf(entry.relative_path) ||
            lower(filename(entry.relative_path)).find("mmproj") != std::string::npos;
    }), entries.end());
    if (entries.empty()) return root.snapshots ? "" : root.path;
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return a.relative_path < b.relative_path;
    });
    const auto match = [&](bool active_only) -> std::string {
        std::vector<const ArtifactEntry*> candidates;
        for (const auto& entry : entries) if (!active_only || entry.active) candidates.push_back(&entry);
        if (candidates.empty()) return "";
        if (variant.empty() || variant == "*") return candidates.front()->resolved_path;
        if (variant.find(".gguf") != std::string::npos) {
            for (const auto* entry : candidates) if (filename(entry->relative_path) == variant) return entry->resolved_path;
            return "";
        }
        const auto suffix = lower(variant) + ".gguf";
        for (const auto* entry : candidates) {
            const auto name = lower(filename(entry->relative_path));
            if (name.size() >= suffix.size() && name.substr(name.size() - suffix.size()) == suffix) return entry->resolved_path;
        }
        for (const auto* entry : candidates) {
            const auto path = "/" + lower(entry->relative_path);
            if (path.find("/" + lower(variant) + "/") != std::string::npos) return entry->resolved_path;
        }
        std::vector<std::string> paths;
        std::map<std::string, std::string> resolved;
        for (const auto* entry : candidates) {
            auto relative = entry->relative_path;
            if (relative.rfind("snapshots/", 0) == 0) {
                const auto end = relative.find('/', 10);
                if (end != std::string::npos) relative = relative.substr(end + 1);
            }
            if (resolved.emplace(relative, entry->resolved_path).second) paths.push_back(relative);
        }
        std::vector<std::string> matches;
        for (const auto& item : enumerate_gguf_variants(paths).variants) {
            if (lower(item.name) == lower(variant)) matches.push_back(resolved.at(item.primary_file));
        }
        return matches.size() == 1 ? matches.front() : "";
    };
    const auto active = match(true);
    return active.empty() ? match(false) : active;
}

}

ArtifactMetadata metadata(const ArtifactMetadataInput& input, ArtifactReadServices& reads) {
    LEMON_TYPED_ENTRY("artifacts.metadata");
    ArtifactMetadata result{std::nullopt, input.labels};
    if (!gguf(input.filename)) return result;
    result.header = reads.header(input.file);
    if (result.header && input.model_type == ModelType::LLM) {
        apply_gguf_capability_labels(result.labels, result.header->capabilities);
    }
    return result;
}

std::string validate_registration(const std::string& checkpoint) {
    LEMON_TYPED_ENTRY("artifacts.validate_registration");
    if (lower(checkpoint).find("gguf") != std::string::npos && checkpoint.find(':') == std::string::npos) {
        return "You are required to provide a 'variant' in the checkpoint field when "
            "registering a GGUF model. The variant is provided as CHECKPOINT:VARIANT. "
            "For example: Qwen/Qwen2.5-Coder-3B-Instruct-GGUF:Q4_0 or "
            "Qwen/Qwen2.5-Coder-3B-Instruct-GGUF:qwen2.5-coder-3b-instruct-q4_0.gguf";
    }
    return "";
}

std::string validate_file(const ArtifactFileInput& input, ArtifactReadServices& reads) {
    LEMON_TYPED_ENTRY("artifacts.validate_file");
    if (input.directory || !gguf(input.filename)) return "";
    return reads.prefix(input.file, 4) == "GGUF" ? "" : "Invalid GGUF cache file";
}

std::string resolve_path(const ArtifactResolveInput& input, ArtifactReadServices& reads) {
    LEMON_TYPED_ENTRY("artifacts.resolve_path");
    const auto entries = reads.entries(input.root);
    if (input.role == "main") return select(entries, input.variant, reads.root_info(input.root));
    if (input.role != "mmproj" && input.role != "draft") throw std::logic_error("unknown artifact role");
    for (const auto& entry : entries) {
        if (entry.complete && confined(entry.relative_path) && (entry.relative_path == input.variant || (entry.active && entry.relative_path.substr(entry.relative_path.find('/', 10) + 1) == input.variant))) return entry.resolved_path;
    }
    return "";
}

std::string find_imported(const ArtifactImportInput& input, ArtifactReadServices& reads) {
    LEMON_TYPED_ENTRY("artifacts.find_imported");
    return select(reads.entries(input.root), "", reads.root_info(input.root));
}

}
