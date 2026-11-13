#include "lemon/backends/loom/loom_bundle.h"

#include "lemon/backends/loom/loom.h"
#include "lemon/utils/custom_args.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <stdexcept>

namespace lemon {
namespace backends {
namespace loom {

namespace {

struct Origin {
    std::string repository;
    std::string revision;
};

std::string string_field(const json& object, const char* key) {
    if (!object.is_object()) return "";
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : "";
}

std::map<std::string, Origin> read_origins(const json& card, const json& lock) {
    std::map<std::string, Origin> origins;
    const auto declared = card.find("origins");
    if (declared == card.end() || !declared->is_object()) return origins;
    const json locked = lock.is_object() ? lock.value("origins", json::object()) : json::object();
    for (const auto& [name, origin] : declared->items()) {
        const std::string kind = string_field(origin, "kind");
        if (kind != "hf") {
            throw std::runtime_error("Loom origin '" + name + "' has kind '" + kind +
                                     "'; only Hugging Face origins are supported");
        }
        Origin resolved{string_field(origin, "repository"), string_field(origin, "revision")};
        if (locked.contains(name)) {
            const std::string pinned = string_field(locked[name].value("resolved", json::object()),
                                                    "revision");
            if (!pinned.empty()) resolved.revision = pinned;
        }
        if (resolved.repository.empty() || resolved.revision.empty()) {
            throw std::runtime_error("Loom origin '" + name + "' does not pin a repository revision");
        }
        origins[name] = resolved;
    }
    return origins;
}

std::map<std::string, LockedFile> read_lock_files(const json& lock) {
    std::map<std::string, LockedFile> files;
    const auto entries = lock.is_object() ? lock.find("files") : lock.end();
    if (!lock.is_object() || entries == lock.end() || !entries->is_array()) {
        throw std::runtime_error("Loom lock has no files list");
    }
    for (const auto& entry : *entries) {
        const std::string origin = string_field(entry, "origin");
        const std::string sha256 = string_field(entry, "sha256");
        if (origin.empty() || sha256.empty() || !entry.contains("size") ||
            !entry["size"].is_number_unsigned()) {
            throw std::runtime_error("Loom lock has a malformed file entry");
        }
        files[origin] = LockedFile{origin, entry["size"].get<std::uint64_t>(), sha256};
    }
    return files;
}

void visit_file_references(const json& node,
                           const std::function<void(const std::string&, const std::string&)>& visit) {
    if (node.is_object()) {
        const std::string path = string_field(node, "path");
        const std::string origin = string_field(node, "origin");
        if (!path.empty() && !origin.empty()) visit(path, origin);
        for (const auto& [key, value] : node.items()) {
            if (key != "origins") visit_file_references(value, visit);
        }
    } else if (node.is_array()) {
        for (const auto& value : node) visit_file_references(value, visit);
    }
}

bool is_repo_id_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.';
}

}  // namespace

bool is_contained_relative_path(const std::string& path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos ||
        path.find(':') != std::string::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = std::min(path.find('/', start), path.size());
        const std::string segment = path.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == "..") return false;
        start = end + 1;
    }
    return true;
}

