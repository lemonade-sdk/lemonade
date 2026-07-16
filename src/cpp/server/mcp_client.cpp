#include "lemon/mcp_client.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <lemon/utils/aixlog.hpp>

#ifdef _WIN32
    #define NOMINMAX
    #include <windows.h>
#else
    #include <cerrno>
    #include <csignal>
    #include <cstring>
    #include <fcntl.h>
    #include <pthread.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <unistd.h>

extern char** environ;
#endif

namespace fs = std::filesystem;

namespace lemon {
namespace {

constexpr const char* kProtocolVersion = "2025-11-25";
constexpr const char* kProtocolVersion20250618 = "2025-06-18";
constexpr const char* kProtocolVersion20250326 = "2025-03-26";
constexpr const char* kProtocolVersion20241105 = "2024-11-05";
constexpr const char* kConfigFileName = "mcp_servers.json";
constexpr int kDefaultTimeoutMs = 30000;
constexpr int kMinTimeoutMs = 1000;
constexpr int kMaxTimeoutMs = 300000;
constexpr int kMaxToolListPages = 32;
constexpr std::size_t kReadBufferBytes = 8192;
constexpr std::size_t kMaxMessageBytes = 64U * 1024U * 1024U;

json make_error(const std::string& message, int status_code = 400,
                const std::string& type = "mcp_client_error") {
    return json{{"error", json{{"message", message},
                                {"type", type},
                                {"status_code", status_code}}}};
}

void set_json(httplib::Response& res, const json& body, int status = 200) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

void set_error(httplib::Response& res, const std::string& message,
               int status = 400,
               const std::string& type = "mcp_client_error") {
    set_json(res, make_error(message, status, type), status);
}

bool parse_json_body(const httplib::Request& req, httplib::Response& res,
                     json& out, bool required = true) {
    if (req.body.empty()) {
        if (required) {
            set_error(res, "Request body must be a JSON object", 400);
            return false;
        }
        out = json::object();
        return true;
    }
    try {
        out = json::parse(req.body);
    } catch (const std::exception& e) {
        set_error(res, std::string("Invalid JSON body: ") + e.what(), 400);
        return false;
    }
    if (!out.is_object()) {
        set_error(res, "Request body must be a JSON object", 400);
        return false;
    }
    return true;
}

bool ascii_is_alpha(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool ascii_is_digit(unsigned char c) {
    return c >= '0' && c <= '9';
}

bool ascii_is_alnum(unsigned char c) {
    return ascii_is_alpha(c) || ascii_is_digit(c);
}

char ascii_to_lower(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return static_cast<char>(c);
}

bool has_control_char(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
    });
}

std::string trim(std::string value) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                value.end());
    return value;
}

bool valid_id(const std::string& id) {
    if (id.empty() || id.size() > 96) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return ascii_is_alnum(c) || c == '-' || c == '_' || c == '.';
    });
}

std::string sanitize_id_seed(const std::string& seed) {
    std::string out;
    out.reserve(seed.size());
    bool last_dash = false;
    for (unsigned char c : seed) {
        const bool ok = ascii_is_alnum(c) || c == '_' || c == '.';
        if (ok) {
            out.push_back(ascii_to_lower(c));
            last_dash = false;
        } else if (!last_dash) {
            out.push_back('-');
            last_dash = true;
        }
    }
    while (!out.empty() && out.front() == '-') out.erase(out.begin());
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.empty()) out = "mcp-server";
    if (out.size() > 64) out.resize(64);
    return out;
}

std::string basename_like(const std::string& command) {
    const std::size_t pos = command.find_last_of("/\\");
    if (pos == std::string::npos) return command;
    return command.substr(pos + 1);
}

bool valid_env_name(const std::string& name) {
    if (name.empty()) return false;
    const unsigned char first = static_cast<unsigned char>(name.front());
    if (!(ascii_is_alpha(first) || first == '_')) return false;
    return std::all_of(name.begin() + 1, name.end(), [](unsigned char c) {
        return ascii_is_alnum(c) || c == '_';
    });
}

std::optional<std::string> env_reference_name(const std::string& value) {
    if (value.size() < 4 || value.rfind("${", 0) != 0 ||
        value.back() != '}') {
        return std::nullopt;
    }
    std::string name = value.substr(2, value.size() - 3);
    if (!valid_env_name(name)) return std::nullopt;
    return name;
}

void validate_env_references(const McpServerConfig& config) {
    for (const auto& [key, value] : config.env) {
        if (!env_reference_name(value)) {
            throw std::runtime_error(
                "MCP env value for '" + key +
                "' must be an environment reference such as ${" + key +
                "}; raw values are not persisted");
        }
    }
}

McpServerConfig resolve_env_references(McpServerConfig config) {
    for (auto& [key, value] : config.env) {
        const auto ref = env_reference_name(value);
        if (!ref) {
            throw std::runtime_error(
                "MCP env value for '" + key +
                "' is not a valid ${VARIABLE} reference");
        }
        const char* env_value = std::getenv(ref->c_str());
        if (!env_value) {
            throw std::runtime_error(
                "Required MCP environment variable is not set: " + *ref);
        }
        value = env_value;
    }
    return config;
}

json config_to_persisted_json(const McpServerConfig& config) {
    validate_env_references(config);
    return McpClientManager::config_to_json(config, true);
}

int clamp_timeout_ms(int timeout_ms) {
    return std::max(kMinTimeoutMs, std::min(kMaxTimeoutMs, timeout_ms));
}

bool supported_protocol_version(const std::string& version) {
    return version == kProtocolVersion ||
           version == kProtocolVersion20250618 ||
           version == kProtocolVersion20250326 ||
           version == kProtocolVersion20241105;
}

std::string fnv1a_hex16(const std::string& value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << hash;
    return oss.str();
}

json json_rpc_request(std::int64_t id, const std::string& method,
                      json params = json::object()) {
    json msg{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
    if (!params.is_null()) msg["params"] = std::move(params);
    return msg;
}

json json_rpc_notification(const std::string& method,
                           json params = json::object()) {
    json msg{{"jsonrpc", "2.0"}, {"method", method}};
    if (!params.is_null()) msg["params"] = std::move(params);
    return msg;
}

json json_rpc_method_not_found(const json& id, const std::string& method) {
    return json{{"jsonrpc", "2.0"},
                {"id", id},
                {"error", json{{"code", -32601},
                                {"message",
                                 "Unsupported MCP client request: " + method}}}};
}

std::string json_rpc_error_message(const json& response) {
    if (!response.contains("error")) return "";
    const auto& err = response["error"];
    if (err.is_object()) return err.value("message", err.dump());
    if (err.is_string()) return err.get<std::string>();
    return err.dump();
}

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) return std::wstring();
    const int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        value.data(),
                                        static_cast<int>(value.size()), nullptr, 0);
    if (len <= 0) {
        throw std::runtime_error("Failed to convert UTF-8 to UTF-16");
    }
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), out.data(), len) <= 0) {
        throw std::runtime_error("Failed to convert UTF-8 to UTF-16");
    }
    return out;
}

std::wstring quote_windows_arg(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";
    const bool needs_quotes =
        arg.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needs_quotes) return arg;

    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'\"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(ch);
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            backslashes = 0;
            out.push_back(ch);
        }
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'\"');
    return out;
}

std::wstring build_windows_command_line(
    const std::string& command, const std::vector<std::string>& args) {
    std::wstring cmd = quote_windows_arg(utf8_to_wide(command));
    for (const auto& arg : args) {
        cmd.push_back(L' ');
        cmd += quote_windows_arg(utf8_to_wide(arg));
    }
    return cmd;
}

struct CaseInsensitiveWideLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};

