#include "lemon/backends/adapter/adapter_server.h"
#include "adapter_spec.h"

#include "lemon/backends/adapter/adapter.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/server_process.h"
#include "lemon/system_info.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"

#include <cctype>
#include <csignal>
#include <cstdio>
#include <chrono>
#include <map>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef __linux__
#include <sys/types.h>
#include <signal.h>
#endif

namespace fs = std::filesystem;

namespace lemon {
namespace backends {

namespace adapter {

std::pair<std::string, std::string> split_checkpoint(const std::string& checkpoint) {
    const auto at = checkpoint.find("::");
    if (at == std::string::npos) {
        return {"", checkpoint};
    }
    return {checkpoint.substr(0, at), checkpoint.substr(at + 2)};
}

std::string adapters_dir() {
    if (const char* dir = std::getenv("LEMONADE_ADAPTERS_DIR"); dir != nullptr && *dir != '\0') {
        return dir;
    }
    return (fs::path(utils::get_config_dir()) / "adapters").string();
}

}  // namespace adapter

namespace {

// The pure spec helpers live in adapter_spec.{h,cpp}, tested without lemond (test/cpp/adapter_spec/).
using adapter::specs::contains;
using adapter::specs::fill;
using adapter::specs::split_args;
using adapter::specs::valid_spec_name;

json read_spec(const std::string& name, fs::path& spec_dir) {
    if (!valid_spec_name(name)) {
        throw std::runtime_error("adapter: '" + name + "' is not a valid spec name (letters, digits, - _ .)");
    }
    const fs::path path = fs::path(adapter::adapters_dir()) / (name + ".json");
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("adapter: no spec " + path.string() +
                                 " (install the engine's adapter, or fall back to running the engine without control)");
    }
    json spec;
    try {
        in >> spec;
    } catch (const std::exception& e) {
        throw std::runtime_error("adapter: " + path.string() + " is not valid JSON: " + e.what());
    }
    spec_dir = path.parent_path();
    return spec;
}

// The engine program: an absolute path, a path relative to the spec's folder, or a name on PATH.
std::string resolve_binary(const std::string& binary, const fs::path& spec_dir) {
    if (binary.empty()) {
        throw std::runtime_error("adapter: the spec names no binary");
    }
    const fs::path p(binary);
    std::error_code ec;
    if (p.is_absolute()) {
        if (fs::exists(p, ec)) return p.string();
        throw std::runtime_error("adapter: engine binary not found: " + binary);
    }
    if (binary.find('/') != std::string::npos || binary.find('\\') != std::string::npos) {
        const fs::path rel = spec_dir / p;
        if (fs::exists(rel, ec)) return rel.string();
        throw std::runtime_error("adapter: engine binary not found: " + rel.string());
    }
    if (!utils::find_executable_in_path(binary).empty()) return binary;
    throw std::runtime_error("adapter: engine binary '" + binary + "' is not on PATH");
}

// A recipe option as text: "" when unset (an unset option is JSON null).
std::string option_text(const RecipeOptions& options, const std::string& key) {
    const json v = options.get_option(key);
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number()) return v.dump();
    return "";
}

}  // namespace

InstallParams AdapterServer::get_install_params(const std::string& /*backend*/, const std::string& /*version*/) {
    return {};  // engines are installed by their adapter (e.g. the Lemonade Stand), never by lemond
}

AdapterServer::AdapterServer(const std::string& log_level, ModelManager* model_manager,
                             BackendManager* backend_manager)
    : WrappedServer("adapter", log_level, model_manager, backend_manager) {}

