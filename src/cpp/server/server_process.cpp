#include "lemon/server_process.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>

#include <lemon/utils/aixlog.hpp>
#include "lemon/system_info.h"
#include "lemon/utils/container_manager.h"

namespace fs = std::filesystem;

namespace lemon {

using utils::ContainerManager;
using utils::ProcessManager;

namespace {

constexpr const char* kModelsDir = "/mnt/models";
constexpr int kAddressTimeoutSeconds = 30;

std::string join_command_line(const std::vector<std::string>& argv) {
    std::string out;
    for (const auto& arg : argv) {
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

void ServerProcess::spawn(std::vector<std::string> command_line, const std::string& working_dir,
                          const std::vector<std::pair<std::string, std::string>>& env) {
    command_line_ = std::move(command_line);
    const std::vector<std::string> args(command_line_.begin() + 1, command_line_.end());
    handle_ = ProcessManager::start_process(command_line_.front(), args, working_dir,
                                            output_.inherit, output_.filter_health_logs, env);
    if (!has_handle(handle_)) {
        throw std::runtime_error("Failed to start " + command_line_.front());
    }
    LOG(INFO, "ServerProcess") << "Started " << command_line_.front() << " (PID " << handle_.pid
                               << ")" << std::endl;
}

ServerProcess::~ServerProcess() {
    ServerProcess::stop();
}

void ServerProcess::stop() {
    if (!has_handle(handle_)) {
        return;
    }
    if (ProcessManager::is_running(handle_)) {
        LOG(INFO, "ServerProcess") << "Stopping " << command_line_.front() << " (PID "
                                   << handle_.pid << ")" << std::endl;
        ProcessManager::stop_process(handle_);
    } else {
        const int exit_code = ProcessManager::reap_process(handle_);
        LOG(ERROR, "ServerProcess") << command_line_.front() << " (PID " << handle_.pid
                                    << ") had exited with code " << exit_code << std::endl;
    }
    handle_ = {nullptr, 0};
}

bool ServerProcess::running() const {
    return has_handle(handle_) && ProcessManager::is_running(handle_);
}

std::string NativeProcess::start(const ServerCommand& command) {
    std::vector<std::string> command_line = {command.program};
    command_line.insert(command_line.end(), command.args.begin(), command.args.end());
    spawn(std::move(command_line), working_dir_, command.env);
    return "127.0.0.1";
}

ContainerProcess::ContainerProcess(ProcessOutput output, std::string recipe, std::string backend,
                                   std::string model, ContainerPolicy policy, std::string image)
    : ServerProcess(output),
      recipe_(std::move(recipe)),
      backend_(std::move(backend)),
      model_(std::move(model)),
      policy_(std::move(policy)),
      image_(std::move(image)) {}

ContainerProcess::~ContainerProcess() {
    ContainerProcess::stop();
}

std::string ContainerProcess::start(const ServerCommand& command) {
#ifndef __linux__
    throw std::runtime_error(recipe_ + ":" + backend_ +
                             " is a container backend, which runs on Linux");
#endif
    const auto& manager = ContainerManager::global();
    if (auto failure = manager.check_setup()) {
        throw std::runtime_error(recipe_ + ":" + backend_ + " cannot start: " + failure->text());
    }
    if (!ContainerManager::allowed_repository(policy_.repository)) {
        throw std::runtime_error(recipe_ + ":" + backend_ + " names the repository " +
                                 policy_.repository +
                                 ", which is not one Lemonade runs container backends from");
    }
    const auto tool = manager.tool();

    name_ = ContainerManager::container_name(recipe_, backend_, model_);
    manager.remove_container(name_);

    utils::ContainerRunSpec spec;
    spec.name = name_;
    spec.recipe = recipe_;
    spec.backend = backend_;
    spec.model = model_;
    spec.port = command.port;
    spec.image = image_;
    spec.devices = policy_.devices;
    spec.cap_add = policy_.cap_add;
    spec.ipc_host = policy_.ipc_host;
    spec.memlock_unlimited = policy_.memlock_unlimited;

    // Each model file or directory is mounted on its own under /mnt/models,
    // so the container sees exactly what the command names.
    std::map<std::string, std::string> inside;  // path as the command names it -> path inside
    std::set<std::string> destinations;
    for (const auto& model_file : command.model_files) {
        if (model_file.empty() || inside.count(model_file)) continue;
        std::error_code ec;
        const fs::path source = fs::canonical(fs::path(model_file), ec);
        if (ec) {
            throw std::runtime_error("Model file '" + model_file + "' does not exist");
        }
        fs::path named(model_file);
        if (!named.has_filename()) named = named.parent_path();
        const std::string file_name = named.filename().string();
        std::string destination = std::string(kModelsDir) + "/" + file_name;
        // Two files can share a name; the second gets a numbered directory.
        for (int n = 2; destinations.count(destination); ++n) {
            destination = std::string(kModelsDir) + "/" + std::to_string(n) + "/" + file_name;
        }
        destinations.insert(destination);
        spec.mounts.push_back({source.string(), destination});
        inside[model_file] = destination;
    }
    const auto rewrite = [&inside](const std::string& value) {
        const auto it = inside.find(value);
        return it == inside.end() ? value : it->second;
    };

    if (std::find(policy_.devices.begin(), policy_.devices.end(), "/dev/kfd") !=
        policy_.devices.end()) {
        const std::string gpu = ContainerManager::kfd_gpu_index(SystemInfo::get_rocm_arch());
        if (!gpu.empty()) spec.env.push_back({"HIP_VISIBLE_DEVICES", gpu});
    }
    for (const auto& [key, value] : command.env) {
        spec.env.push_back({key, rewrite(value)});
    }
    if (!command.program.empty()) spec.command.push_back(command.program);
    for (const auto& arg : command.args) {
        spec.command.push_back(rewrite(arg));
    }

    manager.create_network(name_);
    spec.network = name_;

    std::vector<std::string> command_line = manager.run_command(spec);
    LOG(INFO, "ContainerProcess") << "Starting " << name_ << ": "
                                  << join_command_line(command_line) << std::endl;
    spawn(std::move(command_line), "", {});

    if (tool == utils::ContainerTool::Podman) {
        return "127.0.0.1";
    }
    // Docker attaches the container's address asynchronously after `run` starts.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(kAddressTimeoutSeconds);
    while (running() && std::chrono::steady_clock::now() < deadline) {
        const std::string address = manager.address(name_);
        if (!address.empty()) return address;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (!running()) {
        throw std::runtime_error(name_ + " exited before it started; its error is in the log above");
    }
    throw std::runtime_error(name_ + " got no address on its network");
}

void ContainerProcess::stop() {
    if (!name_.empty()) {
        ContainerManager::global().stop(name_);
        name_.clear();
    }
    ServerProcess::stop();
}

}  // namespace lemon