std::vector<wchar_t> build_windows_environment_block(
    const std::map<std::string, std::string>& overrides) {
    std::vector<std::wstring> hidden_entries;
    std::map<std::wstring, std::wstring, CaseInsensitiveWideLess> entries;

    LPWCH raw = GetEnvironmentStringsW();
    if (!raw) {
        throw std::runtime_error("GetEnvironmentStringsW failed");
    }
    for (LPWCH p = raw; *p;) {
        std::wstring entry(p);
        p += entry.size() + 1;
        if (!entry.empty() && entry.front() == L'=') {
            hidden_entries.push_back(entry);
            continue;
        }
        const std::size_t eq = entry.find(L'=');
        if (eq != std::wstring::npos) {
            entries[entry.substr(0, eq)] = entry;
        }
    }
    FreeEnvironmentStringsW(raw);

    for (const auto& [key, value] : overrides) {
        std::wstring wkey = utf8_to_wide(key);
        entries[wkey] = wkey + L"=" + utf8_to_wide(value);
    }

    std::vector<wchar_t> block;
    for (const auto& entry : hidden_entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    for (const auto& [_, entry] : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}
#else
std::map<std::string, std::string> current_environment() {
    std::map<std::string, std::string> env;
    if (!environ) return env;
    for (char** entry = environ; *entry; ++entry) {
        const std::string value(*entry);
        const std::size_t eq = value.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        env[value.substr(0, eq)] = value.substr(eq + 1);
    }
    return env;
}

std::string resolve_executable(
    const McpServerConfig& config,
    const std::map<std::string, std::string>& environment) {
    if (config.command.find('/') != std::string::npos) {
        return config.command;
    }

    auto path_it = environment.find("PATH");
    const std::string path_value =
        path_it == environment.end() ? "/usr/local/bin:/usr/bin:/bin"
                                     : path_it->second;

    std::size_t begin = 0;
    while (begin <= path_value.size()) {
        const std::size_t end = path_value.find(':', begin);
        std::string directory = path_value.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (directory.empty()) directory = ".";

        fs::path candidate(directory);
        if (candidate.is_relative() && !config.working_dir.empty()) {
            candidate = fs::path(config.working_dir) / candidate;
        }
        candidate /= config.command;

        std::error_code ec;
        fs::path absolute = fs::absolute(candidate, ec);
        const std::string candidate_string =
            ec ? candidate.string() : absolute.string();
        if (::access(candidate_string.c_str(), X_OK) == 0) {
            return candidate_string;
        }

        if (end == std::string::npos) break;
        begin = end + 1;
    }

    throw std::runtime_error("MCP command was not found on PATH: " +
                             config.command);
}

ssize_t write_without_sigpipe(int fd, const void* data, std::size_t size) {
    sigset_t blocked;
    sigset_t previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);

    const int mask_result = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    if (mask_result != 0) {
        errno = mask_result;
        return -1;
    }

    sigset_t pending;
    bool already_pending = false;
    if (sigpending(&pending) == 0) {
        already_pending = sigismember(&pending, SIGPIPE) == 1;
    }

    ssize_t result;
    do {
        result = ::write(fd, data, size);
    } while (result < 0 && errno == EINTR);
    const int saved_errno = errno;

    if (result < 0 && saved_errno == EPIPE && !already_pending) {
        sigset_t pending_after;
        if (sigpending(&pending_after) == 0 &&
            sigismember(&pending_after, SIGPIPE) == 1) {
            int consumed_signal = 0;
            (void)sigwait(&blocked, &consumed_signal);
        }
    }

    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    errno = saved_errno;
    return result;
}
#endif

class StdioProcess {
public:
    using LineCallback = std::function<void(const std::string&)>;
    using ExitCallback = std::function<void()>;

    StdioProcess() = default;
    ~StdioProcess() { stop(); }

    StdioProcess(const StdioProcess&) = delete;
    StdioProcess& operator=(const StdioProcess&) = delete;

    void start(const McpServerConfig& config, LineCallback on_stdout_line,
               LineCallback on_stderr_line, ExitCallback on_exit) {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        stop_locked();

        on_stdout_line_ = std::move(on_stdout_line);
        on_stderr_line_ = std::move(on_stderr_line);
        {
            std::lock_guard<std::mutex> callback_lock(callback_mutex_);
            on_exit_ = std::move(on_exit);
        }
        exit_notified_.store(false, std::memory_order_release);

        const McpServerConfig process_config = resolve_env_references(config);
        if (!process_config.working_dir.empty()) {
            std::error_code ec;
            if (!fs::is_directory(fs::path(process_config.working_dir), ec) ||
                ec) {
                throw std::runtime_error(
                    "MCP working_dir is not a readable directory: " +
                    process_config.working_dir);
            }
        }

#ifdef _WIN32
        start_windows(process_config);
#else
        start_posix(process_config);
#endif

        running_.store(true, std::memory_order_release);
        try {
            stdout_thread_ = std::thread([this] { read_loop_stdout(); });
            stderr_thread_ = std::thread([this] { read_loop_stderr(); });
        } catch (...) {
            running_.store(false, std::memory_order_release);
            stop_locked();
            throw;
        }
    }

    bool write_line(const std::string& line) {
        bool failed = false;
        {
            std::lock_guard<std::mutex> write_lock(write_mutex_);
            if (!running_.load(std::memory_order_acquire)) return false;

            std::string payload = line;
            payload.push_back('\n');
#ifdef _WIN32
            const char* data = payload.data();
            std::size_t remaining = payload.size();
            while (remaining > 0) {
                const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
                    remaining, std::numeric_limits<DWORD>::max()));
                DWORD written = 0;
                if (!WriteFile(stdin_write_, data, chunk, &written, nullptr) ||
                    written == 0) {
                    failed = true;
                    break;
                }
                data += written;
                remaining -= written;
            }
#else
            const char* data = payload.data();
            std::size_t remaining = payload.size();
            while (remaining > 0) {
                const ssize_t written =
                    write_without_sigpipe(stdin_write_, data, remaining);
                if (written <= 0) {
                    failed = true;
                    break;
                }
                data += written;
                remaining -= static_cast<std::size_t>(written);
            }
#endif
            if (failed) {
                running_.store(false, std::memory_order_release);
            }
        }

        if (failed) notify_exit_once();
        return !failed;
    }

    void stop() {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        stop_locked();
    }

private:
    void stop_locked() {
        if (!started_) return;
        running_.store(false, std::memory_order_release);

        {
            std::lock_guard<std::mutex> write_lock(write_mutex_);
#ifdef _WIN32
            if (stdin_write_ != INVALID_HANDLE_VALUE) {
                CloseHandle(stdin_write_);
                stdin_write_ = INVALID_HANDLE_VALUE;
            }
#else
            if (stdin_write_ >= 0) {
                ::close(stdin_write_);
                stdin_write_ = -1;
            }
#endif
        }

#ifdef _WIN32
        if (process_info_.hProcess) {
            DWORD wait = WaitForSingleObject(process_info_.hProcess, 1000);
            if (wait == WAIT_TIMEOUT) {
                if (job_ != nullptr) {
                    TerminateJobObject(job_, 1);
                } else {
                    TerminateProcess(process_info_.hProcess, 1);
                }
                WaitForSingleObject(process_info_.hProcess, 1000);
            } else if (job_ != nullptr) {
                // The main process may exit while descendants still hold the pipe
                // handles. Terminating the job prevents reader-thread shutdown
                // from hanging on inherited handles.
                TerminateJobObject(job_, 0);
            }
        }
#else
        if (pid_ > 0) {
            int status = 0;
            bool reaped = false;
            auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::seconds(1);
            while (std::chrono::steady_clock::now() < deadline) {
                const pid_t result = ::waitpid(pid_, &status, WNOHANG);
                if (result == pid_ || (result < 0 && errno == ECHILD)) {
                    reaped = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            if (!reaped) {
                ::kill(-pid_, SIGTERM);
                deadline = std::chrono::steady_clock::now() +
                           std::chrono::seconds(1);
                while (std::chrono::steady_clock::now() < deadline) {
                    const pid_t result = ::waitpid(pid_, &status, WNOHANG);
                    if (result == pid_ ||
                        (result < 0 && errno == ECHILD)) {
                        reaped = true;
                        break;
                    }
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(25));
                }
            }
            if (!reaped) {
                ::kill(-pid_, SIGKILL);
                while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
                }
            } else {
                // Clean up descendants in the process group even when the direct
                // child exited after stdin was closed.
                ::kill(-pid_, SIGTERM);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                ::kill(-pid_, SIGKILL);
            }
        }
#endif

        if (stdout_thread_.joinable()) stdout_thread_.join();
        if (stderr_thread_.joinable()) stderr_thread_.join();

#ifdef _WIN32
        if (process_info_.hThread) {
            CloseHandle(process_info_.hThread);
            process_info_.hThread = nullptr;
        }
        if (process_info_.hProcess) {
            CloseHandle(process_info_.hProcess);
            process_info_.hProcess = nullptr;
        }
        if (job_ != nullptr) {
            CloseHandle(job_);
            job_ = nullptr;
        }
        if (stdout_read_ != INVALID_HANDLE_VALUE) {
            CloseHandle(stdout_read_);
            stdout_read_ = INVALID_HANDLE_VALUE;
        }
        if (stderr_read_ != INVALID_HANDLE_VALUE) {
            CloseHandle(stderr_read_);
            stderr_read_ = INVALID_HANDLE_VALUE;
        }
#else
        if (stdout_read_ >= 0) {
            ::close(stdout_read_);
            stdout_read_ = -1;
        }
        if (stderr_read_ >= 0) {
            ::close(stderr_read_);
            stderr_read_ = -1;
        }
        pid_ = -1;
#endif

        started_ = false;
        on_stdout_line_ = nullptr;
        on_stderr_line_ = nullptr;
        {
            std::lock_guard<std::mutex> callback_lock(callback_mutex_);
            on_exit_ = nullptr;
        }
    }