void AdapterServer::load(const std::string& model_name, const ModelInfo& model_info,
                         const RecipeOptions& options, bool /*do_not_upgrade*/) {
  try {
    const auto [name, model_path] = adapter::split_checkpoint(model_info.checkpoint());
    if (name.empty()) {
        throw std::runtime_error("adapter model '" + model_name + "' has no spec: write its checkpoint as "
                                 "\"<spec>::<model path>\"");
    }
    fs::path spec_dir;
    const json spec = read_spec(name, spec_dir);
    spec_name_ = name;
    const auto checked = adapter::specs::check_spec(spec);
    if (checked.level_cap == 0) {
        std::string problems;
        for (size_t i = 0; i < checked.problems.size(); ++i) {
            if (i > 0) problems += "; ";
            problems += checked.problems[i];
        }
        throw std::runtime_error("adapter " + name + ": the spec cannot be used (" + problems +
                                 "); run the engine without control instead");
    }

    const json ready = spec.value("ready", json::object());
    const std::string ready_path = ready.value("path", std::string("/v1/models"));
    const long ready_timeout = ready.value("timeout_s", 600L);
    const std::string ready_field = ready.value("json_field", std::string{});
    const json ready_values = ready.value("ready_values", json::array());
    const json error_values = ready.value("error_values", json::array());
    const std::string owned_by = spec.value("identity", json::object()).value("owned_by", std::string{});
    completion_ = spec.value("completion", true);
    telemetry_path_ = spec.value("telemetry_path", std::string{});
    memory_path_ = spec.value("memory_path", std::string{});

    std::string device;
    const std::string requested = option_text(options, "adapter_device");
    const std::string spec_default = spec.value("default_device_index", std::string{});
    std::vector<adapter::specs::Gpu> inventory;
    try {
        auto sys = create_system_info();
        for (auto& g : sys->get_nvidia_gpu_devices())
            if (g.index >= 0) inventory.push_back({g.index, g.uuid});
        for (auto& g : sys->get_amd_dgpu_devices())
            if (g.index >= 0) inventory.push_back({g.index, g.uuid});
    } catch (...) { }
    std::string gpu_uuids;
    try {
        const auto values = adapter::specs::device_values(requested, spec_default, inventory);
        device = values.device;
        gpu_uuids = values.gpu_uuids;
    } catch (const adapter::specs::SpecError& e) {
        throw std::runtime_error("adapter " + name + ": " + std::string(e.what()));
    }

    port_ = choose_port();
    if (port_ == 0) {
        throw std::runtime_error("adapter: failed to allocate a port for " + name);
    }
    std::map<std::string, std::string> subs = {
        {"{model}", model_path}, {"{model_path}", model_path}, {"{port}", std::to_string(port_)}, {"{device}", device},
        {"{spec_dir}", spec_dir.string()}};
    subs["{gpu_uuids}"] = gpu_uuids;

    std::vector<std::string> args;
    const std::string override_args = option_text(options, "adapter_args");
    if (!override_args.empty()) {
        args = split_args(fill(override_args, subs));
    } else {
        try {
            args = adapter::specs::build_argv(spec, subs);
        } catch (const adapter::specs::SpecError& e) {
            throw std::runtime_error("adapter " + name + ": " + std::string(e.what()));
        }
    }

    ServerCommand command;
    command.program = resolve_binary(fill(spec.value("binary", std::string{}), subs), spec_dir);
    command.args = std::move(args);
    const json env = spec.value("env", json::object());
    std::vector<std::string> allowed_dirs = {spec_dir.string()};
    if (fs::path(command.program).is_absolute()) {
        allowed_dirs.push_back(fs::path(command.program).parent_path().string());
        // When the binary's directory is named "bin", also push the engine bundle's root (parent of <binary>/bin) to allow LD_LIBRARY_PATH to point to sibling lib directories.
        if (fs::path(command.program).parent_path().filename() == "bin") {
            allowed_dirs.push_back(fs::path(command.program).parent_path().parent_path().string());
        }
    }
    try {
        for (auto& [k, v] : adapter::specs::build_env(env, subs, allowed_dirs)) {
            command.env.emplace_back(k, v);
        }
    } catch (const adapter::specs::SpecError& e) {
        throw std::runtime_error("adapter " + name + ": " + std::string(e.what()));
    }
    // Opt-in telemetry is declared in the spec, never hidden in its arguments: its env applies only when the
    // spec enables it or the user sets adapter_telemetry=on for this model.
    const json telemetry = spec.value("telemetry", json::object());
    if (telemetry.value("enabled", false) || option_text(options, "adapter_telemetry") == "on") {
        const json tenv = telemetry.value("env", json::object());
        try {
            for (auto& [k, v] : adapter::specs::build_env(tenv, subs, allowed_dirs)) {
                command.env.emplace_back(k, v);
            }
        } catch (const adapter::specs::SpecError& e) {
            throw std::runtime_error("adapter " + name + ": " + std::string(e.what()));
        }
        LOG(INFO, "Adapter") << name << ": telemetry on (" << telemetry.value("what", std::string("declared in the spec")) << ")" << std::endl;
    }
    // A catalogue can pin exactly which program runs.
    const std::string want_sha = spec.value("binary_sha256", std::string{});
    if (!want_sha.empty()) {
        std::string full = command.program;
        if (!fs::path(full).is_absolute()) full = utils::find_executable_in_path(full);
        std::string digest_error;
        if (full.empty() || !utils::HttpClient::verify_file_digest(full, "sha256", want_sha, digest_error)) {
            throw std::runtime_error("adapter " + name + ": " + command.program +
                                     " does not match the spec's binary_sha256 (" + digest_error + ")");
        }
    }
    command.port = port_;
    command.ready_endpoint = ready_path;

    LOG(INFO, "Adapter") << "Starting " << name << " (" << command.program << ") for " << model_path
                         << " on port " << port_ << " (device " << device << ")" << std::endl;
    const bool inherit_output = (log_level_ == "info") || (log_level_ == "debug");
    start_server(std::make_unique<NativeProcess>(ProcessOutput{inherit_output, true}), command, ready_timeout);

    // A health route can answer before the engine is ready (e.g. while weights load).
    if (!ready_field.empty()) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(ready_timeout);
        std::string last;
        while (true) {
            try {
                last = forward_get_request(ready_path, 5).value(ready_field, std::string{});
            } catch (...) {
                last = "";
            }
            if (contains(ready_values, last)) break;
            if (contains(error_values, last) || std::chrono::steady_clock::now() > deadline) {
                stop_server();
                throw std::runtime_error("adapter " + name + ": engine did not become ready (" + ready_field +
                                         "=\"" + last + "\")");
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    // Refuse a mislabelled engine when the spec names its owner.
    if (!owned_by.empty()) {
        bool identified = false;
        try {
            for (const auto& m : forward_get_request("/v1/models", 10).value("data", json::array())) {
                if (m.value("owned_by", std::string{}) == owned_by) identified = true;
            }
        } catch (...) {
        }
        if (!identified) {
            stop_server();
            throw std::runtime_error("adapter " + name + ": /v1/models does not report owned_by=" + owned_by +
                                     "; refusing a mislabelled engine");
        }
    }
    LOG(INFO, "Adapter") << name << " ready on port " << port_ << std::endl;
  } catch (const nlohmann::json::exception& e) {
    throw std::runtime_error(std::string("adapter spec for '") + model_name + "': a field has the wrong type (" +
                             e.what() + ")");
  }
}

namespace {

#ifdef __linux__
// The engine's process and every descendant, read from /proc (a launcher script often starts the real server).
std::vector<int> process_tree(int root) {
    std::vector<int> out;
    if (root <= 0) return out;
    std::vector<int> todo{root};
    while (!todo.empty()) {
        const int pid = todo.back();
        todo.pop_back();
        out.push_back(pid);
        std::error_code ec;
        for (const auto& task : fs::directory_iterator("/proc/" + std::to_string(pid) + "/task", ec)) {
            std::ifstream children(task.path() / "children");
            for (int child; children >> child;) todo.push_back(child);
        }
    }
    return out;
}

// pid -> MiB for every process holding GPU memory, from nvidia-smi and rocm-smi when they are installed.
std::map<int, long> gpu_holders() {
    std::map<int, long> out;
    const auto run = [](const char* cmd) {
        std::string text;
        if (FILE* p = popen(cmd, "r")) {
            char buf[512];
            while (fgets(buf, sizeof(buf), p)) text += buf;
            pclose(p);
        }
        return text;
    };
    std::istringstream nv(run("nvidia-smi --query-compute-apps=pid,used_memory --format=csv,noheader,nounits 2>/dev/null"));
    for (std::string line; std::getline(nv, line);) {
        int pid = 0;
        long mib = 0;
        if (std::sscanf(line.c_str(), "%d, %ld", &pid, &mib) == 2) out[pid] += mib;
    }
    std::istringstream amd(run("rocm-smi --showpids 2>/dev/null"));
    for (std::string line; std::getline(amd, line);) {
        int pid = 0;
        if (std::sscanf(line.c_str(), "%d", &pid) == 1 && pid > 1) out[pid] += 0;   // rocm-smi lists holders, not sizes
    }
    return out;
}
#endif

}  // namespace

void AdapterServer::unload() {
#ifdef __linux__
    const std::vector<int> tree = process_tree(get_process_id());
#endif
    stop_server();
#ifdef __linux__
    if (tree.empty()) return;
    // A device frees its memory a moment after the process exits; give it time before calling it a leak.
    std::map<int, long> held;
    for (int i = 0; i < 15; ++i) {
        held.clear();
        const auto holders = gpu_holders();
        for (int pid : tree) {
            if (const auto it = holders.find(pid); it != holders.end()) held[pid] = it->second;
        }
        if (held.empty()) {
            LOG(INFO, "Adapter") << spec_name_ << ": unloaded, device memory released (checked on the GPU)" << std::endl;
            return;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    for (const auto& [pid, mib] : held) {
        LOG(WARNING, "Adapter") << spec_name_ << ": engine reported stopped but pid " << pid << " still holds "
                                << mib << " MiB on a GPU; stopping it" << std::endl;
        ::kill(pid, SIGKILL);
    }
#endif
}

json AdapterServer::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", request);
}

json AdapterServer::completion(const json& request) {
    if (!completion_) {
        throw std::runtime_error("adapter " + spec_name_ + ": this engine has no /v1/completions");
    }
    return forward_request("/v1/completions", request);
}

std::map<std::string, nlohmann::json> AdapterServer::get_additional_telemetry() {
    if (get_backend_port() <= 0 || (telemetry_path_.empty() && memory_path_.empty())) return {};
    std::map<std::string, nlohmann::json> out;
    if (!telemetry_path_.empty()) {
        try {
            out["adapter_stats"] = forward_get_request(telemetry_path_, 2);
        } catch (...) {
            out["adapter_stats"] = nullptr;
        }
    }
    // R9: the engine's memory by device and tier, plus the one number lemond's evictor would use.
    if (!memory_path_.empty()) {
        try {
            out["adapter_memory"] = adapter::specs::memory_telemetry(forward_get_request(memory_path_, 2));
        } catch (const std::exception& e) {
            out["adapter_memory"] = nlohmann::json{{"error", e.what()}};
        }
    }
    return out;
}

std::string AdapterServer::get_additional_telemetry_url() const {
    if (get_backend_port() <= 0 || telemetry_path_.empty()) return "";
    return get_base_url() + telemetry_path_;
}

}  // namespace backends
}  // namespace lemon

namespace lemon {
namespace backends {
namespace adapter {

namespace {
// The model is a local path ("<spec>::<path>") the engine reads itself: "downloaded" means it exists
// here and its spec is installed. Nothing is fetched by lemond.
class AdapterOps : public BackendOps {
public:
    std::string resolve_checkpoint_path(const ModelInfo& info,
                                        const CheckpointResolveContext& /*ctx*/) const override {
        return split_checkpoint(info.checkpoint()).second;
    }
    bool is_downloaded(const ModelInfo& info, const BackendOpsContext& /*ctx*/) const override {
        const auto [name, path] = split_checkpoint(info.checkpoint());
        std::error_code ec;
        return !name.empty() && fs::exists(fs::path(path), ec) &&
               fs::exists(fs::path(adapters_dir()) / (name + ".json"), ec);
    }
    void download_model(const ModelInfo& info, bool /*do_not_upgrade*/, DownloadProgressCallback /*progress*/,
                        const BackendOpsContext& ctx) const override {
        if (is_downloaded(info, ctx)) return;
        const auto [name, path] = split_checkpoint(info.checkpoint());
        if (name.empty()) {
            throw std::runtime_error("adapter checkpoints are \"<spec>::<model path>\"; got '" + info.checkpoint() + "'");
        }
        throw std::runtime_error("adapter " + name + ": '" + path + "' or the spec " +
                                 (fs::path(adapters_dir()) / (name + ".json")).string() + " is missing on this machine");
    }
};
}  // namespace

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<AdapterServer>(ctx);
}

const BackendSpec* spec() {
    return make_spec<AdapterServer>(descriptor);
}

const BackendOps* ops() {
    static const AdapterOps kOps;
    return &kOps;
}

}  // namespace adapter
}  // namespace backends
}  // namespace lemon
