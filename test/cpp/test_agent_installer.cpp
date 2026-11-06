// Installing, upgrading and removing a managed agent from fixture releases
// served on loopback.
#include "agent_test_util.h"

#include <atomic>
#include <map>
#include <thread>

#include <httplib.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "lemon/agents/agent_installer.h"
#include "lemon/agents/host_protocol.h"
#include "lemon/utils/process_manager.h"

using agent_test::check;
using agent_test::read_file;
using agent_test::write_file;
namespace fs = std::filesystem;
using namespace lemon::agents;

namespace {

int run(const std::string& exe, const std::vector<std::string>& args, const fs::path& cwd) {
    return lemon::utils::ProcessManager::run_process_with_output(
        exe, args, [](const std::string&) { return true; }, cwd.string(), 60, false);
}

// Builds the release asset the way pi publishes it: a tarball whose entries
// sit under pi/, or a flat zip on Windows.
std::string build_asset(const fs::path& work, const std::string& name, bool with_binary,
                        const std::string& symlink_target = "") {
    const fs::path tree = work / ("tree-" + name);
    fs::remove_all(tree);
#ifdef _WIN32
    const fs::path root = tree;
#else
    const fs::path root = tree / "pi";
#endif
    write_file(root / "theme" / "dark.json", "{}");
    if (with_binary) {
#ifdef _WIN32
        write_file(root / "pi.exe", "MZ");
#else
        write_file(root / "pi", "#!/bin/sh\necho pi " + name + "\n");
#endif
    }
#ifndef _WIN32
    if (symlink_target == "chain") {
        // Each link is lexically inside the payload; following them climbs out.
        fs::create_symlink(".", root / "m1");
        fs::create_symlink("m1/..", root / "m2");
    } else if (!symlink_target.empty()) {
        fs::create_symlink(symlink_target, root / "escape");
    }
#endif
    const fs::path archive = work / name;
#ifdef _WIN32
    run("tar", {"-a", "-cf", archive.string(), "-C", tree.string(), "."}, work);
#else
    run("tar", {"-czf", archive.string(), "-C", tree.string(), "pi"}, work);
#endif
    return read_file(archive);
}

std::string sha256_of(const std::string& data) {
    return "sha256:" + protocol::token_hash(data);
}

int error_status(const std::function<void()>& fn, std::string* code = nullptr) {
    try {
        fn();
    } catch (const AgentError& e) {
        if (code != nullptr) *code = e.code;
        return e.status;
    }
    return 0;
}

} // namespace