BundlePlan plan_bundle(const json& card, const json& lock) {
    if (!card.is_object()) throw std::runtime_error("Loom card is not a JSON object");
    BundlePlan plan;
    plan.model_id = string_field(card.value("model", json::object()), "id");
    if (plan.model_id.empty()) throw std::runtime_error("Loom card has no model.id");
    const auto presets = card.find("presets");
    if (presets != card.end() && presets->is_object() && !presets->contains("chat") &&
        presets->size() == 1) {
        plan.preset = presets->begin().key();
    }

    const auto origins = read_origins(card, lock);
    const auto locked = read_lock_files(lock);

    std::map<std::string, std::string> origin_by_path;
    visit_file_references(card, [&](const std::string& path, const std::string& origin) {
        if (origin.empty() || origin.front() != '@') return;
        if (!is_contained_relative_path(path)) {
            throw std::runtime_error("Loom card places a file outside its folder: " + path);
        }
        const auto [seen, inserted] = origin_by_path.emplace(path, origin);
        if (!inserted) {
            if (seen->second != origin) {
                throw std::runtime_error("Loom card maps " + path + " to two origins");
            }
            return;
        }
        const std::size_t slash = origin.find('/');
        const std::string name = origin.substr(1, slash == std::string::npos ? 0 : slash - 1);
        const std::string file = slash == std::string::npos ? "" : origin.substr(slash + 1);
        const auto source = origins.find(name);
        if (source == origins.end() || !is_contained_relative_path(file)) {
            throw std::runtime_error("Loom card names an undeclared origin: " + origin);
        }
        const auto digest = locked.find(origin);
        if (digest == locked.end()) {
            throw std::runtime_error("Loom lock has no digest for " + origin);
        }
        plan.origin_files.push_back(OriginFile{path, source->second.repository,
                                               source->second.revision, file,
                                               digest->second.size, digest->second.sha256});
    });

    for (const auto& [origin, file] : locked) {
        if (origin.front() == '@') continue;
        if (!is_contained_relative_path(origin)) {
            throw std::runtime_error("Loom lock names a file outside the bundle: " + origin);
        }
        plan.locked_files.push_back(file);
    }
    return plan;
}

std::string store_dir_name(const std::string& repo_id) {
    const std::size_t slash = repo_id.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 == repo_id.size() ||
        repo_id.find('/', slash + 1) != std::string::npos ||
        !std::all_of(repo_id.begin(), repo_id.end(),
                     [](char c) { return c == '/' || is_repo_id_char(c); }) ||
        repo_id.find("..") != std::string::npos) {
        throw std::invalid_argument("Not an org/name repository id: " + repo_id);
    }
    return repo_id.substr(0, slash) + "--" + repo_id.substr(slash + 1);
}

std::vector<std::string> build_server_argv(const std::string& model_dir, int port,
                                           const std::string& preset,
                                           const std::string& device,
                                           const std::string& memory_bytes,
                                           const std::string& custom_args) {
    if (device.empty() ||
        std::any_of(device.begin(), device.end(),
                    [](char c) { return std::isspace(static_cast<unsigned char>(c)); })) {
        throw std::invalid_argument("loom_device must be a single HAL device URI");
    }
    std::vector<std::string> argv = {
        "--model=" + model_dir + "/" + kCardFile,
        "--listen=127.0.0.1:" + std::to_string(port),
        "--device=" + device,
    };
    if (!preset.empty()) argv.push_back("--preset=" + preset);
    if (!memory_bytes.empty()) argv.push_back("--memory_bytes=" + memory_bytes);

    if (!custom_args.empty()) {
        const std::string error =
            utils::validate_custom_args(custom_args, reserved_custom_arg_flags());
        if (!error.empty()) {
            throw std::invalid_argument("Invalid custom loom-serve arguments:\n" + error);
        }
        const auto parsed = utils::parse_custom_args(custom_args);
        argv.insert(argv.end(), parsed.begin(), parsed.end());
    }
    return argv;
}

json prepare_chat_request(json request, const std::string& model_id) {
    request["model"] = model_id;
    const auto kwargs = request.find("chat_template_kwargs");
    const bool thinking_off = kwargs != request.end() && kwargs->is_object() &&
                              kwargs->contains("enable_thinking") &&
                              (*kwargs)["enable_thinking"].is_boolean() &&
                              !(*kwargs)["enable_thinking"].get<bool>();
    if (!thinking_off) return request;

    // Lemonade sends reasoning_effort=none beside the template switch, and no
    // Loom card accepts a reasoning effort.
    if (request.value("reasoning_effort", json()) == "none") request.erase("reasoning_effort");

    static const std::string kPrefix = "/no_think\n";
    if (request.contains("messages") && request["messages"].is_array()) {
        for (auto& message : request["messages"]) {
            if (message.is_object() && message.value("role", "") == "user" &&
                message.contains("content") && message["content"].is_string()) {
                const std::string content = message["content"].get<std::string>();
                if (content.rfind(kPrefix, 0) == 0) message["content"] = content.substr(kPrefix.size());
            }
        }
    }
    return request;
}

