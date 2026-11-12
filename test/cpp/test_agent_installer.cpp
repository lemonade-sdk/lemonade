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
#include "lemon/utils/path_utils.h"
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

#ifdef _WIN32
constexpr const char* kPiBinary = "pi.exe";
#else
constexpr const char* kPiBinary = "pi";
#endif

// Whether the shim runs binary. A cmd shim names it through %LOCALAPPDATA%
// when it's below that, so only the version directory and file are compared.
bool shim_names(const fs::path& shim, const fs::path& binary) {
    fs::path tail = binary.parent_path().filename() / binary.filename();
    return read_file(shim).find(tail.make_preferred().string()) != std::string::npos;
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

int main(int argc, char** argv) {
    // macOS kills a copy of a system binary, so the test runs a copy of itself as a pi.
    if (argc == 3 && std::string(argv[1]) == "--sleep") {
        std::this_thread::sleep_for(std::chrono::seconds(std::stoi(argv[2])));
        return 0;
    }
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
    releases["v1.7.0/" + asset] = build_asset(work, "a6", true);
#ifndef _WIN32
    releases["v3.0.0/" + asset] = build_asset(work, "a4", true, "../../../../etc/passwd");
    releases["v3.1.0/" + asset] = build_asset(work, "a5", true, "chain");
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
                                         shim_names(shim, first.installed.binary));
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
    check("upgrade moves the shim", shim_names(shim, upgraded.installed.binary));
    check("upgrade removes the version it replaced when nothing runs it",
          !fs::exists(paths.agent_dir("pi") / "1.5.0"));

    const auto forced = install_agent(pi, pin_for("v1.6.0"), paths, true, nullptr, no_cancel,
                                      loopback);
    check("force reinstalls", forced.downloaded && fs::is_regular_file(forced.installed.binary));


    const auto downgraded = install_agent(pi, pin_for("v1.5.0"), paths, false, nullptr, no_cancel,
                                          loopback);
    check("a downgrade points the shim at the pinned version",
          shim_names(shim, downgraded.installed.binary));
    check("find_active prefers the pinned version", find_active(pi, paths, "1.5.0")->version == "1.5.0");

    {
        // A pi still running from an old version keeps it; nothing else does.
        AgentPaths running = paths;
#ifdef _WIN32
        running.data_root = work / "running" / "agents";
        running.bin_dir = work / "running" / "bin";
        const fs::path sleeper_exe =
            fs::path(lemon::utils::get_environment_variable_utf8("SystemRoot")) / "System32" /
            "ping.exe";
        const std::vector<std::string> sleeper_args = {"-n", "30", "127.0.0.1"};
#else
        // Reached through a symlink, as with a symlinked XDG_DATA_HOME.
        fs::create_directories(work / "running-real");
        fs::create_directory_symlink(work / "running-real", work / "running");
        running.data_root = work / "running" / "agents";
        running.bin_dir = work / "running" / "bin";
#ifdef __APPLE__
        const fs::path sleeper_exe = fs::absolute(argv[0]);
        const std::vector<std::string> sleeper_args = {"--sleep", "30"};
#else
        const fs::path sleeper_exe = "/bin/sleep";
        const std::vector<std::string> sleeper_args = {"30"};
#endif
#endif
        install_agent(pi, pin_for("v1.5.0"), running, false, nullptr, no_cancel, loopback);
        const auto v16 =
            install_agent(pi, pin_for("v1.6.0"), running, false, nullptr, no_cancel, loopback);
        check("an upgrade removes a version nothing runs", !fs::exists(running.agent_dir("pi") / "1.5.0"));
        fs::copy_file(sleeper_exe, v16.installed.binary, fs::copy_options::overwrite_existing);
        auto sleeper = lemon::utils::ProcessManager::start_process(
            v16.installed.binary.string(), sleeper_args, "", false, false, {});
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        install_agent(pi, pin_for("v1.7.0"), running, false, nullptr, no_cancel, loopback);
        check("a version a process runs from is kept", fs::exists(v16.installed.binary));
        std::vector<std::string> running_warnings;
        std::string running_code;
        check("uninstall while a process runs from a version is busy",
              error_status([&] { uninstall_agent(pi, running, running_warnings); },
                           &running_code) == 409 &&
                  running_code == "agent_busy");
        check("a refused uninstall removes nothing",
              fs::exists(v16.installed.binary) && fs::exists(shim_path(running, "pi")) &&
                  find_installed(pi, running, "1.7.0").has_value());
        lemon::utils::ProcessManager::kill_process(sleeper);
        lemon::utils::ProcessManager::wait_for_exit(sleeper, 5);
        install_agent(pi, pin_for("v1.7.0"), running, true, nullptr, no_cancel, loopback);
        check("once it exits, the next install removes it", !fs::exists(running.agent_dir("pi") / "1.6.0"));
    }
    {
        // A launch that picked a version keeps it until the client has run it.
        AgentPaths leased = paths;
        leased.data_root = work / "leased" / "agents";
        leased.bin_dir = work / "leased" / "bin";
        install_agent(pi, pin_for("v1.6.0"), leased, false, nullptr, no_cancel, loopback);
        const fs::path v16 = leased.agent_dir("pi") / "1.6.0";
        VersionLeases leases;
        {
            std::lock_guard<std::mutex> lock(leases.mutex());
            leases.acquire(v16);
            leases.acquire(v16);
        }
        leases.release(v16, std::chrono::seconds(0));
        install_agent(pi, pin_for("v1.7.0"), leased, false, nullptr, no_cancel, loopback, &leases);
        check("a version another launch still uses is kept", fs::exists(v16 / kPiBinary));
        leases.release(v16, std::chrono::seconds(60));
        install_agent(pi, pin_for("v1.7.0"), leased, true, nullptr, no_cancel, loopback, &leases);
        check("a version stays through its grace period", fs::exists(v16 / kPiBinary));

        // Retiring holds the lease lock, so a launch can't pick a version mid-removal.
        VersionLeases busy;
        std::unique_lock<std::mutex> picking(busy.mutex());
        std::atomic<bool> installed{false};
        std::thread installer([&] {
            install_agent(pi, pin_for("v1.7.0"), leased, true, nullptr, no_cancel, loopback, &busy);
            installed = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        check("an install waits for a launch that is picking a version", !installed);
        picking.unlock();
        installer.join();

        VersionLeases launching;
        const fs::path v17 = leased.agent_dir("pi") / "1.7.0";
        {
            std::lock_guard<std::mutex> lock(launching.mutex());
            launching.acquire(v17);
        }
        std::vector<std::string> uninstall_warnings;
        std::string busy_code;
        check("uninstall while a launch holds a version is busy",
              error_status([&] { uninstall_agent(pi, leased, uninstall_warnings, &launching); },
                           &busy_code) == 409 &&
                  busy_code == "agent_busy");
        check("a busy uninstall removes nothing",
              fs::exists(v17 / kPiBinary) && fs::exists(shim_path(leased, "pi")));
        launching.release(v17, std::chrono::seconds(0));
        check("once released, uninstall removes everything",
              uninstall_agent(pi, leased, uninstall_warnings, &launching) &&
                  !fs::exists(leased.agent_dir("pi")) && !fs::exists(shim_path(leased, "pi")));
        check("uninstalling nothing reports nothing removed",
              !uninstall_agent(pi, leased, uninstall_warnings, &launching));

        // Leftovers of an interrupted removal don't block or count.
        install_agent(pi, pin_for("v1.5.0"), leased, false, nullptr, no_cancel, loopback, &launching);
        write_file(leased.agent_dir("pi") / ".removing-1.5.0" / "stale", "x");
        install_agent(pi, pin_for("v1.6.0"), leased, false, nullptr, no_cancel, loopback, &launching);
        check("a leftover of the same name doesn't keep an old version",
              !fs::exists(leased.agent_dir("pi") / "1.5.0") &&
                  !fs::exists(leased.agent_dir("pi") / ".removing-1.5.0"));
        uninstall_agent(pi, leased, uninstall_warnings, &launching);
        write_file(leased.agent_dir("pi") / ".removing-1.6.0" / "stale", "x");
        check("leftovers alone don't count as an uninstall",
              !uninstall_agent(pi, leased, uninstall_warnings, &launching) &&
                  !fs::exists(leased.agent_dir("pi")));
    }
#ifndef _WIN32
    if (geteuid() != 0) {
        // A shim that can't be removed fails the uninstall and keeps pi usable.
        AgentPaths locked = paths;
        locked.data_root = work / "locked" / "agents";
        locked.bin_dir = work / "locked" / "bin";
        const auto kept = install_agent(pi, pin_for("v1.6.0"), locked, false, nullptr, no_cancel, loopback);
        fs::permissions(locked.bin_dir, fs::perms::owner_read | fs::perms::owner_exec);
        std::vector<std::string> warnings_locked;
        const int status = error_status([&] { uninstall_agent(pi, locked, warnings_locked); });
        fs::permissions(locked.bin_dir, fs::perms::owner_all);
        check("an uninstall that can't remove the shim fails",
              status == 500 && fs::is_regular_file(kept.installed.binary) &&
                  fs::exists(shim_path(locked, "pi")));

        fs::permissions(locked.bin_dir, fs::perms::none);
        const int hidden = error_status([&] { uninstall_agent(pi, locked, warnings_locked); });
        fs::permissions(locked.bin_dir, fs::perms::owner_all);
        check("an uninstall that can't inspect the shim fails and keeps pi",
              hidden == 500 && fs::is_regular_file(kept.installed.binary) &&
                  fs::exists(shim_path(locked, "pi")));

        std::error_code perm_ec;
        fs::permissions(locked.agent_dir("pi"), fs::perms::owner_exec, perm_ec);
        const int unlisted = error_status([&] { uninstall_agent(pi, locked, warnings_locked); });
        fs::permissions(locked.agent_dir("pi"), fs::perms::owner_all, perm_ec);
        check("an uninstall that can't list the versions fails and keeps the shim",
              unlisted == 500 && fs::exists(shim_path(locked, "pi")) &&
                  fs::is_regular_file(kept.installed.binary));
    }
    if (geteuid() != 0) {
        // The shim can't be rewritten, so it keeps running 1.6.0 from a path
        // with a quote in it: that payload must stay.
        AgentPaths stuck = paths;
        stuck.data_root = work / "o'neil" / "agents";
        stuck.bin_dir = work / "stuck" / "bin";
        install_agent(pi, pin_for("v1.6.0"), stuck, false, nullptr, no_cancel, loopback);
        fs::permissions(stuck.bin_dir, fs::perms::owner_read | fs::perms::owner_exec);
        const auto blocked =
            install_agent(pi, pin_for("v1.5.0"), stuck, false, nullptr, no_cancel, loopback);
        install_agent(pi, pin_for("v1.7.0"), stuck, false, nullptr, no_cancel, loopback);
        fs::permissions(stuck.bin_dir, fs::perms::owner_all);
        check("a shim that couldn't be rewritten is reported", !blocked.warnings.empty());
        check("the payload the shim still runs is never removed",
              fs::exists(stuck.agent_dir("pi") / "1.6.0" / "pi"));
        check("a version neither running nor in the shim is removed",
              !fs::exists(stuck.agent_dir("pi") / "1.5.0"));
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