#ifdef _WIN32
    void start_windows(const McpServerConfig& config) {
        std::wstring command_line =
            build_windows_command_line(config.command, config.args);
        std::vector<wchar_t> mutable_command_line(command_line.begin(),
                                                   command_line.end());
        mutable_command_line.push_back(L'\0');
        const std::wstring cwd = config.working_dir.empty()
                                     ? std::wstring()
                                     : utf8_to_wide(config.working_dir);
        std::vector<wchar_t> environment =
            build_windows_environment_block(config.env);

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE stdin_read = INVALID_HANDLE_VALUE;
        HANDLE stdout_write = INVALID_HANDLE_VALUE;
        HANDLE stderr_write = INVALID_HANDLE_VALUE;

        auto close_if_valid = [](HANDLE handle) {
            if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
                CloseHandle(handle);
            }
        };

        if (!CreatePipe(&stdin_read, &stdin_write_, &sa, 0) ||
            !CreatePipe(&stdout_read_, &stdout_write, &sa, 0) ||
            !CreatePipe(&stderr_read_, &stderr_write, &sa, 0)) {
            const DWORD error = GetLastError();
            close_if_valid(stdin_read);
            close_if_valid(stdin_write_);
            close_if_valid(stdout_read_);
            close_if_valid(stdout_write);
            close_if_valid(stderr_read_);
            close_if_valid(stderr_write);
            stdin_write_ = INVALID_HANDLE_VALUE;
            stdout_read_ = INVALID_HANDLE_VALUE;
            stderr_read_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error(
                "CreatePipe failed for MCP server (GetLastError=" +
                std::to_string(error) + ")");
        }

        if (!SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(stderr_read_, HANDLE_FLAG_INHERIT, 0)) {
            const DWORD error = GetLastError();
            close_if_valid(stdin_read);
            close_if_valid(stdin_write_);
            close_if_valid(stdout_read_);
            close_if_valid(stdout_write);
            close_if_valid(stderr_read_);
            close_if_valid(stderr_write);
            stdin_write_ = INVALID_HANDLE_VALUE;
            stdout_read_ = INVALID_HANDLE_VALUE;
            stderr_read_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error(
                "SetHandleInformation failed for MCP server (GetLastError=" +
                std::to_string(error) + ")");
        }

        struct AttributeListGuard {
            LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
            ~AttributeListGuard() {
                if (list) DeleteProcThreadAttributeList(list);
            }
        } attribute_guard;

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = stdin_read;
        startup.StartupInfo.hStdOutput = stdout_write;
        startup.StartupInfo.hStdError = stderr_write;

        SIZE_T attribute_bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0,
                                          &attribute_bytes);
        if (attribute_bytes == 0) {
            const DWORD error = GetLastError();
            close_if_valid(stdin_read);
            close_if_valid(stdin_write_);
            close_if_valid(stdout_read_);
            close_if_valid(stdout_write);
            close_if_valid(stderr_read_);
            close_if_valid(stderr_write);
            stdin_write_ = INVALID_HANDLE_VALUE;
            stdout_read_ = INVALID_HANDLE_VALUE;
            stderr_read_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error(
                "Failed to size Windows process attribute list (GetLastError=" +
                std::to_string(error) + ")");
        }
        std::vector<unsigned char> attribute_storage(attribute_bytes);
        startup.lpAttributeList =
            reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
                attribute_storage.data());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0,
                                               &attribute_bytes)) {
            const DWORD error = GetLastError();
            close_if_valid(stdin_read);
            close_if_valid(stdin_write_);
            close_if_valid(stdout_read_);
            close_if_valid(stdout_write);
            close_if_valid(stderr_read_);
            close_if_valid(stderr_write);
            stdin_write_ = INVALID_HANDLE_VALUE;
            stdout_read_ = INVALID_HANDLE_VALUE;
            stderr_read_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error(
                "InitializeProcThreadAttributeList failed (GetLastError=" +
                std::to_string(error) + ")");
        }
        attribute_guard.list = startup.lpAttributeList;
        HANDLE inherited_handles[] = {stdin_read, stdout_write, stderr_write};
        if (!UpdateProcThreadAttribute(
                startup.lpAttributeList, 0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited_handles,
                sizeof(inherited_handles), nullptr, nullptr)) {
            const DWORD error = GetLastError();
            close_if_valid(stdin_read);
            close_if_valid(stdin_write_);
            close_if_valid(stdout_read_);
            close_if_valid(stdout_write);
            close_if_valid(stderr_read_);
            close_if_valid(stderr_write);
            stdin_write_ = INVALID_HANDLE_VALUE;
            stdout_read_ = INVALID_HANDLE_VALUE;
            stderr_read_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error(
                "UpdateProcThreadAttribute failed (GetLastError=" +
                std::to_string(error) + ")");
        }

        job_ = CreateJobObjectW(nullptr, nullptr);
        if (!job_) {
            const DWORD error = GetLastError();
            close_if_valid(stdin_read);
            close_if_valid(stdin_write_);
            close_if_valid(stdout_read_);
            close_if_valid(stdout_write);
            close_if_valid(stderr_read_);
            close_if_valid(stderr_write);
            stdin_write_ = INVALID_HANDLE_VALUE;
            stdout_read_ = INVALID_HANDLE_VALUE;
            stderr_read_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error(
                "CreateJobObjectW failed for MCP server (GetLastError=" +
                std::to_string(error) + ")");
        }

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info{};
        job_info.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation,
                                     &job_info, sizeof(job_info))) {
            const DWORD error = GetLastError();
            CloseHandle(job_);
            job_ = nullptr;
            close_if_valid(stdin_read);
            close_if_valid(stdin_write_);
            close_if_valid(stdout_read_);
            close_if_valid(stdout_write);
            close_if_valid(stderr_read_);
            close_if_valid(stderr_write);
            stdin_write_ = INVALID_HANDLE_VALUE;
            stdout_read_ = INVALID_HANDLE_VALUE;
            stderr_read_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error(
                "SetInformationJobObject failed for MCP server (GetLastError=" +
                std::to_string(error) + ")");
        }

        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(
            nullptr, mutable_command_line.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED |
                EXTENDED_STARTUPINFO_PRESENT,
            environment.data(), cwd.empty() ? nullptr : cwd.c_str(),
            &startup.StartupInfo, &process);

        close_if_valid(stdin_read);
        close_if_valid(stdout_write);
        close_if_valid(stderr_write);

        if (!created) {
            const DWORD error = GetLastError();
            CloseHandle(job_);
            job_ = nullptr;
            close_if_valid(stdin_write_);
            close_if_valid(stdout_read_);
            close_if_valid(stderr_read_);
            stdin_write_ = INVALID_HANDLE_VALUE;
            stdout_read_ = INVALID_HANDLE_VALUE;
            stderr_read_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error(
                "CreateProcessW failed for MCP server '" + config.command +
                "' (GetLastError=" + std::to_string(error) + ")");
        }

        if (!AssignProcessToJobObject(job_, process.hProcess)) {
            const DWORD error = GetLastError();
            TerminateProcess(process.hProcess, 1);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            CloseHandle(job_);
            job_ = nullptr;
            close_if_valid(stdin_write_);
            close_if_valid(stdout_read_);
            close_if_valid(stderr_read_);
            stdin_write_ = INVALID_HANDLE_VALUE;
            stdout_read_ = INVALID_HANDLE_VALUE;
            stderr_read_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error(
                "AssignProcessToJobObject failed for MCP server (GetLastError=" +
                std::to_string(error) + ")");
        }

        if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
            const DWORD error = GetLastError();
            TerminateJobObject(job_, 1);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            CloseHandle(job_);
            job_ = nullptr;
            close_if_valid(stdin_write_);
            close_if_valid(stdout_read_);
            close_if_valid(stderr_read_);
            stdin_write_ = INVALID_HANDLE_VALUE;
            stdout_read_ = INVALID_HANDLE_VALUE;
            stderr_read_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error(
                "ResumeThread failed for MCP server (GetLastError=" +
                std::to_string(error) + ")");
        }

        process_info_ = process;
        started_ = true;
    }
