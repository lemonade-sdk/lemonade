#pragma once

#include <string>
#include <utility>
#include <vector>

#include "lemon/utils/process_manager.h"

namespace lemon {

// What a backend wants to run. It has the same shape for native and container
// backends; the ServerProcess decides where it runs.
struct ServerCommand {
    // A native server's executable path, or a container's program name on its
    // image's PATH ("" runs the image's own entrypoint).
    std::string program;
    std::vector<std::string> args;
    std::vector<std::pair<std::string, std::string>> env;
    // Files the server reads. A container mounts each one and sees it at a new
    // path, so any arg or env value equal to one of these is rewritten to it.
    std::vector<std::string> model_files;
    int port = 0;
    std::string ready_endpoint = "/health";
};

class ServerProcess {
public:
    virtual ~ServerProcess() = default;

    // Starts `command` and returns the host that serves it.
    virtual std::string start(const ServerCommand& command, bool inherit_output) = 0;
    // Terminates the child, or reaps it and logs its exit code when it has
    // already exited.
    virtual void stop();

    bool running() const { return utils::ProcessManager::is_running(handle_); }
    int pid() const { return handle_.pid; }
    const std::vector<std::string>& command_line() const { return command_line_; }

protected:
    void spawn(const std::string& working_dir, bool inherit_output,
               const std::vector<std::pair<std::string, std::string>>& env);

    utils::ProcessHandle handle_{nullptr, 0};
    std::vector<std::string> command_line_;
};

class NativeProcess : public ServerProcess {
public:
    explicit NativeProcess(std::string working_dir = "") : working_dir_(std::move(working_dir)) {}

    std::string start(const ServerCommand& command, bool inherit_output) override;

private:
    std::string working_dir_;
};

// Runs the backend's pinned image, resolved when it starts.
class ContainerProcess : public ServerProcess {
public:
    ContainerProcess(std::string recipe, std::string backend, std::string model);

    std::string start(const ServerCommand& command, bool inherit_output) override;
    // Stops the container before the client: the client forwards SIGTERM into
    // the container, and SIGKILL would end the client alone.
    void stop() override;

private:
    std::string recipe_;
    std::string backend_;
    std::string model_;
    std::string name_;
};

}  // namespace lemon
