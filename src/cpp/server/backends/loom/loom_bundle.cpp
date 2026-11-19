#include "lemon/backends/loom/loom_bundle.h"

#include "lemon/backends/loom/loom.h"
#include "lemon/utils/custom_args.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <map>
#include <set>
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

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0;
}

// info.json is bundle data, so it may only size and back the pool. A listener,
// model, device or flagfile flag from it would escape Lemonade's auth and routing.
bool is_recordable_serve_arg(const std::string& arg) {
    static const char* const kPrefixes[] = {
        "--memory_bytes=", "--parameter_mode=", "--pool_backing=",
        "--state_slab_bytes=", "--weight_slab_bytes=",
    };
    for (const char* prefix : kPrefixes) {
        const std::string value = arg.substr(std::min(arg.size(), std::strlen(prefix)));
        if (starts_with(arg, prefix) && !value.empty() &&
            std::all_of(value.begin(), value.end(),
                        [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; })) {
            return true;
        }
    }
    return false;
}

bool is_repo_id_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.';
}

PresetLimits execution_limits(const json& preset) {
    PresetLimits limits;
    if (!preset.is_object()) return limits;
    const auto execution = preset.find("execution");
    if (execution == preset.end() || !execution->is_object()) return limits;
    if (execution->contains("context_capacity") && (*execution)["context_capacity"].is_number_integer()) {
        const auto value = (*execution)["context_capacity"].get<std::int64_t>();
        if (value > 0) limits.context_capacity = value;
    }
    if (execution->contains("max_tokens") && (*execution)["max_tokens"].is_number_integer()) {
        const auto value = (*execution)["max_tokens"].get<std::int64_t>();
        if (value > 0) limits.max_tokens = value;
    }
    return limits;
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

std::vector<std::string> program_files(const json& card) {
    std::vector<std::string> files;
    const auto add = [&files](const json& entry) {
        const std::string path = entry.is_string() ? entry.get<std::string>() : string_field(entry, "path");
        if (path.empty()) return;
        if (!is_contained_relative_path(path)) {
            throw std::runtime_error("Loom card names a program file outside its folder: " + path);
        }
        files.push_back(path);
    };
    const auto programs = card.is_object() ? card.find("programs") : card.end();
    if (!card.is_object() || programs == card.end() || !programs->is_object()) return files;
    for (const auto& [name, program] : programs->items()) {
        (void)name;
        if (!program.is_object()) continue;
        for (const char* role : {"inputs", "libraries"}) {
            const auto entries = program.find(role);
            if (entries == program.end() || !entries->is_array()) continue;
            for (const auto& entry : *entries) {
                add(entry);
                if (!entry.is_object()) continue;
                const auto data = entry.find("data");
                if (data == entry.end() || !data->is_array()) continue;
                for (const auto& auxiliary : *data) add(auxiliary);
            }
        }
    }
    return files;
}

void validate_repo_id(const std::string& repo_id) {
    const std::size_t slash = repo_id.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 == repo_id.size() ||
        repo_id.find('/', slash + 1) != std::string::npos ||
        !std::all_of(repo_id.begin(), repo_id.end(),
                     [](char c) { return c == '/' || is_repo_id_char(c); }) ||
        repo_id.find("..") != std::string::npos) {
        throw std::invalid_argument("Not an org/name repository id: " + repo_id);
    }
}

std::string device_target(const std::string& arch, const std::string& gpu_name, double vram_gb) {
    if (arch == "gfx1151") return kStrixHaloTarget;
    std::string lower = gpu_name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // Windows reports only the gfx120X family; the RX 9060 (gfx1200) is not Navi 48.
    const bool navi48 = arch == "gfx1201" ||
                        (arch == "gfx120X" && lower.find("9060") == std::string::npos);
    if (!navi48) return "";
    if (vram_gb <= 0.0 || vram_gb >= 30.0) return kRadeonR9700Target;
    return vram_gb >= 14.0 ? kGfx120116gbTarget : "";
}

bool bundle_supports_target(const json& info, const std::string& target) {
    const auto devices = info.is_object() ? info.find("devices") : info.end();
    if (!info.is_object() || devices == info.end() || !devices->is_object()) return true;
    const auto entry = devices->find(target);
    if (entry == devices->end() || !entry->is_object()) return false;
    const std::string state = string_field(*entry, "state");
    return state != "unsupported" && state != "not_a_target";
}

