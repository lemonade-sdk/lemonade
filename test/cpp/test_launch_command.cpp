// Proves /health reads a backend's PID and launch command as one snapshot of the
// ServerProcess the WrappedServer owns: starting a server publishes both, a
// restart replaces the previous command, and stopping or failing to start
// clears both together. None of this is reachable from /health: a restart needs
// a backend to be started twice (in production, a watchdog reset), and cleanup
// is invisible because Router::get_all_loaded_models() drops dead backends
// before it builds any JSON.

#include "lemon/wrapped_server.h"

#include <httplib.h>

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(const std::string& what, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

}  // namespace

namespace lemon {

// A server that reports itself running under `pid` without spawning anything,
// so the test controls liveness and never signals a real process.
class FakeProcess : public ServerProcess {
public:
    explicit FakeProcess(int pid) : ServerProcess(ProcessOutput{}), pid_(pid) {}
    ~FakeProcess() override { handle_ = {nullptr, 0}; }

    std::string start(const ServerCommand& command) override {
        command_line_ = {command.program};
        command_line_.insert(command_line_.end(), command.args.begin(), command.args.end());
        handle_.pid = pid_;
        return "127.0.0.1";
    }

    void stop() override { handle_ = {nullptr, 0}; }

    bool running() const override { return handle_.pid != 0; }

private:
    int pid_;
};

// Minimal WrappedServer that runs whatever ServerProcess the test hands it.
// start_server() and stop_server() are protected, so the stub republishes them.
class StubWrappedServer : public WrappedServer {
public:
    StubWrappedServer() : WrappedServer("stub", "error", nullptr, nullptr) {}

    void load(const std::string&, const ModelInfo&, const RecipeOptions&, bool) override {}

    void unload() override { stop_server(); }

    using WrappedServer::start_server;
    using WrappedServer::stop_server;
};

}  // namespace lemon

int main() {
    httplib::Server health;
    health.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("{}", "application/json");
    });
    const int port = health.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        std::printf("FAIL could not bind the ready endpoint\n");
        return 1;
    }
    std::thread health_thread([&health]() { health.listen_after_bind(); });
    health.wait_until_ready();

    lemon::ServerCommand command;
    command.program = "llama-server.exe";
    command.port = port;

    {
        lemon::StubWrappedServer server;

        check("a server that never started reports no command",
              server.get_process_info().launch_command.empty());

        command.args = {"-m", "model.gguf", "--ctx-size", "8192"};
        server.start_server(std::make_unique<lemon::FakeProcess>(101), command, 5);
        const lemon::WrappedServer::ProcessInfo first = server.get_process_info();
        check("executable lands at index 0",
              !first.launch_command.empty() && first.launch_command[0] == "llama-server.exe");
        check("arguments follow the executable in order",
              first.launch_command == std::vector<std::string>({"llama-server.exe", "-m",
                                                                "model.gguf", "--ctx-size",
                                                                "8192"}));
        check("the PID is the started process's", first.pid == 101);
        check("the standalone accessors agree with the snapshot",
              server.get_launch_command() == first.launch_command &&
                  server.get_process_id() == first.pid && server.get_backend_port() == port);

        // What a watchdog reset does: same object, second process.
        server.stop_server();
        command.args = {"-m", "model.gguf", "--port", "8082"};
        server.start_server(std::make_unique<lemon::FakeProcess>(202), command, 5);
        const lemon::WrappedServer::ProcessInfo second = server.get_process_info();
        check("a restart replaces the previous command instead of appending",
              second.launch_command == std::vector<std::string>({"llama-server.exe", "-m",
                                                                 "model.gguf", "--port",
                                                                 "8082"}));
        check("a restart reports the new PID", second.pid == 202);

        server.stop_server();
        const lemon::WrappedServer::ProcessInfo stopped = server.get_process_info();
        check("stopping erases pid and command in the same snapshot",
              stopped.pid == 0 && stopped.launch_command.empty());

        bool threw = false;
        try {
            server.start_server(std::make_unique<lemon::FakeProcess>(0), command, 5);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        const lemon::WrappedServer::ProcessInfo failed = server.get_process_info();
        check("a server that exits before it is ready fails the start", threw);
        check("a failed start leaves no pid or command behind",
              failed.pid == 0 && failed.launch_command.empty());
    }

    health.stop();
    health_thread.join();

    if (failures == 0) {
        std::printf("\nAll launch command checks passed.\n");
        return 0;
    }
    std::printf("\n%d launch command check(s) failed.\n", failures);
    return 1;
}
