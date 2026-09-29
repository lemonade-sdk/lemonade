#include "lemon/server_process.h"

#include <stdexcept>

#include <lemon/utils/aixlog.hpp>

namespace lemon {

using utils::ProcessManager;

namespace {

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
        LOG(INFO, "ServerProcess") << command_line_.front() << " (PID " << handle_.pid
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

}  // namespace lemon
