// lemonade-agent-host: installs, configures and launches agents for one OS
// user. It dials lemond and serves the /v1/agents routes lemond relays to it.

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <system_error>
#include <thread>

#include <nlohmann/json.hpp>

#include "lemon/agents/agent_lock.h"
#include "lemon/agents/host_link.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/path_utils.h"
#include "lemon/version.h"

#ifdef _WIN32
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using lemon::utils::get_environment_variable_utf8;
using namespace lemon::agents;

namespace {

std::string new_token() {
    std::random_device random;
    static const char* hex = "0123456789abcdef";
    std::string token;
    for (int i = 0; i < 32; ++i) {
        const unsigned value = random() & 0xff;
        token.push_back(hex[value >> 4]);
        token.push_back(hex[value & 0xf]);
    }
    return token;
}

// Written to a fresh file that only the user can read, then renamed over the old one.
bool write_private_file(const fs::path& path, const std::string& content) {
    const fs::path tmp = path.string() + ".tmp";
    std::error_code ec;
    fs::remove(tmp, ec);
#ifdef _WIN32
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << content;
        if (!out) {
            return false;
        }
    }
#else
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    const bool written = write(fd, content.data(), content.size()) ==
                         static_cast<ssize_t>(content.size());
    close(fd);
    if (!written) {
        fs::remove(tmp, ec);
        return false;
    }
#endif
    fs::rename(tmp, path, ec);
    return !ec;
}

// The CLI passes the server it targets; anything else keeps the old defaults.
std::string loopback_host(const std::string& host) {
    if (host.empty() || host == "0.0.0.0" || host == "localhost") {
        return "127.0.0.1";
    }
    if (host == "::1" || host == "[::1]" || host.rfind("127.", 0) == 0) {
        return host;
    }
    return "";
}

int usage() {
    std::cerr << "usage: lemonade-agent-host [--lemond http://127.0.0.1:PORT] [--persistent]"
              << std::endl;
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    std::string lemond;
    bool persistent = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--lemond" && i + 1 < argc) {
            lemond = argv[++i];
        } else if (arg == "--persistent") {
            persistent = true;
        } else if (arg == "--version") {
            std::cout << LEMON_VERSION_STRING << std::endl;
            return 0;
        } else {
            return usage();
        }
    }
    if (lemond.empty()) {
        const std::string port = get_environment_variable_utf8("LEMONADE_PORT");
        lemond = "http://" + loopback_host(get_environment_variable_utf8("LEMONADE_HOST")) + ":" +
                 (port.empty() ? "13305" : port);
    }
    // Phase 1 serves only a lemond on this machine.
    const std::string prefix = "http://";
    const auto host_end = lemond.rfind(':');
    if (lemond.rfind(prefix, 0) != 0 || host_end == std::string::npos ||
        host_end < prefix.size() ||
        loopback_host(lemond.substr(prefix.size(), host_end - prefix.size())).empty()) {
        std::cerr << "lemonade-agent-host only serves a lemond on this machine (got " << lemond
                  << ")." << std::endl;
        return 2;
    }

    HostContext ctx;
    ctx.paths = agent_paths();
    std::error_code ec;
    fs::create_directories(ctx.paths.state_dir, ec);
    fs::permissions(ctx.paths.state_dir, fs::perms::owner_all, ec);

    AixLog::Log::init({std::make_shared<AixLog::SinkFile>(
        AixLog::Severity::info, lemon::utils::path_to_utf8(ctx.paths.log_file()))});

    auto lock = FileLock::try_acquire(ctx.paths.host_lock());
    std::unique_ptr<FileLock> takeover;
    if (!lock) {
        if (!persistent) {
            return 0;  // another host already serves this user
        }
        // An on-demand host finishes its work and exits once it sees this. It
        // probes the same lock each second, so a busy try is retried.
        takeover = FileLock::try_acquire_for(ctx.paths.takeover_lock(), std::chrono::seconds(3));
        if (!takeover) {
            LOG(INFO, "AgentHost") << "Another persistent host is waiting; exiting" << std::endl;
            return 0;
        }
        LOG(INFO, "AgentHost") << "Waiting for the running host to finish" << std::endl;
        lock = FileLock::acquire(ctx.paths.host_lock());
        takeover.reset();
        if (!lock) {
            return 1;
        }
    }

    ctx.lemond_origin = lemond;
    ctx.api_key = get_environment_variable_utf8("LEMONADE_ADMIN_API_KEY");
    if (ctx.api_key.empty()) {
        ctx.api_key = get_environment_variable_utf8("LEMONADE_API_KEY");
    }
    const std::string release_base = get_environment_variable_utf8("LEMONADE_AGENT_RELEASE_BASE_URL");
    if (!release_base.empty()) {
        // Test override: only a local fixture server is trusted over plain http.
        if (release_base.rfind("http://127.0.0.1:", 0) == 0) {
            ctx.release_base_url = release_base;
            ctx.download_policy = lemon::utils::HttpSecurityPolicy::TrustedLoopback;
        } else {
            LOG(WARNING, "AgentHost") << "Ignoring LEMONADE_AGENT_RELEASE_BASE_URL" << std::endl;
        }
    }

    ctx.token = new_token();
    if (!write_private_file(ctx.paths.token_file(), ctx.token + "\n")) {
        LOG(ERROR, "AgentHost") << "Can't write " << ctx.paths.token_file() << std::endl;
        return 1;
    }
    const nlohmann::json info = {
        {"lemond", lemond}, {"persistent", persistent}, {"version", LEMON_VERSION_STRING},
#ifdef _WIN32
        {"pid", _getpid()},
#else
        {"pid", getpid()},
#endif
    };
    write_private_file(ctx.paths.host_info(), info.dump() + "\n");

    LinkOptions options;
    options.ws_url = "ws://" + lemond.substr(prefix.size()) + "/v1/agents/host";
    options.persistent = persistent;
    const std::string idle = get_environment_variable_utf8("LEMONADE_AGENT_HOST_IDLE_SECONDS");
    if (!idle.empty()) {
        try {
            options.idle_exit = std::chrono::seconds(std::stoi(idle));
        } catch (const std::exception&) {
        }
    }
    LOG(INFO, "AgentHost") << "lemonade-agent-host " << LEMON_VERSION_STRING << " serving "
                           << lemond << (persistent ? " (persistent)" : "") << std::endl;

    HostRoutes routes(ctx);
    const LinkExit reason = run_link(ctx, routes, options);
    static const char* reasons[] = {"idle", "lemond unreachable", "API key rejected", "taken over"};
    LOG(INFO, "AgentHost") << "Exiting: " << reasons[static_cast<int>(reason)] << std::endl;
    // Request threads use routes and ctx, so they finish before either goes.
    while (routes.in_flight() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // A token left behind would point clients at a host that's gone.
    fs::remove(ctx.paths.token_file(), ec);
    fs::remove(ctx.paths.host_info(), ec);
    return reason == LinkExit::Unauthorized ? 3 : 0;
}
