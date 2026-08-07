#include "lemon/model_registry.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <initializer_list>
#include <sstream>
#include <utility>

#include "lemon/utils/http_client.h"
#include "lemon/utils/json_utils.h"

namespace lemon {
namespace {

using lemon::utils::HttpClient;
using lemon::utils::JsonUtils;
using json = nlohmann::json;

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool is_unreserved(unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

std::string percent_encode(const std::string& value, bool keep_slash = false) {
    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (unsigned char c : value) {
        if (is_unreserved(c) || (keep_slash && c == '/')) {
            out << static_cast<char>(c);
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return out.str();
}

std::string env_string(const char* name) {
    const char* value = std::getenv(name);
    return (value && value[0]) ? std::string(value) : std::string();
}

std::string trim_trailing_slash(std::string value) {
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

std::uint64_t fnv1a64(const std::string& text, std::uint64_t hash = 14695981039346656037ull) {
    for (unsigned char c : text) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string safe_revision_component(std::string revision) {
    if (revision.empty()) revision = "default";
    for (char& c : revision) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '-' && c != '_' && c != '.') c = '-';
    }
    if (revision.size() > 48) revision.resize(48);
    return revision;
}

std::string tree_snapshot_id(const std::string& prefix,
                             const std::string& revision,
                             std::vector<RegistryFile> files) {
    std::sort(files.begin(), files.end(), [](const RegistryFile& a, const RegistryFile& b) {
        return a.path < b.path;
    });
    std::uint64_t hash = fnv1a64(revision);
    for (const auto& file : files) {
        hash = fnv1a64(file.path, hash);
        hash = fnv1a64("\n" + std::to_string(file.size) + "\n", hash);
        hash = fnv1a64(file.hash_algorithm + ":" + file.hash + "\n", hash);
    }
    std::ostringstream out;
    out << prefix << '-' << safe_revision_component(revision) << '-'
        << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

std::uint64_t json_size(const json& entry) {
    for (const char* key : {"size", "Size", "FileSize", "file_size"}) {
        auto it = entry.find(key);
        if (it == entry.end()) continue;
        if (it->is_number_unsigned()) return it->get<std::uint64_t>();
        if (it->is_number_integer()) {
            const auto value = it->get<std::int64_t>();
            return value > 0 ? static_cast<std::uint64_t>(value) : 0;
        }
        if (it->is_string()) {
            try { return static_cast<std::uint64_t>(std::stoull(it->get<std::string>())); }
            catch (...) { return 0; }
        }
    }
    return 0;
}

std::string first_string(const json& entry,
                         std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        auto it = entry.find(key);
        if (it != entry.end() && it->is_string()) return it->get<std::string>();
    }
    return "";
}

std::uint64_t first_uint64(const json& entry,
                           std::initializer_list<const char*> keys,
                           std::uint64_t fallback = 0) {
    for (const char* key : keys) {
        auto it = entry.find(key);
        if (it == entry.end()) continue;
        if (it->is_number_unsigned()) return it->get<std::uint64_t>();
        if (it->is_number_integer()) {
            const auto value = it->get<std::int64_t>();
            return value > 0 ? static_cast<std::uint64_t>(value) : 0;
        }
        if (it->is_string()) {
            try { return static_cast<std::uint64_t>(std::stoull(it->get<std::string>())); }
            catch (...) { continue; }
        }
    }
    return fallback;
}

std::vector<std::string> first_string_array(
    const json& entry,
    std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        auto it = entry.find(key);
        if (it == entry.end()) continue;
        if (it->is_string()) return {it->get<std::string>()};
        if (!it->is_array()) continue;
        std::vector<std::string> values;
        for (const auto& value : *it) {
            if (value.is_string()) values.push_back(value.get<std::string>());
        }
        return values;
    }
    return {};
}

bool contains_gguf(const std::string& value) {
    return lower_copy(value).find("gguf") != std::string::npos;
}

std::string compact_search_text(const std::string& value) {
    std::string compact;
    compact.reserve(value.size());
    for (unsigned char c : value) {
        if (std::isalnum(c)) compact.push_back(static_cast<char>(std::tolower(c)));
    }
    return compact;
}

std::int64_t registry_search_rank(const std::string& query,
                                  const RegistrySearchResult& model) {
    const std::string query_lower = lower_copy(query);
    const std::string query_compact = compact_search_text(query);
    const std::string id_lower = lower_copy(model.repo_id);
    const auto slash = id_lower.find_last_of('/');
    const std::string name_lower = slash == std::string::npos
        ? id_lower : id_lower.substr(slash + 1);
    const std::string id_compact = compact_search_text(id_lower);
    const std::string name_compact = compact_search_text(name_lower);

    std::int64_t score = 0;
    if (name_lower == query_lower || name_compact == query_compact) {
        score += 100000;
    } else if (name_lower.rfind(query_lower, 0) == 0 ||
               name_compact.rfind(query_compact, 0) == 0) {
        score += 70000;
    } else if (name_lower.find(query_lower) != std::string::npos ||
               name_compact.find(query_compact) != std::string::npos) {
        score += 50000;
    } else if (id_lower.find(query_lower) != std::string::npos ||
               id_compact.find(query_compact) != std::string::npos) {
        score += 35000;
    }

    if (model.has_gguf) score += 12000;
    score += static_cast<std::int64_t>(std::min<std::uint64_t>(model.downloads, 1000));
    return score;
}

bool has_gguf_metadata(const std::string& repo_id,
                       const std::vector<std::string>& tags,
                       const json& metadata) {
    if (contains_gguf(repo_id)) return true;
    if (std::any_of(tags.begin(), tags.end(), contains_gguf)) return true;
    for (const char* key : {"model_type", "ModelType", "format", "Format"}) {
        const std::string value = first_string(metadata, {key});
        if (!value.empty() && contains_gguf(value)) return true;
    }
    return false;
}

bool excluded_generation_task(const std::string& task) {
    const std::string normalized = lower_copy(task);
    static const std::vector<std::string> excluded = {
        "automatic-speech-recognition", "text-to-speech", "audio-text-to-text",
        "text-to-audio", "audio-to-audio", "voice-activity-detection",
        "text-to-image", "image-to-image", "image-to-video", "image-to-3d",
        "image-text-to-image", "image-text-to-video", "unconditional-image-generation",
        "image-segmentation", "object-detection", "depth-estimation", "mask-generation",
        "zero-shot-object-detection", "text-to-video", "text-to-3d", "video-to-video",
    };
    return std::find(excluded.begin(), excluded.end(), normalized) != excluded.end();
}

const json* registry_search_items(const json& body) {
    const json* payload = &body;
    if (body.is_object()) {
        if (auto it = body.find("data"); it != body.end()) payload = &*it;
        else if (auto it = body.find("Data"); it != body.end()) payload = &*it;
    }
    if (payload->is_array()) return payload;
    if (!payload->is_object()) return nullptr;
    for (const char* key : {"models", "Models", "items", "Items", "results", "Results", "list", "List"}) {
        auto it = payload->find(key);
        if (it != payload->end() && it->is_array()) return &*it;
    }
    return nullptr;
}

std::uint64_t registry_search_total(const json& body, std::uint64_t fallback) {
    if (!body.is_object()) return fallback;
    std::uint64_t total = first_uint64(body, {"total_count", "TotalCount", "total", "Total"}, fallback);
    if (auto it = body.find("data"); it != body.end() && it->is_object()) {
        total = first_uint64(*it, {"total_count", "TotalCount", "total", "Total"}, total);
    } else if (auto it = body.find("Data"); it != body.end() && it->is_object()) {
        total = first_uint64(*it, {"total_count", "TotalCount", "total", "Total"}, total);
    }
    return total;
}

std::string registry_search_status_message(RemoteRegistrySource source,
                                           int status_code,
                                           const std::string& response_body) {
    std::string message;
    const json body = json::parse(response_body, nullptr, false);
    if (body.is_object()) {
        if (auto it = body.find("error"); it != body.end()) {
            if (it->is_string()) message = it->get<std::string>();
            else if (it->is_object()) {
                message = first_string(*it, {"message", "Message", "detail", "Detail"});
            }
        }
        if (message.empty()) {
            message = first_string(body, {"message", "Message", "detail", "Detail"});
        }
    }

    std::string result = remote_registry_display_name(source) +
        " search returned HTTP " + std::to_string(status_code);
    if (!message.empty()) result += ": " + message;
    return result;
}

bool is_modelscope_not_found_or_access_error(const std::string& code_text,
                                              const std::string& message) {
    const std::string code = lower_copy(code_text);
    if (code == "401" || code == "403" || code == "404" ||
        code == "e3001" || code == "e3002" || code == "e3020") {
        return true;
    }

    const std::string normalized_message = lower_copy(message);
    for (const char* marker : {
             "not found",
             "does not exist",
             "not exist",
             "permission denied",
             "access denied",
             "not accessible",
             "unauthorized",
             "authentication failed",
         }) {
        if (normalized_message.find(marker) != std::string::npos) {
            return true;
        }
    }

    return false;
}

void ensure_modelscope_success(const json& body, const std::string& repo_id) {
    if (!body.is_object()) return;

    bool failed = false;
    std::string code_text;
    const json* code_value = nullptr;
    if (auto it = body.find("Code"); it != body.end()) code_value = &*it;
    else if (auto it = body.find("code"); it != body.end()) code_value = &*it;
    if (code_value) {
        if (code_value->is_number_integer()) {
            const auto code = code_value->get<long long>();
            failed = code != 0 && code != 200;
            code_text = std::to_string(code);
        } else if (code_value->is_string()) {
            code_text = code_value->get<std::string>();
            const std::string normalized = lower_copy(code_text);
            failed = normalized != "0" && normalized != "200" &&
                     normalized != "ok" && normalized != "success";
        }
    }
    const json* success_value = nullptr;
    if (auto it = body.find("Success"); it != body.end()) success_value = &*it;
    else if (auto it = body.find("success"); it != body.end()) success_value = &*it;
    if (success_value && success_value->is_boolean()) {
        failed = failed || !success_value->get<bool>();
    }
    if (!failed) return;

    std::string message = first_string(body, {"Message", "message", "Msg", "msg"});
    if (message.empty()) {
        message = "ModelScope API rejected the request";
    }

    std::string detail = message;
    if (!code_text.empty()) {
        detail += " (code " + code_text + ")";
    }

    if (is_modelscope_not_found_or_access_error(code_text, message)) {
        throw RegistryNotFoundError(
            "ModelScope repository not found or not accessible: " +
            repo_id + " (" + detail + ")");
    }

    throw std::runtime_error(detail + " for " + repo_id);
}

class HuggingFaceRegistry final : public ModelRegistry {
public:
    RemoteRegistrySource source() const override {
        return RemoteRegistrySource::HuggingFace;
    }

    std::string default_revision() const override { return "main"; }

    std::map<std::string, std::string> auth_headers() const override {
        std::map<std::string, std::string> headers;
        const std::string token = env_string("HF_TOKEN");
        if (!token.empty()) headers["Authorization"] = "Bearer " + token;
        return headers;
    }

    RegistryRepository fetch_repository(const std::string& repo_id,
                                        const std::string& requested_revision) const override {
        const std::string revision = requested_revision.empty() ? default_revision() : requested_revision;
        std::string endpoint = trim_trailing_slash(env_string("HF_ENDPOINT"));
        if (endpoint.empty()) endpoint = "https://huggingface.co";

        std::string url = endpoint + "/api/models/" + percent_encode(repo_id, true);
        if (!requested_revision.empty() && revision != "main") {
            url += "/revision/" + percent_encode(revision, true);
        }
        url += "?blobs=true";

        const auto response = HttpClient::get(url, auth_headers());
        if (response.status_code == 404 || response.status_code == 401 || response.status_code == 403) {
            throw RegistryNotFoundError("Hugging Face repository not found or not accessible: " + repo_id);
        }
        if (response.status_code != 200) {
            throw std::runtime_error("Hugging Face API returned status " +
                                     std::to_string(response.status_code) +
                                     " for " + repo_id);
        }

        json metadata = JsonUtils::parse(response.body);
        if (!metadata.contains("siblings") || !metadata["siblings"].is_array()) {
            throw std::runtime_error("Hugging Face response for " + repo_id +
                                     " is missing the siblings array");
        }

        RegistryRepository result;
        result.repo_id = repo_id;
        result.revision = revision;
        result.raw_metadata = metadata;
        result.snapshot_id = metadata.value("sha", std::string());
        if (result.snapshot_id.empty()) result.snapshot_id = revision;
        // Resolve downloads against the immutable commit whenever HF provides it.
        result.revision = result.snapshot_id;

        for (const auto& sibling : metadata["siblings"]) {
            if (!sibling.is_object()) continue;
            RegistryFile file;
            file.path = first_string(sibling, {"rfilename", "path", "name"});
            if (file.path.empty()) continue;
            file.size = json_size(sibling);
            if (sibling.contains("lfs") && sibling["lfs"].is_object()) {
                file.hash = first_string(sibling["lfs"], {"oid", "sha256"});
                if (!file.hash.empty()) file.hash_algorithm = "sha256";
                if (file.size == 0) file.size = json_size(sibling["lfs"]);
            }
            if (file.hash.empty()) {
                file.hash = first_string(sibling, {"blobId", "oid"});
                if (!file.hash.empty()) file.hash_algorithm = "git-sha1";
            }
            result.files.push_back(std::move(file));
        }
        return result;
    }

    std::string resolve_file_url(const std::string& repo_id,
                                 const std::string& revision,
                                 const std::string& file_path) const override {
        std::string endpoint = trim_trailing_slash(env_string("HF_ENDPOINT"));
        if (endpoint.empty()) endpoint = "https://huggingface.co";
        return endpoint + "/" + percent_encode(repo_id, true) + "/resolve/" +
               percent_encode(revision.empty() ? default_revision() : revision, true) + "/" +
               percent_encode(file_path, true);
    }
};

class ModelScopeRegistry final : public ModelRegistry {
public:
    RemoteRegistrySource source() const override {
        return RemoteRegistrySource::ModelScope;
    }

    std::string default_revision() const override { return "master"; }

    std::map<std::string, std::string> auth_headers() const override {
        std::map<std::string, std::string> headers;
        std::string token = env_string("MODELSCOPE_API_TOKEN");
        if (token.empty()) token = env_string("MODELSCOPE_ACCESS_TOKEN");
        if (!token.empty()) {
            headers["Authorization"] = "Bearer " + token;
            // Legacy ModelScope endpoints historically authenticate private repos
            // with this session cookie. Sending both is accepted by current APIs.
            headers["Cookie"] = "m_session_id=" + token;
        }
        return headers;
    }

    RegistryRepository fetch_repository(const std::string& repo_id,
                                        const std::string& requested_revision) const override {
        const std::string revision = requested_revision.empty() ? default_revision() : requested_revision;
        const std::string endpoint = base_endpoint();
        const std::string url = endpoint + "/api/v1/models/" +
            percent_encode(repo_id, true) + "/repo/files?Revision=" +
            percent_encode(revision) + "&Recursive=True";

        const auto response = HttpClient::get(url, auth_headers());
        if (response.status_code == 404 || response.status_code == 401 || response.status_code == 403) {
            throw RegistryNotFoundError("ModelScope repository not found or not accessible: " + repo_id);
        }
        if (response.status_code != 200) {
            throw std::runtime_error("ModelScope API returned status " +
                                     std::to_string(response.status_code) +
                                     " for " + repo_id);
        }

        json body = JsonUtils::parse(response.body);
        ensure_modelscope_success(body, repo_id);
        json files_json;
        if (body.is_array()) {
            files_json = body;
        } else if (body.is_object()) {
            json data = body.contains("Data") ? body["Data"] : body;
            if (data.is_array()) {
                files_json = data;
            } else if (data.is_object()) {
                if (data.contains("Files")) files_json = data["Files"];
                else if (data.contains("files")) files_json = data["files"];
            }
        }
        if (!files_json.is_array()) {
            throw std::runtime_error("ModelScope response for " + repo_id +
                                     " does not contain a file list");
        }

        RegistryRepository result;
        result.repo_id = repo_id;
        result.revision = revision;
        result.raw_metadata = body;
        for (const auto& entry : files_json) {
            if (!entry.is_object()) continue;
            RegistryFile file;
            file.path = first_string(entry, {"Path", "path", "Name", "name"});
            if (file.path.empty()) continue;
            const std::string type = lower_copy(first_string(entry, {"Type", "type"}));
            file.directory = type == "tree" || type == "dir" || type == "directory";
            file.size = json_size(entry);
            file.hash = first_string(entry, {"Sha256", "sha256", "SHA256"});
            if (!file.hash.empty()) file.hash_algorithm = "sha256";
            result.files.push_back(std::move(file));
        }
        result.snapshot_id = registry_tree_snapshot_id(source(), revision, result.files);
        return result;
    }

    std::string resolve_file_url(const std::string& repo_id,
                                 const std::string& revision,
                                 const std::string& file_path) const override {
        return base_endpoint() + "/api/v1/models/" + percent_encode(repo_id, true) +
               "/repo?Revision=" + percent_encode(
                   revision.empty() ? default_revision() : revision) +
               "&FilePath=" + percent_encode(file_path);
    }

private:
    static std::string base_endpoint() {
        std::string endpoint = trim_trailing_slash(env_string("MODELSCOPE_ENDPOINT"));
        if (endpoint.empty()) endpoint = "https://modelscope.cn";
        return endpoint;
    }
};

}  // namespace

RegistrySearchResult normalize_registry_search_result(
    RemoteRegistrySource source,
    const nlohmann::json& metadata) {
    RegistrySearchResult result;
    result.source = source;
    if (!metadata.is_object()) return result;

    result.repo_id = first_string(metadata, {
        "id", "Id", "repo_id", "repoId", "model_id", "modelId"
    });
    if (result.repo_id.empty()) {
        const std::string path = first_string(metadata, {"Path", "path"});
        if (path.find('/') != std::string::npos) {
            result.repo_id = path;
        } else {
            const std::string owner = first_string(metadata, {
                "owner", "Owner", "namespace", "Namespace", "Path", "path"
            });
            const std::string name = first_string(metadata, {
                "name", "Name", "repo_name", "repoName", "model_name", "modelName"
            });
            if (!owner.empty() && !name.empty()) result.repo_id = owner + "/" + name;
        }
    }

    result.display_name = first_string(metadata, {
        "display_name", "displayName", "ChineseName", "name", "Name"
    });
    if (result.display_name.empty()) {
        const auto slash = result.repo_id.find('/');
        result.display_name = slash == std::string::npos
            ? result.repo_id : result.repo_id.substr(slash + 1);
    }
    result.description = first_string(metadata, {
        "description", "Description", "summary", "Summary"
    });
    result.task = first_string(metadata, {
        "pipeline_tag", "pipelineTag", "task", "Task"
    });
    if (result.task.empty()) {
        const auto tasks = first_string_array(metadata, {"tasks", "Tasks"});
        if (!tasks.empty()) result.task = tasks.front();
    }
    result.tags = first_string_array(metadata, {
        "tags", "Tags", "custom_tags", "customTags"
    });
    if (result.task.empty()) {
        for (const auto& tag : result.tags) {
            const std::string normalized_tag = lower_copy(tag);
            if (normalized_tag.rfind("task:", 0) == 0 && tag.size() > 5) {
                result.task = tag.substr(5);
                break;
            }
        }
    }
    result.downloads = first_uint64(metadata, {
        "downloads", "Downloads", "download_count", "downloadCount", "DownloadCount"
    });
    result.likes = first_uint64(metadata, {
        "likes", "Likes", "like_count", "likeCount", "LikeCount"
    });
    result.has_gguf = has_gguf_metadata(result.repo_id, result.tags, metadata);
    return result;
}

RegistrySearchResponse normalize_registry_search_response(
    RemoteRegistrySource source,
    const nlohmann::json& body,
    std::size_t limit) {
    limit = std::clamp<std::size_t>(limit, 1, 50);
    if (source == RemoteRegistrySource::ModelScope) {
        ensure_modelscope_success(body, "model search");
    }

    const json* items = registry_search_items(body);
    if (!items) {
        throw std::runtime_error(
            remote_registry_display_name(source) + " search response is missing a model list");
    }

    RegistrySearchResponse result;
    result.total = registry_search_total(body, items->size());
    result.results.reserve(std::min<std::size_t>(items->size(), limit));
    for (const auto& item : *items) {
        RegistrySearchResult normalized = normalize_registry_search_result(source, item);
        if (normalized.repo_id.empty() || excluded_generation_task(normalized.task)) continue;
        // Provider list metadata is only a hint here. The request-specific
        // format filter is applied by search_registry_models below, while
        // /pull/variants remains the authoritative file compatibility check.
        result.results.push_back(std::move(normalized));
        if (result.results.size() >= limit) break;
    }
    return result;
}

RegistrySearchResponse search_registry_models(RemoteRegistrySource source,
                                              const std::string& query,
                                              std::size_t limit,
                                              bool gguf_only) {
    if (query.empty()) throw std::invalid_argument("Search query must not be empty");
    limit = std::clamp<std::size_t>(limit, 1, 50);

    struct SearchRequest {
        std::string url;
        bool gguf_hint = false;
        bool optional = false;
    };

    std::string endpoint;
    std::vector<SearchRequest> requests;
    if (source == RemoteRegistrySource::HuggingFace) {
        endpoint = trim_trailing_slash(env_string("HF_ENDPOINT"));
        if (endpoint.empty()) endpoint = "https://huggingface.co";
        std::string url = endpoint + "/api/models?search=" + percent_encode(query) +
                          "&sort=downloads&direction=-1&limit=" + std::to_string(limit);
        if (gguf_only) url += "&filter=gguf";
        requests.push_back({std::move(url), gguf_only, false});
    } else {
        endpoint = trim_trailing_slash(env_string("MODELSCOPE_ENDPOINT"));
        if (endpoint.empty()) endpoint = "https://modelscope.cn";

        // ModelScope tokenizes version-like names broadly (Qwen3.6 may match
        // Qwen3-0.6B). Ask for a moderately larger candidate page and rank it
        // locally before the renderer performs the authoritative file-tree
        // check through /pull/variants.
        const std::size_t candidate_page_size = std::min<std::size_t>(
            50, std::max<std::size_t>(24, limit * 2));
        const std::string base = endpoint +
            "/openapi/v1/models?sort=downloads&page_number=1&page_size=" +
            std::to_string(candidate_page_size);

        if (gguf_only) {
            // Start with the explicit GGUF query. The library facet is only a
            // fallback when that first response does not provide enough
            // candidates; older code always made both calls and also issued a
            // third unfiltered request, adding avoidable latency.
            requests.push_back({
                base + "&search=" + percent_encode(query + " GGUF"),
                true,
                false,
            });
            requests.push_back({
                base + "&search=" + percent_encode(query) + "&filter.library=gguf",
                true,
                true,
            });
        } else {
            requests.push_back({base + "&search=" + percent_encode(query), false, false});
        }
    }

    RegistrySearchResponse merged;
    for (const auto& request : requests) {
        if (source == RemoteRegistrySource::ModelScope && gguf_only && request.optional) {
            const std::size_t confirmed = static_cast<std::size_t>(std::count_if(
                merged.results.begin(), merged.results.end(),
                [](const RegistrySearchResult& model) { return model.has_gguf; }));
            if (confirmed >= std::min<std::size_t>(limit, 8)) break;
        }

        const auto response = HttpClient::get(
            request.url, model_registry(source).auth_headers());
        if (response.status_code != 200) {
            if (request.optional && !merged.results.empty()) continue;
            throw RegistrySearchError(
                response.status_code,
                registry_search_status_message(source, response.status_code, response.body));
        }

        RegistrySearchResponse page;
        try {
            page = normalize_registry_search_response(
                source, JsonUtils::parse(response.body), 50);
        } catch (...) {
            if (request.optional && !merged.results.empty()) continue;
            throw;
        }
        merged.total = std::max(merged.total, page.total);
        for (auto& model : page.results) {
            // An explicit GGUF query or provider library facet is a useful
            // ranking hint but not proof; /pull/variants remains the only
            // compatibility authority.
            if (request.gguf_hint) model.has_gguf = true;

            const bool duplicate = std::any_of(
                merged.results.begin(), merged.results.end(),
                [&model](const RegistrySearchResult& existing) {
                    return existing.source == model.source &&
                           existing.repo_id == model.repo_id;
                });
            if (!duplicate) merged.results.push_back(std::move(model));
        }
    }

    std::stable_sort(
        merged.results.begin(), merged.results.end(),
        [&query](const RegistrySearchResult& lhs,
                 const RegistrySearchResult& rhs) {
            const auto lhs_rank = registry_search_rank(query, lhs);
            const auto rhs_rank = registry_search_rank(query, rhs);
            if (lhs_rank != rhs_rank) return lhs_rank > rhs_rank;
            if (lhs.downloads != rhs.downloads) return lhs.downloads > rhs.downloads;
            return lhs.repo_id < rhs.repo_id;
        });
    if (merged.results.size() > limit) merged.results.resize(limit);
    return merged;
}

RemoteRegistrySource parse_remote_registry_source(const std::string& source) {
    const std::string normalized = lower_copy(source);
    if (normalized.empty() || normalized == "huggingface" || normalized == "hf") {
        return RemoteRegistrySource::HuggingFace;
    }
    if (normalized == "modelscope" || normalized == "ms") {
        return RemoteRegistrySource::ModelScope;
    }
    throw std::invalid_argument("Unsupported model source '" + source +
                                "' (expected 'huggingface' or 'modelscope')");
}

std::string remote_registry_source_name(RemoteRegistrySource source) {
    return source == RemoteRegistrySource::ModelScope ? "modelscope" : "huggingface";
}

std::string remote_registry_display_name(RemoteRegistrySource source) {
    return source == RemoteRegistrySource::ModelScope ? "ModelScope" : "Hugging Face";
}

bool is_remote_registry_source(const std::string& source) {
    const std::string normalized = lower_copy(source);
    return normalized == "huggingface" || normalized == "hf" ||
           normalized == "modelscope" || normalized == "ms";
}

std::string registry_tree_snapshot_id(RemoteRegistrySource source,
                                      const std::string& revision,
                                      const std::vector<RegistryFile>& files) {
    return tree_snapshot_id(remote_registry_source_name(source), revision, files);
}

std::string registry_repo_cache_dir_name(const std::string& repo_id,
                                         RemoteRegistrySource source) {
    std::string cache_dir_name = source == RemoteRegistrySource::ModelScope
        ? "modelscope--models--"
        : "models--";
    for (char c : repo_id) {
        cache_dir_name += (c == '/') ? "--" : std::string(1, c);
    }
    return cache_dir_name;
}

const ModelRegistry& model_registry(RemoteRegistrySource source) {
    static const HuggingFaceRegistry huggingface;
    static const ModelScopeRegistry modelscope;
    return source == RemoteRegistrySource::ModelScope
        ? static_cast<const ModelRegistry&>(modelscope)
        : static_cast<const ModelRegistry&>(huggingface);
}

const ModelRegistry& model_registry(const std::string& source) {
    return model_registry(parse_remote_registry_source(source));
}

}  // namespace lemon
