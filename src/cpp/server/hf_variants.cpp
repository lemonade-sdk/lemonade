#include "lemon/hf_variants.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <regex>
#include <stdexcept>
#include <unordered_map>

#include "lemon/model_types.h"
#include "lemon/model_registry.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/json_utils.h"

namespace lemon {

namespace {

using lemon::utils::HttpClient;
using lemon::utils::JsonUtils;

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool contains_ci(const std::string& s, const std::string& needle) {
    return to_lower(s).find(to_lower(needle)) != std::string::npos;
}

// Quant token extractor. Recognizes the variants we actually see in
// production GGUF repos (see src/cpp/resources/server_models.json), including
// the `UD-` Unsloth Dynamic prefix and the `_XL` "extra-large" suffix that
// the previous regex stripped off and silently lumped together with `Q*_K_M`.
//
// The token is anchored on a separator (`-._/` or start of string) and must
// be followed by another separator, a slash, the file extension, or end of
// string, so it doesn't match parts of arbitrary identifiers.
const std::regex& quant_regex() {
    static const std::regex re(
        R"((?:^|[-._/])((?:UD[-_])?(?:Q\d+(?:_\d)?(?:_K)?(?:_(?:M|S|L|XL|XXL))?|IQ\d+(?:_(?:M|S|L|XS|XXS|NL))?|F(?:16|32)|BF16|MXFP\d+(?:_MOE)?))(?=[-._/]|\.gguf$|$))",
        std::regex::icase);
    return re;
}

std::string normalize_quant(std::string q) {
    std::transform(q.begin(), q.end(), q.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return q;
}

// Try to extract a quant token from `s`. Returns true on success.
bool extract_quant(const std::string& s, std::string& out) {
    std::smatch m;
    if (!std::regex_search(s, m, quant_regex())) return false;
    out = normalize_quant(m[1].str());
    return true;
}

// Priority of a quant for sorting. Lower is better. The list is the quants
// that appear three or more times in src/cpp/resources/server_models.json,
// in frequency order. Anything else falls into a single "everything else"
// bucket sorted lexicographically.
int quant_priority(const std::string& q) {
    static const std::map<std::string, int> priority = {
        {"Q4_K_M",      1},  // 23 occurrences
        {"UD-Q4_K_XL",  2},  // 14
        {"Q8_0",        3},  //  7
        {"Q4_0",        4},  //  6
    };
    auto it = priority.find(q);
    return it == priority.end() ? 100 : it->second;
}

void add_label(std::vector<std::string>& labels, const std::string& label) {
    if (std::find(labels.begin(), labels.end(), label) == labels.end()) {
        labels.push_back(label);
    }
}

}  // namespace

GgufVariantSet enumerate_gguf_variants(
    const std::vector<std::string>& repo_files,
    const std::vector<std::pair<std::string, uint64_t>>& file_sizes) {
    GgufVariantSet result;

    std::unordered_map<std::string, uint64_t> size_by_file;
    for (const auto& kv : file_sizes) {
        size_by_file[kv.first] = kv.second;
    }
    auto size_of = [&](const std::string& f) -> uint64_t {
        auto it = size_by_file.find(f);
        return it == size_by_file.end() ? 0 : it->second;
    };

    // Partition into mmproj vs regular gguf files.
    std::vector<std::string> gguf_files;
    for (const auto& f : repo_files) {
        std::string f_lower = to_lower(f);
        if (!ends_with(f_lower, ".gguf")) continue;
        if (f_lower.find("mmproj") != std::string::npos) {
            size_t slash = f.find_last_of('/');
            result.mmproj_files.push_back(slash == std::string::npos ? f : f.substr(slash + 1));
        } else {
            gguf_files.push_back(f);
        }
    }
    std::sort(result.mmproj_files.begin(), result.mmproj_files.end());

    // Group by top-level folder vs root files.
    std::map<std::string, std::vector<std::string>> folder_groups;
    std::vector<std::string> root_files;
    for (const auto& f : gguf_files) {
        size_t slash = f.find('/');
        if (slash != std::string::npos && slash > 0) {
            folder_groups[f.substr(0, slash)].push_back(f);
        } else {
            root_files.push_back(f);
        }
    }

    // Folder-based (sharded) variants. The folder name itself is the
    // quant container; we try to extract a recognizable token, otherwise
    // we keep the folder name verbatim as the variant name.
    for (auto& kv : folder_groups) {
        const std::string& folder = kv.first;
        auto& files = kv.second;
        std::sort(files.begin(), files.end());

        GgufVariant v;
        std::string q;
        v.name = extract_quant(folder, q) ? q : folder;
        v.files = files;
        v.primary_file = files.front();
        v.sharded = true;
        for (const auto& f : files) v.size_bytes += size_of(f);
        result.variants.push_back(std::move(v));
    }

    // Root-file variants, grouped by extracted quant token. Files that share
    // the same token (typically multi-shard root files like
    // `model-Q4_K_M-00001-of-00003.gguf`) are merged into one sharded variant.
    std::sort(root_files.begin(), root_files.end());
    std::map<std::string, std::vector<std::string>> root_by_quant;
    std::vector<std::string> root_unmatched;
    for (const auto& f : root_files) {
        std::string q;
        if (extract_quant(f, q)) {
            root_by_quant[q].push_back(f);
        } else {
            root_unmatched.push_back(f);
        }
    }
    for (auto& kv : root_by_quant) {
        GgufVariant v;
        v.name = kv.first;
        v.files = kv.second;
        v.primary_file = kv.second.front();
        v.sharded = kv.second.size() > 1;
        for (const auto& f : kv.second) v.size_bytes += size_of(f);
        result.variants.push_back(std::move(v));
    }

    // Fall back to filename-named variants if nothing matched the regex (e.g.
    // a repo with a single `model.gguf` and no quant suffix at all).
    bool nothing_matched = result.variants.empty();
    if (nothing_matched) {
        for (const auto& f : root_unmatched) {
            GgufVariant v;
            v.name = f;
            v.primary_file = f;
            v.files = {f};
            v.size_bytes = size_of(f);
            result.variants.push_back(std::move(v));
        }
    }

    // Sort by quant priority then name.
    std::sort(result.variants.begin(), result.variants.end(),
              [](const GgufVariant& a, const GgufVariant& b) {
                  int pa = quant_priority(a.name);
                  int pb = quant_priority(b.name);
                  if (pa != pb) return pa < pb;
                  return a.name < b.name;
              });

    return result;
}

nlohmann::json fetch_pull_variants(const std::string& checkpoint,
                                   const std::string& registry_source,
                                   bool& not_found) {
    not_found = false;

    if (checkpoint.empty() || checkpoint.find('/') == std::string::npos) {
        throw std::runtime_error("checkpoint must be a repository id of the form 'owner/name'");
    }

    const auto source = parse_remote_registry_source(registry_source);
    const auto& registry = model_registry(source);
    RegistryRepository repository;
    try {
        repository = registry.fetch_repository(checkpoint);
    } catch (const RegistryNotFoundError&) {
        not_found = true;
        return {};
    }

    const auto headers = registry.auth_headers();
    std::vector<std::string> repo_files;
    std::vector<std::pair<std::string, uint64_t>> file_sizes;
    for (const auto& file : repository.files) {
        if (file.directory) continue;
        repo_files.push_back(file.path);
        if (file.size > 0) file_sizes.emplace_back(file.path, file.size);
    }

    // Detect repo kind: GGUF (existing path) vs ONNX RyzenAI vs unknown.
    // ONNX RyzenAI repos contain `.onnx` files plus a `genai_config.json`
    // (the OGA runtime config). They are pulled as a single unit — no
    // sub-variant selection — and use the `ryzenai-llm` recipe.
    bool has_gguf = false;
    bool has_onnx = false;
    bool has_genai_config = false;
    uint64_t total_repo_size = 0;
    for (const auto& f : repo_files) {
        std::string lf = to_lower(f);
        if (ends_with(lf, ".gguf")) has_gguf = true;
        if (ends_with(lf, ".onnx")) has_onnx = true;
        if (lf == "genai_config.json" ||
            (lf.size() > 18 && ends_with(lf, "/genai_config.json"))) {
            has_genai_config = true;
        }
    }
    for (const auto& kv : file_sizes) total_repo_size += kv.second;

    // Suggested name = component after the last '/'.
    std::string suggested_name = checkpoint;
    {
        size_t slash = checkpoint.find_last_of('/');
        if (slash != std::string::npos) suggested_name = checkpoint.substr(slash + 1);
    }

    // ONNX RyzenAI branch: synthesize a 1-element variant set.
    if (!has_gguf && has_onnx && has_genai_config) {
        nlohmann::json vj;
        vj["name"] = "default";
        vj["primary_file"] = "genai_config.json";
        vj["files"] = repo_files;
        vj["sharded"] = repo_files.size() > 1;
        vj["size_bytes"] = total_repo_size;

        nlohmann::json out;
        out["checkpoint"] = checkpoint;
        out["source"] = remote_registry_source_name(source);
        out["recipe"] = "ryzenai-llm";
        out["repo_kind"] = "onnx-ryzenai";
        out["suggested_name"] = suggested_name;
        out["suggested_labels"] = nlohmann::json::array();
        out["mmproj_files"] = nlohmann::json::array();
        out["variants"] = nlohmann::json::array({vj});
        return out;
    }

    // Omni collection branch. This function only INSPECTS the repo and reports
    // what it is — it never downloads anything to disk. A repo published from
    // `lemonade export` carries its collection manifest as <RepoName>.json (see
    // "Share a collection" in the custom models guide). The filename is the
    // discovery key; the content is read here only to confirm it really is a
    // collection manifest (a same-named file that is not a manifest is an error,
    // not a fall-through). The manifest itself is NOT returned — /pull downloads
    // it to disk as the actual pull step, exactly as the .gguf is pulled for a
    // regular model.
    const std::string manifest_filename = suggested_name + ".json";
    if (!has_gguf &&
        std::find(repo_files.begin(), repo_files.end(), manifest_filename) != repo_files.end()) {
        // Manifests are small (KBs); refuse to inspect absurdly large files.
        static constexpr uint64_t kMaxManifestBytes = 5ull * 1024 * 1024;
        for (const auto& kv : file_sizes) {
            if (kv.first == manifest_filename && kv.second > kMaxManifestBytes) {
                throw std::runtime_error(
                    "Repository " + checkpoint + " contains '" + manifest_filename +
                    "' but it is too large to be a Lemonade Omni collection manifest");
            }
        }

        std::string manifest_url = registry.resolve_file_url(
            checkpoint, repository.revision, manifest_filename);
        auto manifest_response = HttpClient::get(manifest_url, headers);
        if (manifest_response.status_code != 200) {
            throw std::runtime_error(
                "Failed to read '" + manifest_filename + "' from repository " + checkpoint +
                " (HTTP " + std::to_string(manifest_response.status_code) + ")");
        }

        nlohmann::json manifest;
        try {
            manifest = JsonUtils::parse(manifest_response.body);
        } catch (const std::exception&) {
            manifest = nlohmann::json();
        }
        bool valid_manifest = manifest.is_object() &&
            is_omni_collection_recipe(manifest.value("recipe", std::string())) &&
            manifest.contains("components") && manifest["components"].is_array() &&
            !manifest["components"].empty() &&
            manifest.contains("models") && manifest["models"].is_array();
        if (!valid_manifest) {
            throw std::runtime_error(
                "Repository " + checkpoint + " contains '" + manifest_filename +
                "' but it is not a Lemonade Omni collection manifest (expected an exported "
                "collection JSON with recipe \"collection.omni\", a non-empty 'components' "
                "array, and a 'models' array)");
        }

        // Inspection result only: what it is, its name, and (for the CLI's
        // display line) its size and component count. The manifest content is
        // deliberately omitted — /pull re-downloads it to disk.
        nlohmann::json out;
        out["checkpoint"] = checkpoint;
        out["source"] = remote_registry_source_name(source);
        out["recipe"] = "collection.omni";
        out["repo_kind"] = "collection";
        out["suggested_name"] = suggested_name;
        out["suggested_labels"] = nlohmann::json::array();
        out["mmproj_files"] = nlohmann::json::array();
        out["variants"] = nlohmann::json::array();
        if (manifest.contains("size") && manifest["size"].is_number()) {
            out["size"] = manifest["size"];
        }
        out["component_count"] = manifest["components"].size();
        return out;
    }

    auto vset = enumerate_gguf_variants(repo_files, file_sizes);
    if (vset.variants.empty()) {
        throw std::runtime_error(
            "No supported model files found in repository " + checkpoint +
            ". Supported repository types: GGUF models (*.gguf), ONNX RyzenAI models, and "
            "Lemonade Omni collections (a '" + manifest_filename +
            "' manifest exported by 'lemonade export')");
    }

    // Suggested labels.
    std::vector<std::string> labels;
    if (!vset.mmproj_files.empty()) add_label(labels, "vision");
    {
        std::string id_lower = to_lower(checkpoint);
        if (id_lower.find("embed") != std::string::npos) add_label(labels, "embeddings");
        if (id_lower.find("rerank") != std::string::npos) add_label(labels, "reranking");
    }

    nlohmann::json out;
    out["checkpoint"] = checkpoint;
    out["source"] = remote_registry_source_name(source);
    out["recipe"] = "llamacpp";
    out["repo_kind"] = "gguf";
    out["suggested_name"] = suggested_name;
    out["suggested_labels"] = labels;
    out["mmproj_files"] = vset.mmproj_files;

    nlohmann::json variants_json = nlohmann::json::array();
    for (const auto& v : vset.variants) {
        nlohmann::json vj;
        vj["name"] = v.name;
        vj["primary_file"] = v.primary_file;
        vj["files"] = v.files;
        vj["sharded"] = v.sharded;
        vj["size_bytes"] = v.size_bytes;
        variants_json.push_back(std::move(vj));
    }
    out["variants"] = std::move(variants_json);
    return out;
}

}  // namespace lemon
