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
    if (auto it = body.find("Code"); it != body.end()) {
        if (it->is_number_integer()) {
            const auto code = it->get<long long>();
            failed = code != 0 && code != 200;
            code_text = std::to_string(code);
        } else if (it->is_string()) {
            code_text = it->get<std::string>();
            const std::string normalized = lower_copy(code_text);
            failed = normalized != "0" && normalized != "200" &&
                     normalized != "ok" && normalized != "success";
        }
    }
    if (auto it = body.find("Success"); it != body.end() && it->is_boolean()) {
        failed = failed || !it->get<bool>();
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