#else
    static bool set_close_on_exec(int fd) {
        const int flags = ::fcntl(fd, F_GETFD);
        return flags >= 0 && ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
    }

    static int create_cloexec_pipe(int fds[2]) {
#ifdef __linux__
        return ::pipe2(fds, O_CLOEXEC);
#else
        if (::pipe(fds) != 0) return -1;
        if (set_close_on_exec(fds[0]) && set_close_on_exec(fds[1])) {
            return 0;
        }
        const int saved_errno = errno;
        ::close(fds[0]);
        ::close(fds[1]);
        fds[0] = fds[1] = -1;
        errno = saved_errno;
        return -1;
#endif
    }

    void start_posix(const McpServerConfig& config) {
        std::map<std::string, std::string> environment = current_environment();
        for (const auto& [key, value] : config.env) environment[key] = value;
        const std::string executable = resolve_executable(config, environment);

        std::vector<std::string> argv_strings;
        argv_strings.reserve(config.args.size() + 1);
        argv_strings.push_back(config.command);
        argv_strings.insert(argv_strings.end(), config.args.begin(),
                            config.args.end());
        std::vector<char*> argv;
        argv.reserve(argv_strings.size() + 1);
        for (auto& value : argv_strings) argv.push_back(value.data());
        argv.push_back(nullptr);

        std::vector<std::string> env_strings;
        env_strings.reserve(environment.size());
        for (const auto& [key, value] : environment) {
            env_strings.push_back(key + "=" + value);
        }
        std::vector<char*> envp;
        envp.reserve(env_strings.size() + 1);
        for (auto& value : env_strings) envp.push_back(value.data());
        envp.push_back(nullptr);

        int stdin_pipe[2] = {-1, -1};
        int stdout_pipe[2] = {-1, -1};
        int stderr_pipe[2] = {-1, -1};
        int exec_error_pipe[2] = {-1, -1};
        auto close_pair = [](int pair[2]) {
            if (pair[0] >= 0) ::close(pair[0]);
            if (pair[1] >= 0) ::close(pair[1]);
            pair[0] = pair[1] = -1;
        };

        if (create_cloexec_pipe(stdin_pipe) != 0 ||
            create_cloexec_pipe(stdout_pipe) != 0 ||
            create_cloexec_pipe(stderr_pipe) != 0 ||
            create_cloexec_pipe(exec_error_pipe) != 0) {
            const int error = errno;
            close_pair(stdin_pipe);
            close_pair(stdout_pipe);
            close_pair(stderr_pipe);
            close_pair(exec_error_pipe);
            throw std::runtime_error(
                std::string("pipe creation failed: ") +
                std::strerror(error));
        }

        const pid_t child = ::fork();
        if (child < 0) {
            const int error = errno;
            close_pair(stdin_pipe);
            close_pair(stdout_pipe);
            close_pair(stderr_pipe);
            close_pair(exec_error_pipe);
            throw std::runtime_error(std::string("fork failed: ") +
                                     std::strerror(error));
        }

        if (child == 0) {
            ::close(stdin_pipe[1]);
            ::close(stdout_pipe[0]);
            ::close(stderr_pipe[0]);
            ::close(exec_error_pipe[0]);

            auto child_fail = [&](int error) {
                const int saved = error;
                while (::write(exec_error_pipe[1], &saved, sizeof(saved)) < 0 &&
                       errno == EINTR) {
                }
                _exit(127);
            };

            if (::setpgid(0, 0) != 0) child_fail(errno);
            if (!config.working_dir.empty() &&
                ::chdir(config.working_dir.c_str()) != 0) {
                child_fail(errno);
            }
            if (::dup2(stdin_pipe[0], STDIN_FILENO) < 0 ||
                ::dup2(stdout_pipe[1], STDOUT_FILENO) < 0 ||
                ::dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
                child_fail(errno);
            }

            ::close(stdin_pipe[0]);
            ::close(stdout_pipe[1]);
            ::close(stderr_pipe[1]);
            ::execve(executable.c_str(), argv.data(), envp.data());
            child_fail(errno);
        }

        ::setpgid(child, child);
        ::close(stdin_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[1]);
        ::close(exec_error_pipe[1]);

        int exec_error = 0;
        ssize_t bytes = 0;
        do {
            bytes = ::read(exec_error_pipe[0], &exec_error,
                           sizeof(exec_error));
        } while (bytes < 0 && errno == EINTR);
        ::close(exec_error_pipe[0]);

        if (bytes > 0) {
            int status = 0;
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
            ::close(stdin_pipe[1]);
            ::close(stdout_pipe[0]);
            ::close(stderr_pipe[0]);
            throw std::runtime_error(
                "Failed to launch MCP server '" + config.command + "': " +
                std::strerror(exec_error));
        }
        if (bytes < 0) {
            const int error = errno;
            ::kill(-child, SIGKILL);
            int status = 0;
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
            ::close(stdin_pipe[1]);
            ::close(stdout_pipe[0]);
            ::close(stderr_pipe[0]);
            throw std::runtime_error(
                std::string("Failed to verify MCP exec: ") +
                std::strerror(error));
        }

        pid_ = child;
        stdin_write_ = stdin_pipe[1];
        stdout_read_ = stdout_pipe[0];
        stderr_read_ = stderr_pipe[0];
        started_ = true;
    }
#endif

    bool consume_bytes(std::string& line, const char* data, std::size_t count,
                       const LineCallback& callback) {
        for (std::size_t i = 0; i < count; ++i) {
            const char ch = data[i];
            if (ch == '\n') {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) {
                    try {
                        callback(line);
                    } catch (const std::exception& e) {
                        if (on_stderr_line_) {
                            on_stderr_line_(
                                std::string("MCP stdout callback failed: ") +
                                e.what());
                        }
                        return false;
                    } catch (...) {
                        if (on_stderr_line_) {
                            on_stderr_line_(
                                "MCP stdout callback failed with unknown error");
                        }
                        return false;
                    }
                }
                line.clear();
            } else {
                if (line.size() >= kMaxMessageBytes) {
                    if (on_stderr_line_) {
                        on_stderr_line_(
                            "MCP message exceeded the 64 MiB safety limit");
                    }
                    return false;
                }
                line.push_back(ch);
            }
        }
        return true;
    }

    void read_loop_stdout() {
        const bool clean = read_loop(stdout_read_handle(), on_stdout_line_);
        const bool was_running =
            running_.exchange(false, std::memory_order_acq_rel);
        if (was_running || !clean) notify_exit_once();
    }

    void read_loop_stderr() {
        read_loop(stderr_read_handle(), on_stderr_line_);
    }

#ifdef _WIN32
    HANDLE stdout_read_handle() const { return stdout_read_; }
    HANDLE stderr_read_handle() const { return stderr_read_; }

    bool read_loop(HANDLE handle, const LineCallback& callback) {
        std::string line;
        std::vector<char> buffer(kReadBufferBytes);
        while (handle != INVALID_HANDLE_VALUE) {
            DWORD read = 0;
            if (!ReadFile(handle, buffer.data(),
                          static_cast<DWORD>(buffer.size()), &read, nullptr)) {
                const DWORD error = GetLastError();
                return error == ERROR_BROKEN_PIPE ||
                       error == ERROR_HANDLE_EOF ||
                       !running_.load(std::memory_order_acquire);
            }
            if (read == 0) break;
            if (!consume_bytes(line, buffer.data(), read, callback)) return false;
        }
        return line.empty() ||
               consume_bytes(line, "\n", 1, callback);
    }
#else
    int stdout_read_handle() const { return stdout_read_; }
    int stderr_read_handle() const { return stderr_read_; }

    bool read_loop(int fd, const LineCallback& callback) {
        std::string line;
        std::vector<char> buffer(kReadBufferBytes);
        while (fd >= 0) {
            const ssize_t read = ::read(fd, buffer.data(), buffer.size());
            if (read > 0) {
                if (!consume_bytes(line, buffer.data(),
                                   static_cast<std::size_t>(read), callback)) {
                    return false;
                }
            } else if (read == 0) {
                break;
            } else if (errno != EINTR) {
                return !running_.load(std::memory_order_acquire);
            }
        }
        return line.empty() ||
               consume_bytes(line, "\n", 1, callback);
    }
#endif

    void notify_exit_once() {
        if (exit_notified_.exchange(true, std::memory_order_acq_rel)) return;
        ExitCallback callback;
        {
            std::lock_guard<std::mutex> callback_lock(callback_mutex_);
            callback = on_exit_;
        }
        if (!callback) return;
        try {
            callback();
        } catch (...) {
        }
    }

    std::mutex lifecycle_mutex_;
    std::mutex write_mutex_;
    std::mutex callback_mutex_;
    bool started_ = false;
    std::atomic<bool> running_{false};
    std::atomic<bool> exit_notified_{false};
    std::thread stdout_thread_;
    std::thread stderr_thread_;
    LineCallback on_stdout_line_;
    LineCallback on_stderr_line_;
    ExitCallback on_exit_;

#ifdef _WIN32
    PROCESS_INFORMATION process_info_{};
    HANDLE job_ = nullptr;
    HANDLE stdin_write_ = INVALID_HANDLE_VALUE;
    HANDLE stdout_read_ = INVALID_HANDLE_VALUE;
    HANDLE stderr_read_ = INVALID_HANDLE_VALUE;
#else
    pid_t pid_ = -1;
    int stdin_write_ = -1;
    int stdout_read_ = -1;
    int stderr_read_ = -1;
#endif
};

json openai_tool_from_mcp_tool(const McpServerConfig& config,
                               const json& tool) {
    const std::string tool_name = tool.value("name", std::string());
    const std::string chat_name =
        McpClientManager::make_chat_tool_name(config.id, tool_name);
    std::string description = tool.value("description", std::string());
    if (description.empty()) description = tool.value("title", std::string());
    if (description.empty()) {
        description = "MCP tool " + tool_name + " from " + config.name;
    }
    description = "[" + config.name + "] " + description;

    json parameters = tool.value("inputSchema", json::object());
    if (!parameters.is_object() || parameters.empty()) {
        parameters =
            json{{"type", "object"}, {"properties", json::object()}};
    }

    return json{{"type", "function"},
                {"function", json{{"name", chat_name},
                                  {"description", description},
                                  {"parameters", parameters}}}};
}

}  // namespace

struct McpClientManager::Runtime {
    explicit Runtime(McpServerConfig cfg) : config(std::move(cfg)) {}
    ~Runtime() { disconnect(); }

    struct Waiter {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        json response;
        std::weak_ptr<StdioProcess> owner;
    };

    McpServerConfig config;
    mutable std::mutex state_mutex;
    std::mutex connect_mutex;
    std::mutex lifecycle_mutex;
    std::uint64_t lifecycle_generation = 0;
    bool connected = false;
    std::string last_error;
    json server_info = json::object();
    json server_capabilities = json::object();
    std::string negotiated_protocol_version;
    json tools = json::array();
    std::shared_ptr<StdioProcess> process;

    std::atomic<std::int64_t> next_request_id{1};
    std::mutex waiters_mutex;
    std::map<std::int64_t, std::shared_ptr<Waiter>> waiters;

    void connect(const McpServerConfig& new_config) {
        // Capture the generation before waiting for the connect serializer. A
        // disconnect that happens while this call is queued therefore cancels
        // it instead of allowing it to reconnect after disconnect returns.
        std::uint64_t connect_generation = 0;
        {
            std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex);
            connect_generation = lifecycle_generation;
        }

        std::lock_guard<std::mutex> connect_lock(connect_mutex);

