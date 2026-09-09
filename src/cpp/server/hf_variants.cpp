#include "lemon/hf_variants.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <regex>
#include <stdexcept>
#include <unordered_map>

#include "lemon/backends/backend_descriptor_registry.h"
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

std::string filename_only(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

enum class DraftKind { None, Mtp, Dflash };

DraftKind draft_kind(const std::string& path) {
    const std::string filename = to_lower(filename_only(path));
    if (filename.rfind("mtp-", 0) == 0) return DraftKind::Mtp;
    if (filename.rfind("dflash-", 0) == 0 || filename == "dflash.gguf") {
        return DraftKind::Dflash;
    }
    return DraftKind::None;
}

bool is_draft_companion(const std::string& path) {
    return draft_kind(path) != DraftKind::None;
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

int quant_bits(const std::string& value) {
    std::string quant;
    if (!extract_quant(filename_only(value), quant)) return 0;
    size_t pos = quant.find_first_of("0123456789");
    return pos == std::string::npos ? 0 : std::stoi(quant.substr(pos));
}

std::vector<std::string> directory_parts(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return {};

    std::vector<std::string> parts;
    size_t start = 0;
    while (start < slash) {
        size_t next = path.find('/', start);
        if (next == std::string::npos || next > slash) next = slash;
        parts.push_back(path.substr(start, next - start));
        start = next + 1;
    }
    return parts;
}

size_t shared_directory_depth(const std::string& model, const std::string& companion) {
    const auto model_parts = directory_parts(model);
    const auto companion_parts = directory_parts(companion);
    const size_t count = std::min(model_parts.size(), companion_parts.size());
    size_t depth = 0;
    while (depth < count && model_parts[depth] == companion_parts[depth]) ++depth;
    return depth;
}

// Mirrors llama.cpp's generic HF sidecar ranking rather than tuning individual
// models: prefer a deeper shared directory, then an exact quant tag, then the
// closest quant bit width. A repo exposing both MTP and DFlash remains
// intentionally ambiguous because choosing a speculative mechanism is policy.
std::string preferred_draft_companion(
    const GgufVariant& variant,
    const std::vector<std::string>& draft_paths) {
    DraftKind kind = DraftKind::None;
    for (const auto& path : draft_paths) {
        const DraftKind current = draft_kind(path);
        if (current == DraftKind::None) continue;
        if (kind == DraftKind::None) {
            kind = current;
        } else if (kind != current) {
            return {};
        }
    }
    if (kind == DraftKind::None) return {};

    std::string target_quant = variant.quant;
    if (target_quant.empty()) {
        extract_quant(filename_only(variant.primary_file), target_quant);
    }
    const int target_bits = quant_bits(target_quant);

    std::string best;
    size_t best_depth = 0;
    bool best_exact = false;
    int best_diff = 0;
    for (const auto& path : draft_paths) {
        if (draft_kind(path) != kind) continue;

        const std::string draft_filename = filename_only(path);
        std::string draft_quant;
        const bool has_quant = extract_quant(draft_filename, draft_quant);
        const bool exact = !target_quant.empty() && has_quant && draft_quant == target_quant;
        const int draft_bits = has_quant ? quant_bits(draft_filename) : 0;
        // With an unquantized main there is no meaningful bit-distance target.
        // Keep selection deterministic through the existing depth/path ordering
        const int diff = target_bits > 0 ? std::abs(draft_bits - target_bits) : 0;
        const size_t depth = shared_directory_depth(variant.primary_file, path);

        if (best.empty() || depth > best_depth ||
            (depth == best_depth && exact && !best_exact) ||
            (depth == best_depth && exact == best_exact && diff < best_diff) ||
            (depth == best_depth && exact == best_exact && diff == best_diff && path < best)) {
            best = path;
            best_depth = depth;
            best_exact = exact;
            best_diff = diff;
        }
    }
    return best;
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

    // Companion GGUFs must not become selectable main-model variants.
    // Keep repository-relative draft paths internally for variant association;
    // draft_files remains the legacy bare-filename list for API compatibility.
    std::vector<std::string> gguf_files;
    std::vector<std::string> draft_paths;
    for (const auto& f : repo_files) {
        std::string f_lower = to_lower(f);
        if (!ends_with(f_lower, ".gguf")) continue;
        size_t slash = f.find_last_of('/');
        std::string bare = slash == std::string::npos ? f : f.substr(slash + 1);
        if (f_lower.find("mmproj") != std::string::npos) {
            result.mmproj_files.push_back(bare);
        } else if (is_draft_companion(f)) {
            result.draft_files.push_back(bare);
            draft_paths.push_back(f);
        } else {
            gguf_files.push_back(f);
        }
    }
    std::sort(result.mmproj_files.begin(), result.mmproj_files.end());
    std::sort(result.draft_files.begin(), result.draft_files.end());
    std::sort(draft_paths.begin(), draft_paths.end());

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
        bool has_quant = extract_quant(folder, q);
        v.name = has_quant ? q : folder;
        v.quant = has_quant ? q : "";
        v.files = files;
        v.primary_file = files.front();
        v.sharded = true;
        for (const auto& f : files) v.size_bytes += size_of(f);
        result.variants.push_back(std::move(v));
    }

    // Root-file variants. Files are merged into one sharded variant only when
    // their names declare the same shard series, like
    // `model-Q4_K_M-00001-of-00003.gguf`. Sharing a quant token is not enough:
    // `Model-Q4_K_M.gguf` and `Model-Q4_K_M-imatrix.gguf` are separate models.
    static const std::regex shard_re(R"(^(.+)[-._]\d{5}-of-\d{5}\.gguf$)", std::regex::icase);
    std::sort(root_files.begin(), root_files.end());
    std::map<std::string, std::vector<std::string>> root_by_series;
    std::map<std::string, std::string> quant_of_series;
    std::map<std::string, int> series_per_quant;
    std::vector<std::string> root_unmatched;
    for (const auto& f : root_files) {
        std::string q;
        if (!extract_quant(f, q)) {
            root_unmatched.push_back(f);
            continue;
        }
        std::smatch m;
        std::string key = std::regex_match(f, m, shard_re) ? m[1].str() : f;
        if (root_by_series.find(key) == root_by_series.end()) {
            quant_of_series[key] = q;
            series_per_quant[q]++;
        }
        root_by_series[key].push_back(f);
    }
    for (auto& kv : root_by_series) {
        const std::string& quant = quant_of_series[kv.first];
        GgufVariant v;
        // Two variants can now share a quant token, so fall back to the primary
        // file's stem to keep every name in the list distinct. The quant is kept
        // separately: widening the name must not change where this variant sorts.
        v.name = series_per_quant[quant] == 1
                     ? quant
                     : kv.second.front().substr(0, kv.second.front().find_last_of('.'));
        v.quant = quant;
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

    // Sort by quant priority then name. Ordering reads `quant`, never `name`:
    // `name` widens to a file stem when two variants share a quant, and a stem
    // is not a key in the priority table, so sorting on it silently demoted
    // every disambiguated variant into the "everything else" bucket — putting
    // Q8_0 ahead of Q4_K_M and making `pull --yes` take the wrong default.
    std::sort(result.variants.begin(), result.variants.end(),
              [](const GgufVariant& a, const GgufVariant& b) {
                  int pa = a.quant.empty() ? 100 : quant_priority(a.quant);
                  int pb = b.quant.empty() ? 100 : quant_priority(b.quant);
                  if (pa != pb) return pa < pb;
                  return a.name < b.name;
              });

    for (auto& variant : result.variants) {
        variant.draft_file = preferred_draft_companion(variant, draft_paths);
    }

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

        std::vector<std::string> labels;
        backends::ensure_deployment_label(labels, "ryzenai-llm");

        nlohmann::json out;
        out["checkpoint"] = checkpoint;
        out["source"] = remote_registry_source_name(source);
        out["recipe"] = "ryzenai-llm";
        out["repo_kind"] = "onnx-ryzenai";
        out["suggested_name"] = suggested_name;
        out["suggested_labels"] = labels;
        out["mmproj_files"] = nlohmann::json::array();
        out["draft_files"] = nlohmann::json::array();
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

        std::vector<std::string> labels;
        backends::ensure_deployment_label(labels, "collection.omni");

        // Inspection result only: what it is, its name, and (for the CLI's
        // display line) its size and component count. The manifest content is
        // deliberately omitted — /pull re-downloads it to disk.
        nlohmann::json out;
        out["checkpoint"] = checkpoint;
        out["source"] = remote_registry_source_name(source);
        out["recipe"] = "collection.omni";
        out["repo_kind"] = "collection";
        out["suggested_name"] = suggested_name;
        out["suggested_labels"] = labels;
        out["mmproj_files"] = nlohmann::json::array();
        out["draft_files"] = nlohmann::json::array();
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

    // Suggested labels. These are what the client previews before confirming
    // the pull, so they run through the same stamper that /pull applies at
    // registration; otherwise the preview and the registered model disagree.
    std::vector<std::string> labels;
    if (!vset.mmproj_files.empty()) add_label_once(labels, "vision");
    const auto draft_variant = std::find_if(
        vset.variants.begin(), vset.variants.end(),
        [](const GgufVariant& variant) { return !variant.draft_file.empty(); });
    if (draft_variant != vset.variants.end()) {
        switch (draft_kind(draft_variant->draft_file)) {
            case DraftKind::Mtp: add_label_once(labels, "mtp"); break;
            case DraftKind::Dflash: add_label_once(labels, "dflash"); break;
            case DraftKind::None: break;
        }
    }
    {
        std::string id_lower = to_lower(checkpoint);
        if (id_lower.find("embed") != std::string::npos) add_label_once(labels, "embeddings");
        if (id_lower.find("rerank") != std::string::npos) add_label_once(labels, "reranking");
    }
    backends::ensure_deployment_label(labels, "llamacpp");

    nlohmann::json out;
    out["checkpoint"] = checkpoint;
    out["source"] = remote_registry_source_name(source);
    out["recipe"] = "llamacpp";
    out["repo_kind"] = "gguf";
    out["suggested_name"] = suggested_name;
    out["suggested_labels"] = labels;
    out["mmproj_files"] = vset.mmproj_files;
    out["draft_files"] = vset.draft_files;

    nlohmann::json variants_json = nlohmann::json::array();
    for (const auto& v : vset.variants) {
        nlohmann::json vj;
        vj["name"] = v.name;
        vj["primary_file"] = v.primary_file;
        if (!v.draft_file.empty()) vj["draft_file"] = v.draft_file;
        vj["files"] = v.files;
        vj["sharded"] = v.sharded;
        vj["size_bytes"] = v.size_bytes;
        variants_json.push_back(std::move(vj));
    }
    out["variants"] = std::move(variants_json);
    return out;
}

}  // namespace lemon
