#pragma once

#include <string>
#include <utility>
#include <vector>

#include "lemon/backends/backend_descriptor.h"
#include "lemon/utils/process_manager.h"

namespace lemon {

struct ServerCommand {
    std::string program;
    std::vector<std::string> args;
    std::vector<std::pair<std::string, std::string>> env;
    std::vector<std::string> model_files;
    int port = 0;
    // Polled until it answers before the load completes, then polled by the
    // backend watchdog for as long as the server runs.
    std::string ready_endpoint = "/health";
};

struct ProcessOutput {
    bool inherit = false;
    bool filter_health_logs = false;
};

// Destroying a ServerProcess stops its server, so a load that throws after
// start() cannot leave the process running.
class ServerProcess {
public:
    explicit ServerProcess(ProcessOutput output) : output_(output) {}
    virtual ~ServerProcess();
    ServerProcess(const ServerProcess&) = delete;
    ServerProcess& operator=(const ServerProcess&) = delete;

    // Starts the server and returns the host that serves `command.port`.
    virtual std::string start(const ServerCommand& command) = 0;
    // Terminates the server, or reaps it and logs its exit code when it has
    // already exited. Does nothing when it never started.
    virtual void stop();
    virtual bool running() const;

    utils::ProcessHandle handle() const { return handle_; }
    const std::vector<std::string>& command_line() const { return command_line_; }

protected:
    void spawn(std::vector<std::string> command_line, const std::string& working_dir,
               const std::vector<std::pair<std::string, std::string>>& env);

    utils::ProcessHandle handle_{nullptr, 0};
    std::vector<std::string> command_line_;

private:
    ProcessOutput output_;
};

class NativeProcess : public ServerProcess {
public:
    explicit NativeProcess(ProcessOutput output, std::string working_dir = "")
        : ServerProcess(output), working_dir_(std::move(working_dir)) {}

    std::string start(const ServerCommand& command) override;

private:
    std::string working_dir_;
};

// A backend server that runs in a container from a pinned image, through a
// `podman run` or `docker run` client that is a child of lemond. handle() is
// that client: when lemond dies it gets SIGTERM and forwards it to the
// container.
class ContainerProcess : public ServerProcess {
public:
    // `image` is <repository>@<digest>; `model` names the container.
    ContainerProcess(ProcessOutput output, std::string recipe, std::string backend,
                     std::string model, ContainerPolicy policy, std::string image);

    std::string start(const ServerCommand& command) override;
    // Stops the container by name first: the client forwards SIGTERM into the
    // container, but SIGKILL would end only the client.
    void stop() override;

private:
    std::string recipe_;
    std::string backend_;
    std::string model_;
    ContainerPolicy policy_;
    std::string image_;
    std::string name_;
};

}  // namespace lemon