        // Dispose of a process that exited or failed protocol validation before
        // installing a replacement. This work is outside state_mutex so status
        // snapshots never wait on process teardown.
        std::shared_ptr<StdioProcess> stale_process;
        {
            std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex);
            if (lifecycle_generation != connect_generation) {
                throw std::runtime_error(
                    "MCP connection was cancelled before initialization");
            }
            {
                std::lock_guard<std::mutex> state_lock(state_mutex);
                if (connected) return;
                stale_process = std::move(process);
                connected = false;
                tools = json::array();
            }
            if (stale_process) {
                cancel_waiters_for(stale_process,
                                   "MCP server connection was replaced", 499);
            }
        }
        if (stale_process) stale_process->stop();

        auto candidate = std::make_shared<StdioProcess>();
        try {
            {
                // Installing and starting the candidate under lifecycle_mutex
                // closes the small race where disconnect could detach it before
                // StdioProcess::start has initialized its handles.
                std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex);
                if (lifecycle_generation != connect_generation) {
                    throw std::runtime_error(
                        "MCP connection was cancelled before process start");
                }
                {
                    std::lock_guard<std::mutex> state_lock(state_mutex);
                    config = new_config;
                    connected = false;
                    last_error.clear();
                    server_info = json::object();
                    server_capabilities = json::object();
                    negotiated_protocol_version.clear();
                    tools = json::array();
                    process = candidate;
                }

                std::weak_ptr<StdioProcess> weak_candidate(candidate);
                candidate->start(
                    new_config,
                    [this, weak_candidate](const std::string& line) {
                        if (auto source = weak_candidate.lock()) {
                            handle_stdout_line(source, line);
                        }
                    },
                    [this](const std::string& line) {
                        handle_stderr_line(line);
                    },
                    [this, weak_candidate] {
                        if (auto source = weak_candidate.lock()) {
                            handle_process_exit(source);
                        }
                    });
            }

            const json init = request(
                candidate, "initialize",
                json{{"protocolVersion", kProtocolVersion},
                     {"capabilities", json::object()},
                     {"clientInfo",
                      json{{"name", "lemonade-mcp-client"},
                           {"title", "Lemonade MCP Client Host"},
                           {"version", "1"}}}},
                new_config.timeout_ms);
            if (init.contains("error")) {
                throw std::runtime_error(
                    "initialize failed: " + json_rpc_error_message(init));
            }
            if (!init.contains("result") || !init["result"].is_object()) {
                throw std::runtime_error(
                    "MCP initialize response is missing an object result");
            }

            const json result = init["result"];
            if (!result.contains("protocolVersion") ||
                !result["protocolVersion"].is_string()) {
                throw std::runtime_error(
                    "MCP initialize result is missing protocolVersion");
            }
            const std::string protocol_version =
                result["protocolVersion"].get<std::string>();
            if (!supported_protocol_version(protocol_version)) {
                throw std::runtime_error(
                    "MCP server negotiated unsupported protocol version: " +
                    protocol_version);
            }

            const json info = result.value("serverInfo", json::object());
            const json capabilities =
                result.value("capabilities", json::object());
            if (!info.is_object() || !capabilities.is_object()) {
                throw std::runtime_error(
                    "MCP initialize result contains invalid server metadata");
            }

            notify(candidate, "notifications/initialized", json::object(),
                   true);

            json discovered_tools = json::array();
            if (capabilities.contains("tools")) {
                if (!capabilities["tools"].is_object()) {
                    throw std::runtime_error(
                        "MCP tools capability must be an object");
                }
                discovered_tools =
                    fetch_tools(candidate, new_config.timeout_ms);
            }

            {
                std::lock_guard<std::mutex> state_lock(state_mutex);
                if (process != candidate) {
                    throw std::runtime_error(
                        "MCP connection was cancelled during initialization");
                }
                config = new_config;
                negotiated_protocol_version = protocol_version;
                server_info = info;
                server_capabilities = capabilities;
                tools = std::move(discovered_tools);
                connected = true;
                last_error.clear();
            }
        } catch (const std::exception& e) {
            fail_candidate(candidate, e.what());
            throw;
        } catch (...) {
            fail_candidate(candidate, "Unknown MCP connection failure");
            throw;
        }
    }

    void disconnect() {
        std::shared_ptr<StdioProcess> old_process;
        {
            std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex);
            ++lifecycle_generation;
            {
                std::lock_guard<std::mutex> state_lock(state_mutex);
                old_process = std::move(process);
                connected = false;
                last_error.clear();
                server_info = json::object();
                server_capabilities = json::object();
                negotiated_protocol_version.clear();
                tools = json::array();
            }
            if (old_process) {
                cancel_waiters_for(old_process, "MCP server disconnected", 499);
            }
        }
        if (old_process) old_process->stop();
    }

    void refresh_tools() {
        std::shared_ptr<StdioProcess> source;
        int timeout_ms = kDefaultTimeoutMs;
        {
            std::lock_guard<std::mutex> state_lock(state_mutex);
            if (!connected || !process) {
                throw std::runtime_error("MCP server is not connected");
            }
            source = process;
            timeout_ms = config.timeout_ms;
            if (!server_capabilities.contains("tools")) {
                tools = json::array();
                return;
            }
        }

        json discovered = fetch_tools(source, timeout_ms);
        {
            std::lock_guard<std::mutex> state_lock(state_mutex);
            if (!connected || process != source) {
                throw std::runtime_error(
                    "MCP server disconnected while refreshing tools");
            }
            tools = std::move(discovered);
        }
    }

    json call_tool(const std::string& name, const json& arguments,
                   int timeout_ms) {
        std::shared_ptr<StdioProcess> source;
        int configured_timeout = kDefaultTimeoutMs;
        {
            std::lock_guard<std::mutex> state_lock(state_mutex);
            if (!connected || !process) {
                throw std::runtime_error("MCP server is not connected");
            }
            source = process;
            configured_timeout = config.timeout_ms;
            if (!server_capabilities.contains("tools")) {
                throw std::runtime_error(
                    "MCP server did not advertise the tools capability");
            }
        }

        json params{{"name", name},
                    {"arguments",
                     arguments.is_object() ? arguments : json::object()}};
        const json response = request(
            source, "tools/call", std::move(params),
            timeout_ms > 0 ? timeout_ms : configured_timeout);
        if (response.contains("error")) {
            throw std::runtime_error(json_rpc_error_message(response));
        }
        if (!response.contains("result")) {
            throw std::runtime_error(
                "MCP tools/call response is missing result");
        }
        return response["result"];
    }

    json snapshot(bool include_env_values = false) const {
        std::lock_guard<std::mutex> state_lock(state_mutex);
        json out =
            McpClientManager::config_to_json(config, include_env_values);
        out["status"] = connected ? "connected" : "disconnected";
        out["connected"] = connected;
        out["last_error"] = last_error;
        out["protocol_version"] = negotiated_protocol_version;
        out["server_info"] = server_info;
        out["capabilities"] = server_capabilities;
        out["tools"] = tools;
        return out;
    }