int main() {
    const fs::path work = agent_test::temp_dir("agent-installer");
    AgentPaths paths;
    paths.data_root = work / "data" / "lemonade" / "agents";
    paths.bin_dir = work / "bin";
    paths.state_dir = work / "state";

    const std::string asset = find_agent("pi").asset(host_os(), host_arch());
    std::map<std::string, std::string> releases;  // "<tag>/<asset>" -> bytes
    releases["v1.5.0/" + asset] = build_asset(work, "a1", true);
    releases["v1.6.0/" + asset] = build_asset(work, "a2", true);
    releases["v2.0.0/" + asset] = build_asset(work, "a3", false);
#ifndef _WIN32
    releases["v3.0.0/" + asset] = build_asset(work, "a4", true, "../../../../etc/passwd");
    releases["v3.1.0/" + asset] = build_asset(work, "a5", true, "chain");
    releases["v1.7.0/" + asset] = build_asset(work, "a6", true);
#endif

    std::atomic<int> requests{0};
    httplib::Server server;
    server.Get(R"(/r/(v[^/]+)/(.+))", [&](const httplib::Request& req, httplib::Response& res) {
        ++requests;
        auto it = releases.find(req.matches[1].str() + "/" + req.matches[2].str());
        if (it == releases.end()) {
            res.status = 404;
            return;
        }
        res.set_content(it->second, "application/octet-stream");
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    std::thread server_thread([&] { server.listen_after_bind(); });
    server.wait_until_ready();
    const std::string base = "http://127.0.0.1:" + std::to_string(port) + "/r";

    const auto& pi = find_agent("pi");
    auto pin_for = [&](const std::string& tag, const std::string& sha = "") {
        AgentPin pin;
        pin.tag = tag;
        pin.version = tag.substr(1);
        pin.asset = asset;
        pin.sha256 = sha.empty() ? sha256_of(releases[tag + "/" + asset]) : sha;
        pin.url = base + "/" + tag + "/" + asset;
        return pin;
    };
    const auto loopback = lemon::utils::HttpSecurityPolicy::TrustedLoopback;
    const lemon::utils::CancelToken no_cancel;

    size_t last_progress = 0;
    const auto first = install_agent(pi, pin_for("v1.5.0"), paths, false,
                                     [&](size_t done, size_t) { last_progress = done; return true; },
                                     no_cancel, loopback);
    const fs::path shim = shim_path(paths, "pi");
    check("install downloads", first.downloaded && last_progress > 0);
    check("install commits <ver>/ with the binary",
          fs::is_regular_file(first.installed.binary) &&
              first.installed.dir == paths.agent_dir("pi") / "1.5.0");
    check("tarball's pi/ top dir is stripped",
          fs::exists(first.installed.dir / "theme" / "dark.json"));
    check("install writes the manifest",
          fs::exists(first.installed.dir / ".lemonade-install.json"));
    check("install writes our shim", first.shim == ShimState::Ours &&
                                         read_file(shim).find(first.installed.binary.string()) !=
                                             std::string::npos);
    check("find_installed sees it", find_installed(pi, paths).has_value() &&
                                        find_installed(pi, paths)->version == "1.5.0");

    const int before = requests;
    const auto again = install_agent(pi, pin_for("v1.5.0"), paths, false, nullptr, no_cancel,
                                     loopback);
    check("reinstall is a no-op", !again.downloaded && requests == before);

    std::string code;
    check("checksum mismatch is a 502",
          error_status([&] {
              install_agent(pi, pin_for("v1.6.0", sha256_of("other")), paths, false, nullptr,
                            no_cancel, loopback);
          }, &code) == 502 && code == "checksum_mismatch");
    check("checksum mismatch leaves no <ver>/", !fs::exists(paths.agent_dir("pi") / "1.6.0"));

    check("an asset without the binary fails",
          error_status([&] {
              install_agent(pi, pin_for("v2.0.0"), paths, false, nullptr, no_cancel, loopback);
          }, &code) == 500);
    check("a failed install keeps the old version",
          fs::is_regular_file(first.installed.binary) && !fs::exists(paths.agent_dir("pi") / "2.0.0") &&
              !fs::exists(paths.agent_dir("pi") / "2.0.0.staging"));
#ifndef _WIN32
    check("a symlink escaping the payload is rejected",
          error_status([&] {
              install_agent(pi, pin_for("v3.0.0"), paths, false, nullptr, no_cancel, loopback);
          }) == 500 && !fs::exists(paths.agent_dir("pi") / "3.0.0"));
    check("chained symlinks climbing out of the payload are rejected",
          error_status([&] {
              install_agent(pi, pin_for("v3.1.0"), paths, false, nullptr, no_cancel, loopback);
          }) == 500 && !fs::exists(paths.agent_dir("pi") / "3.1.0"));
#endif

    const auto upgraded = install_agent(pi, pin_for("v1.6.0"), paths, false, nullptr, no_cancel,
                                        loopback);
    check("upgrade moves the shim", read_file(shim).find(upgraded.installed.binary.string()) !=
                                        std::string::npos);
    check("upgrade keeps the version it replaced, which a running pi may still use",
          fs::exists(paths.agent_dir("pi") / "1.5.0"));
    check("find_installed picks the newest version", find_installed(pi, paths)->version == "1.6.0");

    const auto forced = install_agent(pi, pin_for("v1.6.0"), paths, true, nullptr, no_cancel,
                                      loopback);
    check("force reinstalls", forced.downloaded && fs::is_regular_file(forced.installed.binary));
    check("the next install removes the version before the last",
          !fs::exists(paths.agent_dir("pi") / "1.5.0"));

    const auto downgraded = install_agent(pi, pin_for("v1.5.0"), paths, false, nullptr, no_cancel,
                                          loopback);
    check("after a downgrade the pinned version is the active one",
          find_active(pi, paths, "1.5.0")->version == "1.5.0" &&
              read_file(shim).find(downgraded.installed.binary.string()) != std::string::npos);
    check("without the pinned version installed, the newest is active",
          find_active(pi, paths, "9.9.9")->version == "1.6.0");

    {
        // Downgrading to a version that was kept keeps the one it replaces.
        AgentPaths kept = paths;
        kept.data_root = work / "kept" / "agents";
        kept.bin_dir = work / "kept" / "bin";
        for (const char* tag : {"v1.5.0", "v1.6.0", "v1.5.0"}) {
            install_agent(pi, pin_for(tag), kept, false, nullptr, no_cancel, loopback);
        }
        check("a downgrade to a kept version keeps the version it replaced",
              fs::exists(kept.agent_dir("pi") / "1.6.0") &&
                  find_active(pi, kept, "1.5.0")->version == "1.5.0");
    }
    {
        // A parent directory named like a version doesn't fool the shim check.
        AgentPaths odd = paths;
        odd.data_root = work / "1.5.0" / "agents";
        odd.bin_dir = work / "odd-bin";
        for (const char* tag : {"v1.5.0", "v1.6.0", "v1.7.0"}) {
            install_agent(pi, pin_for(tag), odd, false, nullptr, no_cancel, loopback);
        }
        check("the version the shim ran is kept, whatever its parent directories are called",
              fs::exists(odd.agent_dir("pi") / "1.6.0"));
    }
    {
        // With someone else's shim, launch runs the last installed version: keep it.
        AgentPaths foreign = paths;
        foreign.data_root = work / "foreign" / "agents";
        foreign.bin_dir = work / "foreign" / "bin";
        write_file(foreign.bin_dir / "pi", "#!/bin/sh\nexec /usr/bin/pi\n");
        for (const char* tag : {"v1.6.0", "v1.5.0", "v1.7.0"}) {
            install_agent(pi, pin_for(tag), foreign, false, nullptr, no_cancel, loopback);
        }
        check("with a foreign shim the last installed version is kept",
              fs::exists(foreign.agent_dir("pi") / "1.5.0"));
    }
#ifndef _WIN32
    if (geteuid() != 0) {
        // The shim can't be rewritten, so it keeps running 1.6.0: that payload must stay.
        AgentPaths stuck = paths;
        stuck.data_root = work / "stuck" / "agents";
        stuck.bin_dir = work / "stuck" / "bin";
        install_agent(pi, pin_for("v1.6.0"), stuck, false, nullptr, no_cancel, loopback);
        fs::permissions(stuck.bin_dir, fs::perms::owner_read | fs::perms::owner_exec);
        const auto blocked =
            install_agent(pi, pin_for("v1.5.0"), stuck, false, nullptr, no_cancel, loopback);
        install_agent(pi, pin_for("v1.7.0"), stuck, false, nullptr, no_cancel, loopback);
        fs::permissions(stuck.bin_dir, fs::perms::owner_all);
        check("a shim that couldn't be rewritten is reported", !blocked.warnings.empty());
        check("the payload the shim still runs is never removed",
              fs::exists(stuck.agent_dir("pi") / "1.6.0" / "pi") &&
                  read_file(stuck.bin_dir / "pi").find("1.6.0") != std::string::npos);
    }
#endif

    std::vector<std::string> warnings;
    check("uninstall removes payload and shim",
          uninstall_agent(pi, paths, warnings) && !fs::exists(paths.agent_dir("pi")) &&
              !fs::exists(shim));
    check("uninstall of nothing reports nothing removed", !uninstall_agent(pi, paths, warnings));

    write_file(shim, "#!/bin/sh\nexec /usr/bin/pi\n");
    const auto foreign = install_agent(pi, pin_for("v1.6.0"), paths, false, nullptr, no_cancel,
                                       loopback);
    check("a foreign shim is kept with a warning",
          foreign.shim == ShimState::Foreign && !foreign.warnings.empty() &&
              read_file(shim).find("/usr/bin/pi") != std::string::npos);
    uninstall_agent(pi, paths, warnings);
    check("uninstall leaves a foreign shim", fs::exists(shim));

    server.stop();
    server_thread.join();
    std::error_code ec;
    fs::remove_all(work, ec);
    return agent_test::finish();
}