ReadinessState parse_readiness(const json& models, const json& status, const std::string& model_id) {
    if (models.is_object() && models.contains("data") && models["data"].is_array()) {
        for (const auto& entry : models["data"]) {
            if (string_field(entry, "id") == model_id) return {Readiness::Ready, ""};
        }
    }
    if (status.is_object() && status.contains("data") && status["data"].is_array()) {
        for (const auto& entry : status["data"]) {
            if (string_field(entry, "id") != model_id) continue;
            const json loom = entry.value("loom", json::object());
            if (string_field(loom, "registration") != "failed") break;
            std::string message = string_field(loom.value("error", json::object()), "message");
            if (message.empty()) message = "loom-serve could not prepare " + model_id;
            return {Readiness::Failed, message};
        }
    }
    return {Readiness::Pending, ""};
}

std::string card_memory_bytes(const json& info) {
    if (!info.is_object()) return "";
    const json card = info.value("card", json::object());
    const auto it = card.is_object() ? card.find("memory_bytes") : card.end();
    if (!card.is_object() || it == card.end() || !it->is_number_unsigned()) return "";
    const auto bytes = it->get<std::uint64_t>();
    return bytes == 0 ? "" : std::to_string(bytes);
}

std::vector<std::string> suggested_labels(const json& info, const std::string& name) {
    const json card = info.is_object() ? info.value("card", json::object()) : json::object();
    const json features = card.is_object() ? card.value("features", json::object()) : json::object();
    const auto on = [&](const char* feature) {
        return features.is_object() && features.contains(feature) &&
               string_field(features[feature], "status") == "yes";
    };
    std::vector<std::string> labels = {"chat"};
    if (on("image_input")) labels.push_back("vision");
    if (on("tool_calling")) labels.push_back("tool-calling");
    if (on("thinking")) labels.push_back("reasoning");
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.find("coder") != std::string::npos) labels.push_back("coding");
    if (on("speculative_decoding")) {
        const std::string method =
            string_field(features["speculative_decoding"].value("value", json::object()), "method");
        if (method == "mtp" || method == "dflash") labels.push_back(method);
    }
    return labels;
}

std::string suggested_name(const std::string& repo_id, const json& card) {
    std::string name = repo_id.substr(repo_id.find('/') + 1);
    static const std::string kSuffix = "-loom";
    if (name.size() > kSuffix.size() &&
        name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
        name.resize(name.size() - kSuffix.size());
    }
    const json origins = card.is_object() ? card.value("origins", json::object()) : json::object();
    if (origins.is_object()) {
        for (const auto& [key, origin] : origins.items()) {
            (void)key;
            const std::string repository = string_field(origin, "repository");
            const std::string owner = repository.substr(0, repository.find('/'));
            if (!owner.empty() && name.size() > owner.size() + 1 &&
                name.compare(0, owner.size() + 1, owner + "-") == 0) {
                name = name.substr(owner.size() + 1);
                break;
            }
        }
    }
    return name + "-Loom";
}

std::uint64_t locked_bytes(const json& lock) {
    std::uint64_t total = 0;
    if (!lock.is_object() || !lock.contains("files") || !lock["files"].is_array()) return total;
    for (const auto& entry : lock["files"]) {
        if (entry.is_object() && entry.contains("size") && entry["size"].is_number_unsigned()) {
            total += entry["size"].get<std::uint64_t>();
        }
    }
    return total;
}

}  // namespace loom
}  // namespace backends
}  // namespace lemon