private:
    bool is_current_process(
        const std::shared_ptr<StdioProcess>& source) const {
        std::lock_guard<std::mutex> state_lock(state_mutex);
        return process == source;
    }

    void fail_candidate(const std::shared_ptr<StdioProcess>& candidate,
                        const std::string& message) {
        bool owned = false;
        {
            std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex);
            {
                std::lock_guard<std::mutex> state_lock(state_mutex);
                if (process == candidate) {
                    process.reset();
                    connected = false;
                    last_error = message;
                    server_info = json::object();
                    server_capabilities = json::object();
                    negotiated_protocol_version.clear();
                    tools = json::array();
                    owned = true;
                }
            }
            cancel_waiters_for(candidate, message, 502);
        }
        if (owned) candidate->stop();
    }

    std::int64_t allocate_request_id() {
        const std::int64_t id =
            next_request_id.fetch_add(1, std::memory_order_relaxed);
        if (id <= 0 || id == std::numeric_limits<std::int64_t>::max()) {
            throw std::runtime_error("MCP request id space exhausted");
        }
        return id;
    }

    json request(const std::shared_ptr<StdioProcess>& source,
                 const std::string& method, json params, int timeout_ms) {
        const std::int64_t id = allocate_request_id();
        auto waiter = std::make_shared<Waiter>();
        waiter->owner = source;

        {
            // Registering under lifecycle_mutex ensures disconnect cannot cancel
            // the old set and then miss a waiter added for the detached process.
            std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex);
            if (!is_current_process(source)) {
                throw std::runtime_error("MCP server is disconnected");
            }
            std::lock_guard<std::mutex> waiters_lock(waiters_mutex);
            waiters[id] = waiter;
        }

        const json message =
            json_rpc_request(id, method, std::move(params));
        if (!source->write_line(message.dump())) {
            remove_waiter(id, waiter);
            throw std::runtime_error(
                "Failed to write JSON-RPC request to MCP server");
        }

        const auto timeout =
            std::chrono::milliseconds(clamp_timeout_ms(timeout_ms));
        std::unique_lock<std::mutex> waiter_lock(waiter->mutex);
        if (!waiter->cv.wait_for(waiter_lock, timeout,
                                 [&] { return waiter->done; })) {
            waiter_lock.unlock();
            const bool removed = remove_waiter(id, waiter);
            if (removed) {
                notify(source, "notifications/cancelled",
                       json{{"requestId", id}, {"reason", "timeout"}},
                       false);
            }
            throw std::runtime_error("MCP request timed out: " + method);
        }
        return waiter->response;
    }

    bool remove_waiter(std::int64_t id,
                       const std::shared_ptr<Waiter>& expected) {
        std::lock_guard<std::mutex> waiters_lock(waiters_mutex);
        const auto it = waiters.find(id);
        if (it == waiters.end() || it->second != expected) return false;
        waiters.erase(it);
        return true;
    }

    void cancel_waiters_for(const std::shared_ptr<StdioProcess>& owner,
                            const std::string& message, int status) {
        std::vector<std::shared_ptr<Waiter>> cancelled;
        {
            std::lock_guard<std::mutex> waiters_lock(waiters_mutex);
            for (auto it = waiters.begin(); it != waiters.end();) {
                if (it->second->owner.lock() == owner) {
                    cancelled.push_back(it->second);
                    it = waiters.erase(it);
                } else {
                    ++it;
                }
            }
        }

        for (const auto& waiter : cancelled) {
            std::lock_guard<std::mutex> waiter_lock(waiter->mutex);
            if (waiter->done) continue;
            waiter->response = make_error(message, status);
            waiter->done = true;
            waiter->cv.notify_all();
        }
    }

    void notify(const std::shared_ptr<StdioProcess>& source,
                const std::string& method, json params, bool required) {
        if (!source || !is_current_process(source)) {
            if (required) {
                throw std::runtime_error("MCP server is disconnected");
            }
            return;
        }
        const json message =
            json_rpc_notification(method, std::move(params));
        if (!source->write_line(message.dump()) && required) {
            throw std::runtime_error(
                "Failed to write MCP notification: " + method);
        }
    }

    json fetch_tools(const std::shared_ptr<StdioProcess>& source,
                     int timeout_ms) {
        json collected = json::array();
        std::string cursor;
        for (int page = 0; page < kMaxToolListPages; ++page) {
            json params = json::object();
            if (!cursor.empty()) params["cursor"] = cursor;

            const json response =
                request(source, "tools/list", std::move(params), timeout_ms);
            if (response.contains("error")) {
                throw std::runtime_error(
                    "tools/list failed: " +
                    json_rpc_error_message(response));
            }
            if (!response.contains("result") ||
                !response["result"].is_object()) {
                throw std::runtime_error(
                    "MCP tools/list response is missing an object result");
            }

            const json& result = response["result"];
            if (result.contains("tools")) {
                if (!result["tools"].is_array()) {
                    throw std::runtime_error(
                        "MCP tools/list result.tools must be an array");
                }
                for (const auto& tool : result["tools"]) {
                    if (!tool.is_object() || !tool.contains("name") ||
                        !tool["name"].is_string() ||
                        tool["name"].get<std::string>().empty()) {
                        throw std::runtime_error(
                            "MCP tools/list returned an invalid tool entry");
                    }
                    collected.push_back(tool);
                }
            }

            cursor.clear();
            if (result.contains("nextCursor") &&
                !result["nextCursor"].is_null()) {
                if (!result["nextCursor"].is_string()) {
                    throw std::runtime_error(
                        "MCP tools/list nextCursor must be a string");
                }
                cursor = result["nextCursor"].get<std::string>();
            }
            if (cursor.empty()) return collected;
        }

        throw std::runtime_error(
            "MCP tools/list exceeded the 32-page safety limit");
    }

    void handle_stdout_line(
        const std::shared_ptr<StdioProcess>& source,
        const std::string& line) {
        json message;
        try {
            message = json::parse(line);
        } catch (const std::exception& e) {
            protocol_failure(source,
                             std::string("Invalid JSON on MCP stdout: ") +
                                 e.what());
            return;
        }

        if (!message.is_object() ||
            message.value("jsonrpc", std::string()) != "2.0") {
            protocol_failure(source,
                             "MCP stdout message is not valid JSON-RPC 2.0");
            return;
        }

        if (message.contains("id") &&
            (message.contains("result") || message.contains("error"))) {
            std::optional<std::int64_t> id;
            if (message["id"].is_number_integer()) {
                id = message["id"].get<std::int64_t>();
            } else if (message["id"].is_number_unsigned()) {
                const auto unsigned_id =
                    message["id"].get<std::uint64_t>();
                if (unsigned_id <= static_cast<std::uint64_t>(
                                       std::numeric_limits<std::int64_t>::max())) {
                    id = static_cast<std::int64_t>(unsigned_id);
                }
            } else if (message["id"].is_string()) {
                try {
                    std::size_t parsed = 0;
                    const std::string text =
                        message["id"].get<std::string>();
                    const std::int64_t parsed_id = std::stoll(text, &parsed);
                    if (parsed == text.size()) id = parsed_id;
                } catch (...) {
                }
            }

            if (!id || *id < 0) {
                protocol_failure(source,
                                 "MCP response contains an invalid id");
                return;
            }

            std::shared_ptr<Waiter> waiter;
            {
                std::lock_guard<std::mutex> waiters_lock(waiters_mutex);
                const auto it = waiters.find(*id);
                if (it != waiters.end() &&
                    it->second->owner.lock() == source) {
                    waiter = it->second;
                    waiters.erase(it);
                }
            }
            if (waiter) {
                std::lock_guard<std::mutex> waiter_lock(waiter->mutex);
                waiter->response = std::move(message);
                waiter->done = true;
                waiter->cv.notify_all();
            }
            return;
        }

        if (message.contains("method") && message["method"].is_string() &&
            message.contains("id")) {
            const std::string method =
                message["method"].get<std::string>();
            source->write_line(
                json_rpc_method_not_found(message["id"], method).dump());
            return;
        }

        if (message.contains("method") && message["method"].is_string()) {
            LOG(DEBUG, "McpClient")
                << "MCP notification from " << server_id() << ": "
                << message["method"].get<std::string>() << std::endl;
            return;
        }

        protocol_failure(source,
                         "MCP stdout message is neither a response, request, nor notification");
    }

    void handle_stderr_line(const std::string& line) {
        LOG(DEBUG, "McpClient")
            << "[" << server_id() << " stderr] " << line << std::endl;
    }

    void handle_process_exit(
        const std::shared_ptr<StdioProcess>& source) {
        bool current = false;
        {
            std::lock_guard<std::mutex> state_lock(state_mutex);
            if (process == source) {
                connected = false;
                last_error = "MCP server process exited";
                server_info = json::object();
                server_capabilities = json::object();
                negotiated_protocol_version.clear();
                tools = json::array();
                current = true;
            }
        }
        if (current) {
            cancel_waiters_for(source, "MCP server process exited", 502);
            std::thread([source] { source->stop(); }).detach();
        }
    }

    void protocol_failure(const std::shared_ptr<StdioProcess>& source,
                          const std::string& message) {
        bool current = false;
        {
            std::lock_guard<std::mutex> state_lock(state_mutex);
            if (process == source) {
                connected = false;
                last_error = message;
                server_info = json::object();
                server_capabilities = json::object();
                negotiated_protocol_version.clear();
                tools = json::array();
                current = true;
            }
        }
        if (current) {
            cancel_waiters_for(source, message, 502);
            std::thread([source] { source->stop(); }).detach();
        }
    }

    std::string server_id() const {
        std::lock_guard<std::mutex> state_lock(state_mutex);
        return config.id;
    }
};

McpClientManager::McpClientManager(std::string cache_dir)
    : cache_dir_(std::move(cache_dir)) {
    if (cache_dir_.empty()) cache_dir_ = ".";
    config_path_ = (fs::path(cache_dir_) / kConfigFileName).string();
    load_config_file();
}

McpClientManager::~McpClientManager() { stop_all(); }

void McpClientManager::register_routes(httplib::Server& server) {
    auto self = shared_from_this();

    server.Get("/internal/mcp/servers",
               [self](const httplib::Request&, httplib::Response& res) {
                   set_json(res, self->list_servers_json());
               });
    server.Get("/internal/mcp/tools",
               [self](const httplib::Request&, httplib::Response& res) {
                   set_json(res, self->list_tools_json());
               });

    server.Post("/internal/mcp/servers",
                [self](const httplib::Request& req,
                       httplib::Response& res) {
                    json body;
                    if (!parse_json_body(req, res, body)) return;
                    try {
                        set_json(res, self->upsert_server_json(body));
                    } catch (const std::exception& e) {
                        set_error(res, e.what(), 400);
                    }
                });

    server.Delete(
        R"(/internal/mcp/servers/([A-Za-z0-9_.-]+))",
        [self](const httplib::Request& req, httplib::Response& res) {
            try {
                set_json(res,
                         self->remove_server_json(req.matches[1].str()));
            } catch (const std::exception& e) {
                set_error(res, e.what(), 404);
            }
        });

    server.Post(
        R"(/internal/mcp/servers/([A-Za-z0-9_.-]+)/connect)",
        [self](const httplib::Request& req, httplib::Response& res) {
            try {
                set_json(res,
                         self->connect_server_json(req.matches[1].str()));
            } catch (const std::exception& e) {
                set_error(res, e.what(), 502);
            }
        });

    server.Post(
        R"(/internal/mcp/servers/([A-Za-z0-9_.-]+)/disconnect)",
        [self](const httplib::Request& req, httplib::Response& res) {
            try {
                set_json(res,
                         self->disconnect_server_json(req.matches[1].str()));
            } catch (const std::exception& e) {
                set_error(res, e.what(), 404);
            }
        });

    server.Post(
        R"(/internal/mcp/servers/([A-Za-z0-9_.-]+)/refresh-tools)",
        [self](const httplib::Request& req, httplib::Response& res) {
            try {
                set_json(res,
                         self->refresh_tools_json(req.matches[1].str()));
            } catch (const std::exception& e) {
                set_error(res, e.what(), 502);
            }
        });

    server.Post(
        R"(/internal/mcp/servers/([A-Za-z0-9_.-]+)/tools/call)",
        [self](const httplib::Request& req, httplib::Response& res) {
            json body;
            if (!parse_json_body(req, res, body)) return;
            try {
                set_json(res,
                         self->call_tool_json(req.matches[1].str(), body));
            } catch (const std::exception& e) {
                set_error(res, e.what(), 502);
            }
        });
}

