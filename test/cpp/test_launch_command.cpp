// Proves the recorded launch command always describes the process currently in
// process_handle_, and that /health reads both halves as one snapshot rather
// than two independently locked reads. Neither half is reachable from /health:
// a restart replacing the previous command needs a backend to be started twice
// (in production, a watchdog reset), and cleanup erasing it is invisible because
// Router::get_all_loaded_models() drops dead backends before it builds any JSON.

#include "lemon/wrapped_server.h"

#include <cstdio>
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

// Minimal WrappedServer that never spawns a subprocess. Only load()/unload() are
// pure virtual; set_process_handle() and the cleanup consumer are protected, so
// the stub republishes them for the test.
class StubWrappedServer : public WrappedServer {
public:
    StubWrappedServer() : WrappedServer("stub", "error", nullptr, nullptr) {}

    void load(const std::string&, const ModelInfo&, const RecipeOptions&, bool) override {}

    void unload() override {}

    using WrappedServer::consume_process_handle_for_cleanup;
    using WrappedServer::set_process_handle;
};

}  // namespace lemon

int main() {
    lemon::StubWrappedServer server;

    // No real child process is started, so the handle stays empty on every
    // platform: has_process_handle() is false for both {nullptr} and {pid 0}.
    const lemon::ProcessHandle handle{nullptr, 0};

    check("a server that never started reports no command",
          server.get_process_info().launch_command.empty());

    server.set_process_handle(handle, "llama-server.exe",
                              {"-m", "model.gguf", "--ctx-size", "8192"});
    const std::vector<std::string> first = server.get_process_info().launch_command;
    check("executable lands at index 0",
          !first.empty() && first[0] == "llama-server.exe");
    check("arguments follow the executable in order",
          first == std::vector<std::string>({"llama-server.exe", "-m", "model.gguf",
                                             "--ctx-size", "8192"}));
    check("the standalone accessor agrees with the snapshot",
          server.get_launch_command() == first &&
              server.get_process_id() == server.get_process_info().pid);

    // What a watchdog reset does: same object, second process, new port.
    server.set_process_handle(handle, "llama-server.exe",
                              {"-m", "model.gguf", "--port", "8082"});
    check("a restart replaces the previous command instead of appending",
          server.get_process_info().launch_command ==
              std::vector<std::string>({"llama-server.exe", "-m", "model.gguf",
                                        "--port", "8082"}));

    server.consume_process_handle_for_cleanup();
    const lemon::WrappedServer::ProcessInfo cleaned = server.get_process_info();
    check("cleanup erases pid and command in the same snapshot",
          cleaned.pid == 0 && cleaned.launch_command.empty());

    if (failures == 0) {
        std::printf("\nAll launch command checks passed.\n");
        return 0;
    }
    std::printf("\n%d launch command check(s) failed.\n", failures);
    return 1;
}
