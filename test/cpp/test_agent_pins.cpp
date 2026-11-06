// Pinned agent versions and their checksums.
#include "agent_test_util.h"

#include "lemon/agents/agent_pins.h"
#include "lemon/utils/path_utils.h"

using agent_test::check;
using lemon::agents::AgentError;
using nlohmann::json;

static int error_status(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const AgentError& e) {
        return e.status;
    }
    return 0;
}

int main() {
    const auto& pi = lemon::agents::find_agent("pi");
    check("pi asset for linux x64", pi.asset("linux", "x64") == "pi-linux-x64.tar.gz");
    check("pi asset for darwin arm64", pi.asset("darwin", "arm64") == "pi-darwin-arm64.tar.gz");
    check("pi asset for windows x64", pi.asset("windows", "x64") == "pi-windows-x64.zip");
    check("no pi asset for riscv", pi.asset("linux", "riscv64").empty());
    check("unknown agent is a 404",
          error_status([] { lemon::agents::find_agent("nope"); }) == 404);

    const json versions = json::parse(R"({
        "pi": {"version": "v1.5.0"},
        "checksums": {"github": {"earendil-works/pi": {"v1.5.0": {
            "pi-linux-x64.tar.gz": "sha256:aa"}}}}})");
    const auto pin = lemon::agents::resolve_pin(pi, versions, "linux", "x64");
    check("pin version drops the v", pin.version == "1.5.0" && pin.tag == "v1.5.0");
    check("pin checksum", pin.sha256 == "sha256:aa");
    check("pin url",
          pin.url == "https://github.com/earendil-works/pi/releases/download/v1.5.0/"
                     "pi-linux-x64.tar.gz");
    check("base url override",
          lemon::agents::resolve_pin(pi, versions, "linux", "x64", "http://127.0.0.1:1/r").url ==
              "http://127.0.0.1:1/r/v1.5.0/pi-linux-x64.tar.gz");
    check("missing checksum is an error",
          error_status([&] { lemon::agents::resolve_pin(pi, versions, "linux", "arm64"); }) == 500);
    check("unsupported platform is a 422",
          error_status([&] { lemon::agents::resolve_pin(pi, versions, "linux", "riscv64"); }) ==
              422);
    check("missing version is an error",
          error_status([&] { lemon::agents::resolve_pin(pi, json::object(), "linux", "x64"); }) ==
              500);

    // The shipped pins cover every asset pi publishes.
    const auto shipped = lemon::agents::load_agent_versions();
    bool all = true;
    for (const char* os : {"linux", "darwin", "windows"}) {
        for (const char* arch : {"x64", "arm64"}) {
            all = all && error_status([&] { lemon::agents::resolve_pin(pi, shipped, os, arch); }) == 0;
        }
    }
    check("shipped agent_versions.json pins every pi asset", all);
    return agent_test::finish();
}
