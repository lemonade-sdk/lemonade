// Proves the launch command /health reports describes the server process
// WrappedServer currently owns, and that a server that never becomes ready is
// stopped and leaves nothing behind in that report.

#include "lemon/wrapped_server.h"

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(const std::string& what, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

}  // namespace

namespace lemon {

// Records its command line without starting anything, so it is never running
// and never becomes ready.
class FakeProcess : public ServerProcess {
public:
    explicit FakeProcess(bool* stopped) : stopped_(stopped) {}

    std::string start(const ServerCommand& command, bool) override {
        command_line_ = {command.program};
        command_line_.insert(command_line_.end(), command.args.begin(), command.args.end());
        return "127.0.0.1";
    }
    void stop() override { *stopped_ = true; }

private:
    bool* stopped_;
};

class StubWrappedServer : public WrappedServer {
public:
    StubWrappedServer() : WrappedServer("stub", "error", nullptr, nullptr) {}

    void load(const std::string&, const ModelInfo&, const RecipeOptions&, bool) override {}
    void unload() override { stop_server(); }

    using WrappedServer::start_server;
};

}  // namespace lemon

int main() {
    lemon::StubWrappedServer server;

    check("a server that never started reports no command",
          server.get_process_info().launch_command.empty());

    bool stopped = false;
    lemon::ServerCommand command;
    command.program = "llama-server.exe";
    command.args = {"-m", "model.gguf", "--ctx-size", "8192"};
    command.port = 8082;

    bool threw = false;
    try {
        server.start_server(std::make_unique<lemon::FakeProcess>(&stopped), command, 1);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check("a server that never becomes ready fails the load", threw);
    check("a server that never becomes ready is stopped", stopped);

    const lemon::WrappedServer::ProcessInfo cleaned = server.get_process_info();
    check("the failed server leaves no pid or command behind",
          cleaned.pid == 0 && cleaned.launch_command.empty());
    check("the failed server leaves no port behind", server.get_backend_port() == 0);

    if (failures == 0) {
        std::printf("\nAll launch command checks passed.\n");
        return 0;
    }
    std::printf("\n%d launch command check(s) failed.\n", failures);
    return 1;
}