PresetLimits preset_limits(const json& card, const std::string& preset) {
    const auto presets = card.is_object() ? card.find("presets") : card.end();
    if (!card.is_object() || presets == card.end() || !presets->is_object()) return {};
    if (!preset.empty()) {
        return presets->contains(preset) ? execution_limits((*presets)[preset]) : PresetLimits{};
    }
    if (presets->size() == 1) return execution_limits(presets->begin().value());
    return {};
}

QuickStart quick_start(const json& info, const std::string& target) {
    const auto devices = info.is_object() ? info.find("devices") : info.end();
    const json entry = devices != info.end() && devices->is_object()
                           ? devices->value(target, json::object())
                           : json::object();
    const auto recorded = entry.is_object() ? entry.find("quick_start") : entry.end();
    if (!entry.is_object() || recorded == entry.end() || !recorded->is_object()) {
        throw std::runtime_error("Loom bundle records no Quick start for " + target);
    }
    QuickStart qs;
    const json preset = recorded->value("preset", json());
    if (preset.is_string()) qs.preset = preset.get<std::string>();
    if ((!preset.is_null() && !preset.is_string()) ||
        !std::all_of(qs.preset.begin(), qs.preset.end(), is_repo_id_char)) {
        throw std::runtime_error("Loom bundle records a preset Lemonade does not allow for " +
                                 target + ": " + preset.dump());
    }
    const std::string device = string_field(*recorded, "device");
    const std::size_t query = device.find('?');
    qs.device_query = query == std::string::npos ? "" : device.substr(query + 1);
    if (!starts_with(device, "amdgpu://") ||
        !std::all_of(qs.device_query.begin(), qs.device_query.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '=' || c == '&';
        })) {
        throw std::runtime_error("Loom bundle records a device Lemonade does not allow for " +
                                 target + ": " + device);
    }
    const auto args = recorded->find("args");
    if (args == recorded->end() || !args->is_array()) {
        throw std::runtime_error("Loom bundle records no Quick start flags for " + target);
    }
    for (const auto& arg : *args) {
        const std::string text = arg.is_string() ? arg.get<std::string>() : "";
        if (!is_recordable_serve_arg(text)) {
            throw std::runtime_error("Loom bundle records a serve argument Lemonade does not "
                                     "allow for " + target + ": " + (arg.is_string() ? text : arg.dump()));
        }
        qs.args.push_back(text);
    }
    return qs;
}

std::vector<std::string> serve_args(const QuickStart& quick_start,
                                    bool preload, const std::string& custom_args) {
    std::vector<std::string> args = quick_start.args;
    if (!preload) args.erase(std::remove(args.begin(), args.end(), kPreloadFlag), args.end());
    if (utils::custom_args_has_flag(utils::parse_custom_args(custom_args), "--memory_bytes")) {
        args.erase(std::remove_if(args.begin(), args.end(),
                                  [](const std::string& arg) {
                                      return starts_with(arg, "--pool_backing=") ||
                                             starts_with(arg, "--memory_bytes=");
                                  }),
                   args.end());
    }
    return args;
}

std::vector<std::string> build_server_argv(const std::string& model_dir, int port,
                                           const std::string& preset,
                                           const std::vector<std::string>& serve_args,
                                           const std::string& custom_args,
                                           const std::string& device_query) {
    std::vector<std::string> argv = {
        "--model=" + model_dir + "/" + kCardFile,
        "--listen=127.0.0.1:" + std::to_string(port),
        "--device=amdgpu://" + (device_query.empty() ? "" : "?" + device_query),
    };
    if (!preset.empty()) argv.push_back("--preset=" + preset);
    argv.insert(argv.end(), serve_args.begin(), serve_args.end());

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

json normalize_chat_response(json response, const json& request, const std::string& fallback_model) {
    if (response.is_object() && response.contains("model")) {
        const std::string requested = string_field(request, "model");
        const std::string model = requested.empty() ? fallback_model : requested;
        if (!model.empty()) response["model"] = model;
    }
    return response;
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

std::string startup_failure_detail(const std::vector<std::string>& output_lines) {
    constexpr std::size_t kMaxLines = 4;
    constexpr std::size_t kMaxChars = 1000;
    std::vector<std::string> kept;
    for (const auto& raw : output_lines) {
        const auto first = raw.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        const auto last = raw.find_last_not_of(" \t\r\n");
        kept.push_back(raw.substr(first, last - first + 1));
        if (kept.size() > kMaxLines) kept.erase(kept.begin());
    }
    std::string detail;
    for (const auto& line : kept) {
        if (!detail.empty()) detail += "; ";
        detail += line;
    }
    if (detail.size() > kMaxChars) detail = "..." + detail.substr(detail.size() - kMaxChars);
    return detail;
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