void McpClientManager::stop_all() {
    std::map<std::string, std::shared_ptr<Runtime>> runtimes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        runtimes.swap(runtimes_);
    }
    for (auto& [_, runtime] : runtimes) {
        if (runtime) runtime->disconnect();
    }
}

McpServerConfig McpClientManager::parse_server_config_json(
    const json& value, bool allow_missing_id) {
    if (!value.is_object()) {
        throw std::runtime_error("MCP server config must be an object");
    }

    auto read_string = [&value](const char* field,
                                const std::string& fallback = std::string()) {
        if (!value.contains(field)) return fallback;
        if (!value[field].is_string()) {
            throw std::runtime_error(std::string("MCP ") + field +
                                     " must be a string");
        }
        return value[field].get<std::string>();
    };

    McpServerConfig config;
    config.id = trim(read_string("id"));
    config.name = trim(read_string("name"));
    config.transport = trim(read_string("transport", "stdio"));
    config.command = trim(read_string("command"));

    if (value.contains("working_dir") && value.contains("workingDir")) {
        throw std::runtime_error(
            "Use only one of working_dir or workingDir");
    }
    config.working_dir = trim(value.contains("working_dir")
                                  ? read_string("working_dir")
                                  : read_string("workingDir"));

    if (value.contains("enabled")) {
        if (!value["enabled"].is_boolean()) {
            throw std::runtime_error("MCP enabled must be a boolean");
        }
        config.enabled = value["enabled"].get<bool>();
    }

    if (value.contains("timeout_ms") && value.contains("timeoutMs")) {
        throw std::runtime_error("Use only one of timeout_ms or timeoutMs");
    }
    const char* timeout_field = value.contains("timeout_ms")
                                    ? "timeout_ms"
                                    : "timeoutMs";
    if (value.contains(timeout_field)) {
        if (!value[timeout_field].is_number_integer()) {
            throw std::runtime_error("MCP timeout must be an integer");
        }
        const auto timeout = value[timeout_field].get<long long>();
        if (timeout < kMinTimeoutMs || timeout > kMaxTimeoutMs) {
            throw std::runtime_error(
                "MCP timeout must be between 1000 and 300000 ms");
        }
        config.timeout_ms = static_cast<int>(timeout);
    }

    if (value.contains("args")) {
        if (!value["args"].is_array()) {
            throw std::runtime_error(
                "MCP args must be an array of strings");
        }
        for (const auto& arg : value["args"]) {
            if (!arg.is_string()) {
                throw std::runtime_error(
                    "MCP args must be an array of strings");
            }
            std::string text = arg.get<std::string>();
            if (text.find('\0') != std::string::npos) {
                throw std::runtime_error("MCP args must not contain NUL");
            }
            config.args.push_back(std::move(text));
        }
    }

    if (value.contains("env")) {
        if (!value["env"].is_object()) {
            throw std::runtime_error(
                "MCP env must be an object of string references");
        }
        for (auto it = value["env"].begin(); it != value["env"].end();
             ++it) {
            if (!valid_env_name(it.key())) {
                throw std::runtime_error("Invalid MCP env var name: " +
                                         it.key());
            }
            if (!it.value().is_string()) {
                throw std::runtime_error(
                    "MCP env values must be strings");
            }
            std::string reference = it.value().get<std::string>();
            if (!env_reference_name(reference)) {
                throw std::runtime_error(
                    "MCP env value for '" + it.key() +
                    "' must be a ${VARIABLE} reference; raw values are rejected");
            }
            config.env[it.key()] = std::move(reference);
        }
    }

    if (allow_missing_id && config.id.empty()) {
        config.id = sanitize_id_seed(
            !config.name.empty() ? config.name
                                 : basename_like(config.command));
    }
    if (!valid_id(config.id)) {
        throw std::runtime_error(
            "MCP server id must match [A-Za-z0-9_.-]+ and be at most 96 chars");
    }
    if (config.name.empty()) config.name = config.id;
    if (has_control_char(config.name)) {
        throw std::runtime_error(
            "MCP server name must not contain control characters");
    }
    if (config.transport != "stdio") {
        throw std::runtime_error(
            "This MCP client host supports only stdio transport");
    }
    if (config.command.empty()) {
        throw std::runtime_error(
            "MCP stdio server config requires command");
    }
    if (config.command.find('\0') != std::string::npos ||
        has_control_char(config.command)) {
        throw std::runtime_error(
            "MCP command must not contain control characters");
    }
    if (config.working_dir.find('\0') != std::string::npos ||
        has_control_char(config.working_dir)) {
        throw std::runtime_error(
            "MCP working_dir must not contain control characters");
    }

    validate_env_references(config);
    return config;
}

json McpClientManager::config_to_json(const McpServerConfig& config,
                                      bool include_env_values) {
    json env = json::object();
    for (const auto& [key, value] : config.env) {
        // A persisted config contains references only. Returning the reference is
        // safe and useful to clients; a legacy in-memory raw value stays masked.
        env[key] = (include_env_values || env_reference_name(value))
                       ? value
                       : "***";
    }
    return json{{"id", config.id},
                {"name", config.name},
                {"transport", config.transport},
                {"command", config.command},
                {"args", config.args},
                {"env", env},
                {"working_dir", config.working_dir},
                {"enabled", config.enabled},
                {"timeout_ms", config.timeout_ms}};
}

std::string McpClientManager::make_chat_tool_name(
    const std::string& server_id, const std::string& tool_name) {
    auto clean = [](const std::string& input) {
        std::string output;
        output.reserve(input.size());
        for (unsigned char c : input) {
            output.push_back(ascii_is_alnum(c) || c == '_' || c == '-'
                                 ? static_cast<char>(c)
                                 : '_');
        }
        while (!output.empty() && output.front() == '_') {
            output.erase(output.begin());
        }
        while (!output.empty() && output.back() == '_') output.pop_back();
        return output.empty() ? std::string("tool") : output;
    };

    const std::string clean_server = clean(server_id);
    const std::string clean_tool = clean(tool_name);
    const std::string raw = server_id + "\n" + tool_name;
    const std::string hash_suffix = "_" + fnv1a_hex16(raw);
    std::string name = "mcp_" + clean_server + "__" + clean_tool;

    const bool lossy = clean_server != server_id || clean_tool != tool_name;
    const bool ambiguous_separator =
        clean_server.find("__") != std::string::npos ||
        clean_tool.find("__") != std::string::npos;
    if (lossy || ambiguous_separator) name += hash_suffix;
    if (name.size() > 64) {
        const std::size_t keep = 64 - hash_suffix.size();
        name.resize(keep);
        while (!name.empty() &&
               (name.back() == '_' || name.back() == '-')) {
            name.pop_back();
        }
        name += hash_suffix;
    }
    return name;
}

json McpClientManager::list_servers_json() const {
    std::vector<std::pair<McpServerConfig, std::shared_ptr<Runtime>>> entries;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries.reserve(configs_.size());
        for (const auto& [id, config] : configs_) {
            const auto runtime_it = runtimes_.find(id);
            entries.emplace_back(
                config, runtime_it == runtimes_.end()
                            ? nullptr
                            : runtime_it->second);
        }
    }

    json servers = json::array();
    for (const auto& [config, runtime] : entries) {
        if (runtime) {
            servers.push_back(runtime->snapshot(false));
            continue;
        }
        json item = config_to_json(config, false);
        item["status"] = "disconnected";
        item["connected"] = false;
        item["last_error"] = "";
        item["protocol_version"] = "";
        item["server_info"] = json::object();
        item["capabilities"] = json::object();
        item["tools"] = json::array();
        servers.push_back(std::move(item));
    }
    return json{{"servers", std::move(servers)}};
}

json McpClientManager::list_tools_json() const {
    std::vector<std::pair<McpServerConfig, std::shared_ptr<Runtime>>> entries;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries.reserve(runtimes_.size());
        for (const auto& [id, runtime] : runtimes_) {
            const auto config_it = configs_.find(id);
            if (runtime && config_it != configs_.end()) {
                entries.emplace_back(config_it->second, runtime);
            }
        }
    }

    json tools_json = json::array();
    for (const auto& [config, runtime] : entries) {
        const json state = runtime->snapshot(false);
        if (!state.value("connected", false)) continue;
        const json available = state.value("tools", json::array());
        if (!available.is_array()) continue;
        for (const auto& tool : available) {
            if (!tool.is_object() || !tool.contains("name") ||
                !tool["name"].is_string()) {
                continue;
            }
            const std::string name = tool["name"].get<std::string>();
            tools_json.push_back(
                json{{"server_id", config.id},
                     {"server_name", config.name},
                     {"name", name},
                     {"chat_name", make_chat_tool_name(config.id, name)},
                     {"title", tool.value("title", std::string())},
                     {"description",
                      tool.value("description", std::string())},
                     {"inputSchema",
                      tool.value("inputSchema", json::object())},
                     {"tool", tool},
                     {"openai_tool",
                      openai_tool_from_mcp_tool(config, tool)}});
        }
    }
    return json{{"tools", std::move(tools_json)}};
}

