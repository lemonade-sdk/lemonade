#include "lemon/server_process.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <thread>

#include <lemon/utils/aixlog.hpp>
#include "lemon/backends/container_backend.h"
#include "lemon/system_info.h"
#include "lemon/utils/container_manager.h"

namespace fs = std::filesystem;

namespace lemon {

using utils::ContainerManager;
using utils::ProcessManager;

namespace {

constexpr const char* kModelsMountRoot = "/mnt/models";

std::string join_command_line(const std::vector<std::string>& args) {
    std::string out;
    for (const auto& arg : args) {
        if (!out.empty()) out += ' ';
        out += arg.find_first_of(" \t\"'") == std::string::npos ? arg : "'" + arg + "'";
    }
    return out;
}

bool has_handle(const utils::ProcessHandle& handle) {
#ifdef _WIN32
    return handle.handle != nullptr;
#else
    return handle.pid > 0;
#endif
}

}  // namespace

void ServerProcess::spawn(const std::string& working_dir, bool inherit_output,
                          const std::vector<std::pair<std::string, std::string>>& env) {
    const std::vector<std::string> args(command_line_.begin() + 1, command_line_.end());
    handle_ = ProcessManager::start_process(command_line_.front(), args, working_dir,
                                            inherit_output, /*filter_health_logs=*/true, env);
    if (!has_handle(handle_)) {
        throw std::runtime_error("Failed to start " + command_line_.front());
    }
    LOG(INFO, "ServerProcess") << "Started " << command_line_.front() << " (PID " << handle_.pid
                               << ")" << std::endl;
}

void ServerProcess::stop() {
    if (!has_handle(handle_)) return;
    if (running()) {
        ProcessManager::stop_process(handle_);
    } else {
        const int exit_code = ProcessManager::reap_process(handle_);
        LOG(INFO, "ServerProcess") << command_line_.front() << " (PID " << handle_.pid
                                   << ") had exited with code " << exit_code << std::endl;
    }
    handle_ = {nullptr, 0};
}

std::string NativeProcess::start(const ServerCommand& command, bool inherit_output) {
    command_line_ = {command.program};
    command_line_.insert(command_line_.end(), command.args.begin(), command.args.end());
    spawn(working_dir_, inherit_output, command.env);
    return "127.0.0.1";
}

ContainerProcess::ContainerProcess(std::string recipe, std::string backend, std::string model)
    : recipe_(std::move(recipe)),
      backend_(std::move(backend)),
      model_(std::move(model)) {}

std::string ContainerProcess::start(const ServerCommand& command, bool inherit_output) {
    auto& manager = ContainerManager::global();

    utils::ContainerRunSpec spec;
    spec.image = backends::pinned_image_or_throw(recipe_, backend_);
    name_ = ContainerManager::container_name(recipe_, backend_, model_);
    spec.name = name_;
    const std::string label = ContainerManager::managed_label();
    spec.labels = {{label + ".recipe", recipe_},
                   {label + ".backend", backend_},
                   {label + ".model", model_},
                   {label + ".port", std::to_string(command.port)}};
    spec.port = command.port;
    spec.env = command.env;

    const auto& devices = spec.image.devices;
    if (std::find(devices.begin(), devices.end(), "/dev/kfd") != devices.end()) {
        const std::string gpu_index =
            ContainerManager::kfd_gpu_index_for_arch(SystemInfo::get_rocm_arch());
        if (!gpu_index.empty()) spec.env.push_back({"HIP_VISIBLE_DEVICES", gpu_index});
    }

    // A container left by a killed lemond would make `run` fail on the name.
    manager.remove_container(name_);

    // Inside a container lemond's loopback is its own network namespace, so the
    // server joins it. Everywhere else it gets a private internal network with
    // no route out.
    const std::string self_id = manager.self_container_id();
    if (!self_id.empty()) {
        spec.network = "container:" + self_id;
    } else {
        manager.ensure_isolated_network(name_);
        spec.network = name_;
    }

    // Each model file is resolved through the Hugging Face cache's symlinks and
    // mounted alone under /mnt/models, so the container sees exactly the files
    // it was given and the path inside is the same on every host.
    std::map<std::string, std::string> inside_path;  // path as the command spells it -> inside
    std::map<std::string, std::string> claimed;      // inside -> its canonical source
    for (const auto& model_file : command.model_files) {
        if (model_file.empty()) continue;
        std::error_code ec;
        const fs::path canonical = fs::canonical(model_file, ec);
        if (ec) {
            throw std::runtime_error("Model path '" + model_file + "' does not exist");
        }
        const std::string visible = canonical.string();
        if (const auto seen = inside_path.find(visible); seen != inside_path.end()) {
            inside_path[model_file] = seen->second;
            continue;
        }
        // Two different files can share a basename; the second one gets a
        // numbered directory instead of shadowing the first.
        const std::string file_name = fs::path(model_file).filename().string();
        std::string inside = std::string(kModelsMountRoot) + "/" + file_name;
        for (int n = 2; claimed.count(inside); ++n) {
            inside = std::string(kModelsMountRoot) + "/" + std::to_string(n) + "/" + file_name;
        }
        claimed[inside] = visible;
        inside_path[visible] = inside;
        inside_path[model_file] = inside;
        spec.mounts.push_back(manager.host_mount(visible, inside));
    }
    const auto rewrite = [&inside_path](std::string& value) {
        if (const auto it = inside_path.find(value); it != inside_path.end()) value = it->second;
    };

    if (!command.program.empty()) spec.command.push_back(command.program);
    spec.command.insert(spec.command.end(), command.args.begin(), command.args.end());
    for (auto& arg : spec.command) rewrite(arg);
    for (auto& entry : spec.env) rewrite(entry.second);

    command_line_ = manager.run_command(spec);
    LOG(INFO, "Container") << "Starting " << name_ << ": " << join_command_line(command_line_)
                           << std::endl;
    spawn("", inherit_output, {});

    const auto tool = manager.info();
    if (tool && ContainerManager::connects_on_loopback(spec, tool->tool)) return "127.0.0.1";

    // podman or docker attaches the container's address asynchronously.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        const std::string address = manager.container_address(name_);
        if (!address.empty()) return address;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    throw std::runtime_error(name_ + " got no network address");
}

void ContainerProcess::stop() {
    ContainerManager::global().stop(name_);
    ServerProcess::stop();
}

}  // namespace lemon
