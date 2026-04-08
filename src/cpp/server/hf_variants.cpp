#include "lemon/hf_variants.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <regex>
#include <stdexcept>
#include <unordered_map>

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

// Maximum number of variants returned by the endpoint. The CLI menu
// supplements this with a free-text "type any variant" option for users who
// need a quant that didn't make the top-N cut.
constexpr size_t kMaxVariants = 5;

}  // namespace

GgufVariantSet enumerate_gguf_variants(
    const std::vector<std::string>& repo_files,
    const std::vector<std::pair<std::string, uint64_t>>& file_sizes,
    size_t max_variants) {
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

    // Cap to the top-N variants. The CLI surfaces a free-text option so the
    // user can still install any quant they like, even if it didn't make the
    // shortlist.
    if (max_variants > 0 && result.variants.size() > max_variants) {
        result.variants.resize(max_variants);
    }

    return result;
}

nlohmann::json fetch_pull_variants(const std::string& checkpoint, bool& not_found) {
    not_found = false;

    if (checkpoint.empty() || checkpoint.find('/') == std::string::npos) {
        throw std::runtime_error("checkpoint must be a Hugging Face repo id of the form 'owner/name'");
    }

    std::map<std::string, std::string> headers;
    const char* hf_token = std::getenv("HF_TOKEN");
    if (hf_token && hf_token[0]) {
        headers["Authorization"] = "Bearer " + std::string(hf_token);
    }

    // `?blobs=true` makes HF include per-file `size` in each siblings entry,
    // which we forward to the variant set so the CLI menu can show real sizes.
    std::string url = "https://huggingface.co/api/models/" + checkpoint + "?blobs=true";
    auto response = HttpClient::get(url, headers);
    // HuggingFace returns 401 (not 404) for nonexistent public repos to mimic
    // the behavior of gated repos. Treat both as "not found" from the user's
    // perspective so the CLI can give a clean error message.
    if (response.status_code == 404 || response.status_code == 401) {
        not_found = true;
        return {};
    }
    if (response.status_code != 200) {
        throw std::runtime_error(
            "Hugging Face API returned status " + std::to_string(response.status_code));
    }

    auto info = JsonUtils::parse(response.body);
    if (!info.contains("siblings") || !info["siblings"].is_array()) {
        throw std::runtime_error("Hugging Face response missing 'siblings' array");
    }

    std::vector<std::string> repo_files;
    std::vector<std::pair<std::string, uint64_t>> file_sizes;
    for (const auto& s : info["siblings"]) {
        if (!s.contains("rfilename")) continue;
        std::string name = s["rfilename"].get<std::string>();
        repo_files.push_back(name);
        if (s.contains("size") && s["size"].is_number()) {
            file_sizes.emplace_back(name, s["size"].get<uint64_t>());
        }
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
        out["recipe"] = "ryzenai-llm";
        out["repo_kind"] = "onnx-ryzenai";
        out["suggested_name"] = suggested_name;
        out["suggested_labels"] = nlohmann::json::array();
        out["mmproj_files"] = nlohmann::json::array();
        out["variants"] = nlohmann::json::array({vj});
        return out;
    }

    auto vset = enumerate_gguf_variants(repo_files, file_sizes, kMaxVariants);
    if (vset.variants.empty()) {
        throw std::runtime_error("No supported model files (.gguf or ONNX RyzenAI) found in repository " + checkpoint);
    }

    // Suggested labels.
    std::vector<std::string> labels;
    if (!vset.mmproj_files.empty()) labels.push_back("vision");
    {
        std::string id_lower = to_lower(checkpoint);
        if (id_lower.find("embed") != std::string::npos) labels.push_back("embeddings");
        if (id_lower.find("rerank") != std::string::npos) labels.push_back("reranking");
    }

    nlohmann::json out;
    out["checkpoint"] = checkpoint;
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
