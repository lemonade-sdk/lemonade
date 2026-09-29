#pragma once

#include <string>
#include <utility>
#include <vector>

#include "lemon/utils/process_manager.h"

namespace lemon {

// The program a backend runs, with its flags, environment variables, model
// files, port and ready endpoint.
struct ServerCommand {
    std::string program;
    std::vector<std::string> args;
    std::vector<std::pair<std::string, std::string>> env;
    std::vector<std::string> model_files;
    int port = 0;
    std::string ready_endpoint = "/health";
};

// Where a server's stdout and stderr go.
struct ProcessOutput {
    bool inherit = false;
    bool filter_health_logs = false;
};

// One running backend server. WrappedServer owns one while a model is loaded.
class ServerProcess {
public:
    explicit ServerProcess(ProcessOutput output) : output_(output) {}
    virtual ~ServerProcess() = default;
    ServerProcess(const ServerProcess&) = delete;
    ServerProcess& operator=(const ServerProcess&) = delete;

    // Starts the server and returns the host that serves `command.port`.
    virtual std::string start(const ServerCommand& command) = 0;
    // Terminates the server, or reaps it and logs its exit code when it has
    // already exited. Does nothing when it never started.
    virtual void stop();
    // Non-mutating, so status checks can call it as often as they like.
    virtual bool running() const;

    utils::ProcessHandle handle() const { return handle_; }
    const std::vector<std::string>& command_line() const { return command_line_; }

protected:
    // Runs `command_line` as a child of lemond.
    void spawn(std::vector<std::string> command_line, const std::string& working_dir,
               const std::vector<std::pair<std::string, std::string>>& env);

    utils::ProcessHandle handle_{nullptr, 0};
    std::vector<std::string> command_line_;

private:
    ProcessOutput output_;
};

// A backend server that runs as a binary on the host.
class NativeProcess : public ServerProcess {
public:
    explicit NativeProcess(ProcessOutput output, std::string working_dir = "")
        : ServerProcess(output), working_dir_(std::move(working_dir)) {}

    std::string start(const ServerCommand& command) override;

private:
    std::string working_dir_;
};

}  // namespace lemon