json McpClientManager::upsert_server_json(const json& body) {
    const json& raw = body.contains("server") && body["server"].is_object()
                          ? body["server"]
                          : body;
    McpServerConfig config = parse_server_config_json(raw, true);

    std::shared_ptr<Runtime> old_runtime;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!raw.contains("id") ||
            (raw["id"].is_string() &&
             trim(raw["id"].get<std::string>()).empty())) {
            config.id = next_id_locked(config.id);
        }

        auto next_configs = configs_;
        next_configs[config.id] = config;
        save_config_file_locked(next_configs);
        configs_.swap(next_configs);

        const auto runtime_it = runtimes_.find(config.id);
        if (runtime_it != runtimes_.end()) {
            old_runtime = runtime_it->second;
            runtimes_.erase(runtime_it);
        }
    }
    if (old_runtime) old_runtime->disconnect();
    return json{{"server", config_to_json(config, false)}};
}

json McpClientManager::remove_server_json(const std::string& id) {
    std::shared_ptr<Runtime> runtime;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!configs_.count(id)) {
            throw std::runtime_error("Unknown MCP server: " + id);
        }
        auto next_configs = configs_;
        next_configs.erase(id);
        save_config_file_locked(next_configs);
        configs_.swap(next_configs);

        const auto runtime_it = runtimes_.find(id);
        if (runtime_it != runtimes_.end()) {
            runtime = runtime_it->second;
            runtimes_.erase(runtime_it);
        }
    }
    if (runtime) runtime->disconnect();
    return json{{"removed", id}};
}

json McpClientManager::connect_server_json(const std::string& id) {
    const McpServerConfig config = config_for_id(id);
    if (!config.enabled) {
        throw std::runtime_error("MCP server is disabled: " + id);
    }
    const auto runtime = get_or_create_runtime(config);
    runtime->connect(config);
    return json{{"server", runtime->snapshot(false)}};
}

json McpClientManager::disconnect_server_json(const std::string& id) {
    McpServerConfig config;
    std::shared_ptr<Runtime> runtime;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto config_it = configs_.find(id);
        if (config_it == configs_.end()) {
            throw std::runtime_error("Unknown MCP server: " + id);
        }
        config = config_it->second;
        const auto runtime_it = runtimes_.find(id);
        if (runtime_it != runtimes_.end()) runtime = runtime_it->second;
    }
    if (runtime) runtime->disconnect();

    json state = config_to_json(config, false);
    state["status"] = "disconnected";
    state["connected"] = false;
    state["last_error"] = "";
    state["protocol_version"] = "";
    state["server_info"] = json::object();
    state["capabilities"] = json::object();
    state["tools"] = json::array();
    return json{{"server", std::move(state)}};
}

json McpClientManager::refresh_tools_json(const std::string& id) {
    const McpServerConfig config = config_for_id(id);
    if (!config.enabled) {
        throw std::runtime_error("MCP server is disabled: " + id);
    }
    const auto runtime = get_or_create_runtime(config);
    runtime->connect(config);
    runtime->refresh_tools();
    return json{{"server", runtime->snapshot(false)}};
}

json McpClientManager::call_tool_json(const std::string& id,
                                      const json& body) {
    if (!body.contains("name") || !body["name"].is_string() ||
        body["name"].get<std::string>().empty()) {
        throw std::runtime_error(
            "tools/call body requires non-empty string field `name`");
    }
    if (body.contains("arguments") && !body["arguments"].is_object()) {
        throw std::runtime_error(
            "tools/call field `arguments` must be an object");
    }

    const McpServerConfig config = config_for_id(id);
    if (!config.enabled) {
        throw std::runtime_error("MCP server is disabled: " + id);
    }
    const auto runtime = get_or_create_runtime(config);
    runtime->connect(config);

    int timeout_ms = config.timeout_ms;
    if (body.contains("timeout_ms") && body.contains("timeoutMs")) {
        throw std::runtime_error("Use only one of timeout_ms or timeoutMs");
    }
    const char* timeout_field = body.contains("timeout_ms")
                                    ? "timeout_ms"
                                    : "timeoutMs";
    if (body.contains(timeout_field)) {
        if (!body[timeout_field].is_number_integer()) {
            throw std::runtime_error("tools/call timeout must be an integer");
        }
        const auto timeout = body[timeout_field].get<long long>();
        if (timeout < kMinTimeoutMs || timeout > kMaxTimeoutMs) {
            throw std::runtime_error(
                "tools/call timeout must be between 1000 and 300000 ms");
        }
        timeout_ms = static_cast<int>(timeout);
    }

    const std::string name = body["name"].get<std::string>();
    const json arguments = body.value("arguments", json::object());
    const json result = runtime->call_tool(name, arguments, timeout_ms);
    return json{{"server_id", id}, {"tool", name}, {"result", result}};
}

std::shared_ptr<McpClientManager::Runtime>
McpClientManager::get_or_create_runtime(const McpServerConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = runtimes_.find(config.id);
    if (it != runtimes_.end() && it->second) return it->second;
    auto runtime = std::make_shared<Runtime>(config);
    runtimes_[config.id] = runtime;
    return runtime;
}

McpServerConfig McpClientManager::config_for_id(
    const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = configs_.find(id);
    if (it == configs_.end()) {
        throw std::runtime_error("Unknown MCP server: " + id);
    }
    return it->second;
}

void McpClientManager::load_config_file() {
    std::lock_guard<std::mutex> lock(mutex_);
    configs_.clear();

    std::ifstream input(config_path_);
    if (!input.good()) return;
    try {
        const json document = json::parse(input);
        if (!document.is_object()) {
            throw std::runtime_error("root must be an object");
        }
        const json servers = document.value("servers", json::array());
        if (!servers.is_array()) {
            throw std::runtime_error("servers must be an array");
        }
        for (const auto& entry : servers) {
            try {
                McpServerConfig config =
                    parse_server_config_json(entry, false);
                if (configs_.count(config.id)) {
                    LOG(WARNING, "McpClient")
                        << "Ignoring duplicate MCP server id in config: "
                        << config.id << std::endl;
                    continue;
                }
                configs_.emplace(config.id, std::move(config));
            } catch (const std::exception& e) {
                LOG(WARNING, "McpClient")
                    << "Skipping invalid MCP server config: " << e.what()
                    << std::endl;
            }
        }
    } catch (const std::exception& e) {
        LOG(WARNING, "McpClient")
            << "Failed to read MCP config " << config_path_ << ": "
            << e.what() << std::endl;
    }
}

void McpClientManager::save_config_file_locked(
    const std::map<std::string, McpServerConfig>& configs) const {
    const fs::path path(config_path_);
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) {
        throw std::runtime_error(
            "Failed to create MCP config directory: " + error.message());
    }

    json servers = json::array();
    for (const auto& [_, config] : configs) {
        servers.push_back(config_to_persisted_json(config));
    }
    const json document{{"version", 1}, {"servers", std::move(servers)}};

    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary,
                             std::ios::binary | std::ios::trunc);
        if (!output.good()) {
            throw std::runtime_error(
                "Failed to open MCP config for writing: " +
                temporary.string());
        }
        output << std::setw(2) << document << '\n';
        output.flush();
        if (!output.good()) {
            output.close();
            std::error_code ignored;
            fs::remove(temporary, ignored);
            throw std::runtime_error(
                "Failed to write MCP config: " + temporary.string());
        }
    }

#ifndef _WIN32
    if (::chmod(temporary.c_str(), S_IRUSR | S_IWUSR) != 0) {
        const int saved_errno = errno;
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw std::runtime_error(
            std::string("Failed to secure MCP config permissions: ") +
            std::strerror(saved_errno));
    }
#endif

#ifdef _WIN32
    const std::wstring source = utf8_to_wide(temporary.string());
    const std::wstring destination = utf8_to_wide(path.string());
    if (!MoveFileExW(source.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD move_error = GetLastError();
        DeleteFileW(source.c_str());
        throw std::runtime_error(
            "Failed to replace MCP config atomically (GetLastError=" +
            std::to_string(move_error) + ")");
    }
#else
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        const int saved_errno = errno;
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw std::runtime_error(
            std::string("Failed to replace MCP config atomically: ") +
            std::strerror(saved_errno));
    }
#endif
}

std::string McpClientManager::next_id_locked(
    const std::string& seed) const {
    const std::string base = sanitize_id_seed(seed);
    std::string id = base;
    for (int suffix = 2; configs_.count(id); ++suffix) {
        id = base + "-" + std::to_string(suffix);
    }
    return id;
}

void register_mcp_client_routes(httplib::Server& server,
                                const std::string& cache_dir) {
    static std::mutex managers_mutex;
    static std::map<std::string, std::weak_ptr<McpClientManager>> managers;

    std::shared_ptr<McpClientManager> manager;
    {
        std::lock_guard<std::mutex> lock(managers_mutex);
        const auto it = managers.find(cache_dir);
        if (it != managers.end()) manager = it->second.lock();
        if (!manager) {
            manager = std::make_shared<McpClientManager>(cache_dir);
            managers[cache_dir] = manager;
        }
    }
    manager->register_routes(server);
}

}  // namespace lemon
