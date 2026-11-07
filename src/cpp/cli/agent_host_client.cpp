#include "lemon_cli/agent_host_client.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "lemon/agents/agent_lock.h"
#include "lemon/agents/agent_paths.h"
#include "lemon/agents/host_protocol.h"
#include "lemon/utils/path_utils.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace lemon_cli {

namespace {

constexpr auto kHostStartWait = std::chrono::seconds(15);

std::string read_token(const lemon::agents::AgentPaths& paths) {
    std::ifstream in(paths.token_file());
    std::string token;
    std::getline(in, token);
    return lemon::agents::protocol::is_token(token) ? token : "";
}

std::string host_serving(const lemon::agents::AgentPaths& paths) {
    std::ifstream in(paths.host_info());
    const json info = json::parse(in, nullptr, false);
    return info.is_object() ? info.value("lemond", "") : "";
}

AgentRouteError route_error(const lemonade::HttpError& e) {
    json body = json::parse(e.response_body(), nullptr, false);
    if (!body.is_object()) {
        body = json::object();
    }
    return AgentRouteError(e.status_code(), body.value("code", ""),
                           lemonade::extract_server_error_message(e), body);
}

// True when a host answered with this token.
bool host_answers(const lemonade::LemonadeClient& client, const std::string& token) {
    if (token.empty()) {
        return false;
    }
    try {
        agent_request(client, token, "GET", "/api/v1/agents");
        return true;
    } catch (const AgentRouteError& e) {
        if (e.status == 503 || e.status == 400 || e.status == 403) {
            return false;
        }
        throw;
    }
}

} // namespace

std::optional<std::string> loopback_target(const std::string& host) {
    return lemon::agents::protocol::loopback_host(host);
}

bool spawn_detached(const std::string& exe, const std::vector<std::string>& args) {
#ifdef _WIN32
    std::wstring command = L"\"" + lemon::utils::path_from_utf8(exe).wstring() + L"\"";
    for (const auto& arg : args) {
        command += L" \"" + lemon::utils::path_from_utf8(arg).wstring() + L"\"";
    }
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB;
    BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, flags, nullptr,
                             nullptr, &startup, &process);
    if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
        // A job that forbids breakaway still lets the host run inside it.
        flags &= ~CREATE_BREAKAWAY_FROM_JOB;
        ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, flags, nullptr,
                            nullptr, &startup, &process);
    }
    if (!ok) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#else
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    for (int fd = 0; fd <= 2; ++fd) {
        posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd == 0 ? O_RDONLY : O_WRONLY,
                                         0);
    }
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
#else
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
#endif
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, exe.c_str(), &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    return rc == 0;
#endif
}

std::string ensure_agent_host(const lemonade::LemonadeClient& client,
                              const AgentHostTarget& target) {
    const auto loopback = target.is_ssl ? std::nullopt : loopback_target(target.host);
    if (!loopback) {
        throw std::runtime_error("Agents need a Lemonade server on this machine; " +
                                 target.host + " isn't one.");
    }
    const auto paths = lemon::agents::agent_paths();
    std::string token = read_token(paths);
    if (host_answers(client, token)) {
        return token;
    }

    const std::string url = lemon::agents::protocol::loopback_url(*loopback, target.port);
    // A running host may be reconnecting; one serving another lemond can't help.
    const bool host_running = lemon::agents::FileLock::is_held(paths.host_lock());
    if (host_running) {
        const std::string serving = host_serving(paths);
        if (!serving.empty() && serving != url) {
            throw std::runtime_error("The agent host is serving the Lemonade server at " + serving +
                                     ", not " + url + ". Stop it or use that server.");
        }
    } else {
#ifdef _WIN32
        const fs::path exe = fs::path(lemon::utils::get_executable_dir()) / "lemonade-agent-host.exe";
#else
        const fs::path exe = fs::path(lemon::utils::get_executable_dir()) / "lemonade-agent-host";
#endif
        std::error_code ec;
        if (!fs::exists(exe, ec)) {
            throw std::runtime_error("lemonade-agent-host isn't installed next to lemonade.");
        }
        // The host presents this key to lemond, just as the CLI does.
        if (!target.api_key.empty()) {
            lemon::utils::set_environment_variable_utf8("LEMONADE_API_KEY", target.api_key);
        }
        if (!spawn_detached(lemon::utils::path_to_utf8(exe), {"--lemond", url})) {
            throw std::runtime_error("Can't start lemonade-agent-host.");
        }
    }

    const auto deadline = std::chrono::steady_clock::now() + kHostStartWait;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        token = read_token(paths);
        if (host_answers(client, token)) {
            return token;
        }
    }
    throw std::runtime_error("The agent host didn't connect to Lemonade; see " +
                             lemon::utils::path_to_utf8(paths.log_file()) + ".");
}

json agent_request(const lemonade::LemonadeClient& client, const std::string& token,
                   const std::string& method, const std::string& path, const json& body) {
    try {
        const std::string response = client.make_request(
            path, method, body.is_null() ? "" : body.dump(),
            body.is_null() ? "" : "application/json", 3000, 120000,
            {{lemon::agents::protocol::kTokenHeader, token}});
        json parsed = json::parse(response, nullptr, false);
        if (!parsed.is_object()) {
            throw std::runtime_error("Unexpected response from the agent host.");
        }
        return parsed;
    } catch (const lemonade::HttpError& e) {
        throw route_error(e);
    }
}

json agent_stream(const lemonade::LemonadeClient& client, const std::string& token,
                  const std::string& path, const json& body,
                  const std::function<void(const std::string&, const json&)>& on_event) {
    json complete;
    std::optional<json> error;
    try {
        constexpr time_t kDay = 86400000;
        client.make_request(
            path, "POST", body.dump(), "application/json",
            [&](const std::string& event, const std::string& data) {
                json parsed = json::parse(data, nullptr, false);
                if (event == "complete") {
                    complete = parsed;
                } else if (event == "error") {
                    error = parsed.is_object() ? parsed : json::object();
                } else {
                    on_event(event, parsed);
                }
            },
            3000, kDay, nullptr, {{lemon::agents::protocol::kTokenHeader, token}});
    } catch (const lemonade::HttpError& e) {
        throw route_error(e);
    }
    if (error) {
        throw AgentRouteError(error->value("status", 500), error->value("code", ""),
                              error->value("error", "The agent host reported an error."), *error);
    }
    if (complete.is_null()) {
        throw std::runtime_error("The agent host closed the stream without a result.");
    }
    return complete;
}

} // namespace lemon_cli
