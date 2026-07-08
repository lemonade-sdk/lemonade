#include "lemon/mcp_client.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#include <lemon/utils/aixlog.hpp>

#ifdef _WIN32
    #define NOMINMAX
    #include <windows.h>
#else
    #include <cerrno>
    #include <csignal>
    #include <cstring>
    #include <fcntl.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace lemon {
namespace {

constexpr const char* kProtocolVersion = "2025-06-18";
constexpr const char* kProtocolVersion20250326 = "2025-03-26";
constexpr const char* kProtocolVersion20241105 = "2024-11-05";
constexpr const char* kConfigFileName = "mcp_servers.json";
constexpr int kDefaultTimeoutMs = 30000;
constexpr int kMinTimeoutMs = 1000;
constexpr int kMaxTimeoutMs = 300000;
constexpr int kMaxToolListPages = 32;

json make_error(const std::string& message, int status_code = 400,
                const std::string& type = "mcp_client_error") {
    return json{{"error", json{{"message", message}, {"type", type}, {"status_code", status_code}}}};
}

void set_json(httplib::Response& res, const json& body, int status = 200) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

void set_error(httplib::Response& res, const std::string& message, int status = 400,
               const std::string& type = "mcp_client_error") {
    set_json(res, make_error(message, status, type), status);
}

bool parse_json_body(const httplib::Request& req, httplib::Response& res, json& out,
                     bool required = true) {
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

bool has_control_char(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
    });
}

std::string trim(std::string value) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

bool valid_id(const std::string& id) {
    if (id.empty() || id.size() > 96) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_' || c == '.';
    });
}

std::string sanitize_id_seed(const std::string& seed) {
    std::string out;
    out.reserve(seed.size());
    bool last_dash = false;
    for (unsigned char c : seed) {
        const bool ok = std::isalnum(c) || c == '_' || c == '.';
        if (ok) {
            out.push_back(static_cast<char>(std::tolower(c)));
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
    size_t pos = command.find_last_of("/\\");
    if (pos == std::string::npos) return command;
    return command.substr(pos + 1);
}

bool valid_env_name(const std::string& name) {
    if (name.empty()) return false;
    unsigned char first = static_cast<unsigned char>(name[0]);
    if (!(std::isalpha(first) || first == '_')) return false;
    return std::all_of(name.begin() + 1, name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_';
    });
}


std::optional<std::string> env_reference_name(const std::string& value) {
    if (value.size() < 4 || value.rfind("${", 0) != 0 || value.back() != '}') {
        return std::nullopt;
    }
    std::string name = value.substr(2, value.size() - 3);
    if (!valid_env_name(name)) return std::nullopt;
    return name;
}

McpServerConfig resolve_env_references(McpServerConfig config) {
    for (auto& [key, value] : config.env) {
        auto ref = env_reference_name(value);
        if (!ref) continue;
        const char* env_value = std::getenv(ref->c_str());
        value = env_value ? std::string(env_value) : std::string();
    }
    return config;
}

json config_to_persisted_json(const McpServerConfig& config) {
    json out = McpClientManager::config_to_json(config, true);
    json env = json::object();
    for (const auto& [key, value] : config.env) {
        if (env_reference_name(value)) {
            env[key] = value;
        } else {
            // MCP env values commonly contain tokens. Do not write raw secrets to
            // the cache file; persist an environment-variable reference instead.
            env[key] = "${" + key + "}";
        }
    }
    out["env"] = std::move(env);
    return out;
}

int clamp_timeout_ms(int timeout_ms) {
    return std::max(kMinTimeoutMs, std::min(kMaxTimeoutMs, timeout_ms));
}

bool supported_protocol_version(const std::string& version) {
    // The PR1 stdio foundation only uses initialize, tools/list and tools/call.
    // Those are stable across the recent protocol revisions that existing MCP
    // servers commonly advertise. Unknown future/ancient versions are rejected
    // so we do not silently run with incompatible semantics.
    return version == kProtocolVersion ||
           version == kProtocolVersion20250326 ||
           version == kProtocolVersion20241105;
}

std::string fnv1a_hex8(const std::string& value) {
    std::uint32_t hash = 2166136261u;
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 16777619u;
    }
    std::ostringstream oss;
    oss << std::hex << std::setw(8) << std::setfill('0') << hash;
    return oss.str();
}

json json_rpc_request(int id, const std::string& method, json params = json::object()) {
    json msg{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
    if (!params.is_null()) msg["params"] = std::move(params);
    return msg;
}

json json_rpc_notification(const std::string& method, json params = json::object()) {
    json msg{{"jsonrpc", "2.0"}, {"method", method}};
    if (!params.is_null()) msg["params"] = std::move(params);
    return msg;
}

json json_rpc_method_not_found(const json& id, const std::string& method) {
    return json{{"jsonrpc", "2.0"},
                {"id", id},
                {"error", json{{"code", -32601}, {"message", "Unsupported MCP client request: " + method}}}};
}

std::string json_rpc_error_message(const json& response) {
    if (!response.contains("error")) return "";
    const auto& err = response["error"];
    if (err.is_object()) {
        return err.value("message", err.dump());
    }
    if (err.is_string()) return err.get<std::string>();
    return err.dump();
}

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (len <= 0) throw std::runtime_error("Failed to convert UTF-8 to UTF-16");
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), len);
    return out;
}

std::wstring quote_windows_arg(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";
    const bool needs_quotes = arg.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needs_quotes) return arg;

    std::wstring out = L"\"";
    size_t backslashes = 0;
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

std::wstring build_windows_command_line(const std::string& command, const std::vector<std::string>& args) {
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

std::vector<wchar_t> build_windows_environment_block(const std::map<std::string, std::string>& overrides) {
    std::vector<std::wstring> hidden_entries;
    std::map<std::wstring, std::wstring, CaseInsensitiveWideLess> entries;

    LPWCH raw = GetEnvironmentStringsW();
    if (raw) {
        for (LPWCH p = raw; *p; ) {
            std::wstring entry(p);
            p += entry.size() + 1;
            if (!entry.empty() && entry[0] == L'=') {
                hidden_entries.push_back(entry);
                continue;
            }
            size_t eq = entry.find(L'=');
            if (eq != std::wstring::npos) {
                entries[entry.substr(0, eq)] = entry;
            }
        }
        FreeEnvironmentStringsW(raw);
    }

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
#endif

class StdioProcess {
public:
    using LineCallback = std::function<void(const std::string&)>;

    StdioProcess() = default;
    ~StdioProcess() { stop(); }

    StdioProcess(const StdioProcess&) = delete;
    StdioProcess& operator=(const StdioProcess&) = delete;

    void start(const McpServerConfig& config, LineCallback on_stdout_line, LineCallback on_stderr_line) {
        stop();
        on_stdout_line_ = std::move(on_stdout_line);
        on_stderr_line_ = std::move(on_stderr_line);

        McpServerConfig process_config = resolve_env_references(config);

        if (!process_config.working_dir.empty()) {
            std::error_code ec;
            if (!fs::is_directory(fs::path(process_config.working_dir), ec) || ec) {
                throw std::runtime_error("MCP working_dir is not a readable directory: " + process_config.working_dir);
            }
        }

#ifdef _WIN32
        start_windows(process_config);
#else
        start_posix(process_config);
#endif
        running_.store(true, std::memory_order_release);
        stdout_thread_ = std::thread([this] { read_loop_stdout(); });
        stderr_thread_ = std::thread([this] { read_loop_stderr(); });
    }

    bool write_line(const std::string& line) {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (!running_.load(std::memory_order_acquire)) return false;
        std::string payload = line;
        payload.push_back('\n');
#ifdef _WIN32
        DWORD total = 0;
        const char* data = payload.data();
        DWORD remaining = static_cast<DWORD>(payload.size());
        while (remaining > 0) {
            DWORD written = 0;
            if (!WriteFile(stdin_write_, data + total, remaining, &written, nullptr) || written == 0) {
                return false;
            }
            total += written;
            remaining -= written;
        }
        FlushFileBuffers(stdin_write_);
        return true;
#else
        const char* data = payload.data();
        size_t remaining = payload.size();
        while (remaining > 0) {
            ssize_t written = ::write(stdin_write_, data, remaining);
            if (written < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (written == 0) return false;
            data += written;
            remaining -= static_cast<size_t>(written);
        }
        return true;
#endif
    }

    void stop() {
        if (!started_) return;
        running_.store(false, std::memory_order_release);

#ifdef _WIN32
        if (stdin_write_ != INVALID_HANDLE_VALUE) {
            CloseHandle(stdin_write_);
            stdin_write_ = INVALID_HANDLE_VALUE;
        }
        if (process_info_.hProcess) {
            DWORD wait = WaitForSingleObject(process_info_.hProcess, 1000);
            if (wait == WAIT_TIMEOUT) {
                TerminateProcess(process_info_.hProcess, 1);
                WaitForSingleObject(process_info_.hProcess, 1000);
            }
        }
#else
        if (stdin_write_ >= 0) {
            ::close(stdin_write_);
            stdin_write_ = -1;
        }
        if (pid_ > 0) {
            int status = 0;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (std::chrono::steady_clock::now() < deadline) {
                pid_t r = ::waitpid(pid_, &status, WNOHANG);
                if (r == pid_) break;
                if (r < 0 && errno == ECHILD) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            if (::waitpid(pid_, &status, WNOHANG) == 0) {
                ::kill(pid_, SIGTERM);
                deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                while (std::chrono::steady_clock::now() < deadline) {
                    pid_t r = ::waitpid(pid_, &status, WNOHANG);
                    if (r == pid_) break;
                    if (r < 0 && errno == ECHILD) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
                if (::waitpid(pid_, &status, WNOHANG) == 0) {
                    ::kill(pid_, SIGKILL);
                    ::waitpid(pid_, &status, 0);
                }
            }
        }
        // Reader fds are closed after joining reader threads. Closing an fd
        // while another thread is blocked in read() can race with fd reuse.
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
    }

private:
#ifdef _WIN32
    void start_windows(const McpServerConfig& config) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;

        HANDLE stdin_read = INVALID_HANDLE_VALUE;
        HANDLE stdout_write = INVALID_HANDLE_VALUE;
        HANDLE stderr_write = INVALID_HANDLE_VALUE;

        if (!CreatePipe(&stdin_read, &stdin_write_, &sa, 0)) {
            throw std::runtime_error("CreatePipe(stdin) failed");
        }
        if (!CreatePipe(&stdout_read_, &stdout_write, &sa, 0)) {
            CloseHandle(stdin_read);
            CloseHandle(stdin_write_);
            stdin_write_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error("CreatePipe(stdout) failed");
        }
        if (!CreatePipe(&stderr_read_, &stderr_write, &sa, 0)) {
            CloseHandle(stdin_read);
            CloseHandle(stdin_write_);
            CloseHandle(stdout_read_);
            CloseHandle(stdout_write);
            stdin_write_ = INVALID_HANDLE_VALUE;
            stdout_read_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error("CreatePipe(stderr) failed");
        }

        SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(stderr_read_, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = stdin_read;
        si.hStdOutput = stdout_write;
        si.hStdError = stderr_write;

        std::wstring cmdline = build_windows_command_line(config.command, config.args);
        std::vector<wchar_t> mutable_cmdline(cmdline.begin(), cmdline.end());
        mutable_cmdline.push_back(L'\0');
        std::wstring cwd = config.working_dir.empty() ? std::wstring() : utf8_to_wide(config.working_dir);
        std::vector<wchar_t> env_block = build_windows_environment_block(config.env);

        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(
            nullptr,
            mutable_cmdline.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
            env_block.empty() ? nullptr : env_block.data(),
            cwd.empty() ? nullptr : cwd.c_str(),
            &si,
            &pi);

        CloseHandle(stdin_read);
        CloseHandle(stdout_write);
        CloseHandle(stderr_write);

        if (!ok) {
            DWORD err = GetLastError();
            CloseHandle(stdin_write_);
            CloseHandle(stdout_read_);
            CloseHandle(stderr_read_);
            stdin_write_ = stdout_read_ = stderr_read_ = INVALID_HANDLE_VALUE;
            std::ostringstream oss;
            oss << "CreateProcess failed for MCP server '" << config.command << "' (GetLastError=" << err << ")";
            throw std::runtime_error(oss.str());
        }

        process_info_ = pi;
        started_ = true;
    }
#else
    void start_posix(const McpServerConfig& config) {
        int stdin_pipe[2] = {-1, -1};
        int stdout_pipe[2] = {-1, -1};
        int stderr_pipe[2] = {-1, -1};
        auto close_pair = [](int pair[2]) {
            if (pair[0] >= 0) ::close(pair[0]);
            if (pair[1] >= 0) ::close(pair[1]);
        };
        if (::pipe(stdin_pipe) != 0 || ::pipe(stdout_pipe) != 0 || ::pipe(stderr_pipe) != 0) {
            int err = errno;
            close_pair(stdin_pipe);
            close_pair(stdout_pipe);
            close_pair(stderr_pipe);
            throw std::runtime_error(std::string("pipe failed: ") + std::strerror(err));
        }

        pid_t pid = ::fork();
        if (pid < 0) {
            int err = errno;
            ::close(stdin_pipe[0]); ::close(stdin_pipe[1]);
            ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
            ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
            throw std::runtime_error(std::string("fork failed: ") + std::strerror(err));
        }

        if (pid == 0) {
            ::dup2(stdin_pipe[0], STDIN_FILENO);
            ::dup2(stdout_pipe[1], STDOUT_FILENO);
            ::dup2(stderr_pipe[1], STDERR_FILENO);

            ::close(stdin_pipe[0]);
            ::close(stdin_pipe[1]);
            ::close(stdout_pipe[0]);
            ::close(stdout_pipe[1]);
            ::close(stderr_pipe[0]);
            ::close(stderr_pipe[1]);

            if (!config.working_dir.empty()) {
                ::chdir(config.working_dir.c_str());
            }
            for (const auto& [key, value] : config.env) {
                ::setenv(key.c_str(), value.c_str(), 1);
            }

            std::vector<std::string> argv_strings;
            argv_strings.reserve(config.args.size() + 1);
            argv_strings.push_back(config.command);
            for (const auto& arg : config.args) argv_strings.push_back(arg);

            std::vector<char*> argv;
            argv.reserve(argv_strings.size() + 1);
            for (auto& value : argv_strings) argv.push_back(value.data());
            argv.push_back(nullptr);

            ::execvp(config.command.c_str(), argv.data());
            _exit(127);
        }

        ::close(stdin_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[1]);

        pid_ = pid;
        stdin_write_ = stdin_pipe[1];
        stdout_read_ = stdout_pipe[0];
        stderr_read_ = stderr_pipe[0];
        started_ = true;
    }
#endif

    void read_loop_stdout() { read_loop(stdout_read_handle(), on_stdout_line_); }
    void read_loop_stderr() { read_loop(stderr_read_handle(), on_stderr_line_); }

#ifdef _WIN32
    HANDLE stdout_read_handle() const { return stdout_read_; }
    HANDLE stderr_read_handle() const { return stderr_read_; }

    void read_loop(HANDLE handle, const LineCallback& cb) {
        std::string line;
        char ch = 0;
        DWORD n = 0;
        while (handle != INVALID_HANDLE_VALUE && ReadFile(handle, &ch, 1, &n, nullptr) && n == 1) {
            if (ch == '\n') {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) cb(line);
                line.clear();
            } else {
                line.push_back(ch);
            }
        }
        if (!line.empty()) cb(line);
    }
#else
    int stdout_read_handle() const { return stdout_read_; }
    int stderr_read_handle() const { return stderr_read_; }

    void read_loop(int fd, const LineCallback& cb) {
        std::string line;
        char ch = 0;
        while (fd >= 0) {
            ssize_t n = ::read(fd, &ch, 1);
            if (n == 1) {
                if (ch == '\n') {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (!line.empty()) cb(line);
                    line.clear();
                } else {
                    line.push_back(ch);
                }
            } else if (n == 0) {
                break;
            } else if (errno != EINTR) {
                break;
            }
        }
        if (!line.empty()) cb(line);
    }
#endif

    bool started_ = false;
    std::atomic<bool> running_{false};
    std::mutex write_mutex_;
    std::thread stdout_thread_;
    std::thread stderr_thread_;
    LineCallback on_stdout_line_;
    LineCallback on_stderr_line_;

#ifdef _WIN32
    PROCESS_INFORMATION process_info_{};
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

json openai_tool_from_mcp_tool(const McpServerConfig& config, const json& tool) {
    const std::string tool_name = tool.value("name", std::string());
    const std::string chat_name = McpClientManager::make_chat_tool_name(config.id, tool_name);
    std::string description = tool.value("description", std::string());
    if (description.empty()) description = tool.value("title", std::string());
    if (description.empty()) description = "MCP tool " + tool_name + " from " + config.name;
    description = "[" + config.name + "] " + description;

    json parameters = tool.value("inputSchema", json::object());
    if (!parameters.is_object() || parameters.empty()) {
        parameters = json{{"type", "object"}, {"properties", json::object()}};
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
    };

    McpServerConfig config;
    mutable std::mutex mutex;
    bool connected = false;
    std::string last_error;
    json server_info = json::object();
    json server_capabilities = json::object();
    std::string negotiated_protocol_version;
    json tools = json::array();

    std::unique_ptr<StdioProcess> process;
    std::atomic<int> next_request_id{1};

    std::mutex waiters_mutex;
    std::map<int, std::shared_ptr<Waiter>> waiters;

    void connect(const McpServerConfig& new_config) {
        std::lock_guard<std::mutex> lock(mutex);
        if (connected) return;
        config = new_config;
        last_error.clear();
        server_info = json::object();
        server_capabilities = json::object();
        negotiated_protocol_version.clear();
        tools = json::array();

        process = std::make_unique<StdioProcess>();
        try {
            process->start(
                config,
                [this](const std::string& line) { handle_stdout_line(line); },
                [this](const std::string& line) { handle_stderr_line(line); });

            json init = request(
                "initialize",
                json{{"protocolVersion", kProtocolVersion},
                     {"capabilities", json::object()},
                     {"clientInfo", json{{"name", "lemonade-mcp-client"},
                                           {"title", "Lemonade MCP Client Host"},
                                           {"version", "pr1"}}}},
                config.timeout_ms);
            if (init.contains("error")) {
                throw std::runtime_error("initialize failed: " + json_rpc_error_message(init));
            }
            const json result = init.value("result", json::object());
            negotiated_protocol_version = result.value("protocolVersion", std::string(kProtocolVersion));
            if (!supported_protocol_version(negotiated_protocol_version)) {
                throw std::runtime_error("MCP server negotiated unsupported protocol version: " +
                                         negotiated_protocol_version);
            }
            server_info = result.value("serverInfo", json::object());
            server_capabilities = result.value("capabilities", json::object());
            notify("notifications/initialized", json::object());
            connected = true;
            refresh_tools_locked();
        } catch (const std::exception& e) {
            last_error = e.what();
            connected = false;
            if (process) process->stop();
            process.reset();
            throw;
        }
    }

    void disconnect() {
        std::lock_guard<std::mutex> lock(mutex);
        connected = false;
        {
            std::lock_guard<std::mutex> wlock(waiters_mutex);
            for (auto& [_, waiter] : waiters) {
                std::lock_guard<std::mutex> lk(waiter->mutex);
                waiter->response = make_error("MCP server disconnected", 499);
                waiter->done = true;
                waiter->cv.notify_all();
            }
            waiters.clear();
        }
        if (process) {
            process->stop();
            process.reset();
        }
    }

    void refresh_tools() {
        std::lock_guard<std::mutex> lock(mutex);
        if (!connected) throw std::runtime_error("MCP server is not connected");
        refresh_tools_locked();
    }

    json call_tool(const std::string& name, const json& arguments, int timeout_ms) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!connected) throw std::runtime_error("MCP server is not connected");
        json params{{"name", name}, {"arguments", arguments.is_object() ? arguments : json::object()}};
        json response = request("tools/call", std::move(params), timeout_ms > 0 ? timeout_ms : config.timeout_ms);
        if (response.contains("error")) {
            throw std::runtime_error(json_rpc_error_message(response));
        }
        return response.value("result", json::object());
    }

    json snapshot(bool include_env_values = false) const {
        std::lock_guard<std::mutex> lock(mutex);
        json out = McpClientManager::config_to_json(config, include_env_values);
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
    json request(const std::string& method, json params, int timeout_ms) {
        int id = next_request_id.fetch_add(1, std::memory_order_relaxed);
        auto waiter = std::make_shared<Waiter>();
        {
            std::lock_guard<std::mutex> lock(waiters_mutex);
            waiters[id] = waiter;
        }

        json msg = json_rpc_request(id, method, std::move(params));
        if (!process || !process->write_line(msg.dump())) {
            std::lock_guard<std::mutex> lock(waiters_mutex);
            waiters.erase(id);
            throw std::runtime_error("Failed to write JSON-RPC request to MCP server");
        }

        const auto timeout = std::chrono::milliseconds(clamp_timeout_ms(timeout_ms));
        std::unique_lock<std::mutex> lock(waiter->mutex);
        if (!waiter->cv.wait_for(lock, timeout, [&] { return waiter->done; })) {
            {
                std::lock_guard<std::mutex> wlock(waiters_mutex);
                waiters.erase(id);
            }
            notify("notifications/cancelled", json{{"requestId", id}, {"reason", "timeout"}});
            throw std::runtime_error("MCP request timed out: " + method);
        }
        return waiter->response;
    }

    void notify(const std::string& method, json params) {
        if (!process) return;
        json msg = json_rpc_notification(method, std::move(params));
        process->write_line(msg.dump());
    }

    void refresh_tools_locked() {
        json collected = json::array();
        std::string cursor;
        for (int page = 0; page < kMaxToolListPages; ++page) {
            json params = json::object();
            if (!cursor.empty()) params["cursor"] = cursor;
            json response = request("tools/list", params, config.timeout_ms);
            if (response.contains("error")) {
                throw std::runtime_error("tools/list failed: " + json_rpc_error_message(response));
            }
            const json result = response.value("result", json::object());
            if (result.contains("tools") && result["tools"].is_array()) {
                for (const auto& tool : result["tools"]) {
                    if (tool.is_object() && tool.contains("name") && tool["name"].is_string()) {
                        collected.push_back(tool);
                    }
                }
            }
            cursor = result.value("nextCursor", std::string());
            if (cursor.empty()) break;
        }
        tools = std::move(collected);
    }

    void handle_stdout_line(const std::string& line) {
        json msg;
        try {
            msg = json::parse(line);
        } catch (const std::exception& e) {
            LOG(WARNING, "McpClient") << "Ignoring non-JSON MCP stdout from " << config.id
                                      << ": " << e.what() << std::endl;
            return;
        }

        if (msg.contains("id") && (msg.contains("result") || msg.contains("error"))) {
            int id = -1;
            if (msg["id"].is_number_integer()) {
                id = msg["id"].get<int>();
            } else if (msg["id"].is_string()) {
                try { id = std::stoi(msg["id"].get<std::string>()); } catch (...) { id = -1; }
            }
            if (id >= 0) {
                std::shared_ptr<Waiter> waiter;
                {
                    std::lock_guard<std::mutex> lock(waiters_mutex);
                    auto it = waiters.find(id);
                    if (it != waiters.end()) {
                        waiter = it->second;
                        waiters.erase(it);
                    }
                }
                if (waiter) {
                    std::lock_guard<std::mutex> lock(waiter->mutex);
                    waiter->response = std::move(msg);
                    waiter->done = true;
                    waiter->cv.notify_all();
                }
            }
            return;
        }

        // PR1 does not expose client-side capabilities such as sampling/roots.
        // Return a JSON-RPC method-not-found response for server-initiated requests
        // so a server does not hang forever waiting for a reply.
        if (msg.contains("method") && msg["method"].is_string() && msg.contains("id")) {
            const std::string method = msg["method"].get<std::string>();
            if (process) process->write_line(json_rpc_method_not_found(msg["id"], method).dump());
            return;
        }

        if (msg.contains("method") && msg["method"].is_string()) {
            LOG(DEBUG, "McpClient") << "MCP notification from " << config.id << ": "
                                    << msg["method"].get<std::string>() << std::endl;
        }
    }

    void handle_stderr_line(const std::string& line) {
        LOG(DEBUG, "McpClient") << "[" << config.id << " stderr] " << line << std::endl;
    }
};

McpClientManager::McpClientManager(std::string cache_dir) : cache_dir_(std::move(cache_dir)) {
    if (cache_dir_.empty()) cache_dir_ = ".";
    fs::path path = fs::path(cache_dir_) / kConfigFileName;
    config_path_ = path.string();
    load_config_file();
}

McpClientManager::~McpClientManager() {
    stop_all();
}

void McpClientManager::register_routes(httplib::Server& server) {
    auto self = shared_from_this();

    server.Get("/internal/mcp/servers", [self](const httplib::Request&, httplib::Response& res) {
        set_json(res, self->list_servers_json());
    });

    server.Get("/internal/mcp/tools", [self](const httplib::Request&, httplib::Response& res) {
        set_json(res, self->list_tools_json());
    });

    server.Post("/internal/mcp/servers", [self](const httplib::Request& req, httplib::Response& res) {
        json body;
        if (!parse_json_body(req, res, body)) return;
        try {
            set_json(res, self->upsert_server_json(body));
        } catch (const std::exception& e) {
            set_error(res, e.what(), 400);
        }
    });

    server.Delete(R"(/internal/mcp/servers/([A-Za-z0-9_.-]+))",
        [self](const httplib::Request& req, httplib::Response& res) {
            try {
                set_json(res, self->remove_server_json(req.matches[1].str()));
            } catch (const std::exception& e) {
                set_error(res, e.what(), 404);
            }
        });

    server.Post(R"(/internal/mcp/servers/([A-Za-z0-9_.-]+)/connect)",
        [self](const httplib::Request& req, httplib::Response& res) {
            try {
                set_json(res, self->connect_server_json(req.matches[1].str()));
            } catch (const std::exception& e) {
                set_error(res, e.what(), 500);
            }
        });

    server.Post(R"(/internal/mcp/servers/([A-Za-z0-9_.-]+)/disconnect)",
        [self](const httplib::Request& req, httplib::Response& res) {
            try {
                set_json(res, self->disconnect_server_json(req.matches[1].str()));
            } catch (const std::exception& e) {
                set_error(res, e.what(), 404);
            }
        });

    server.Post(R"(/internal/mcp/servers/([A-Za-z0-9_.-]+)/refresh-tools)",
        [self](const httplib::Request& req, httplib::Response& res) {
            try {
                set_json(res, self->refresh_tools_json(req.matches[1].str()));
            } catch (const std::exception& e) {
                set_error(res, e.what(), 500);
            }
        });

    server.Post(R"(/internal/mcp/servers/([A-Za-z0-9_.-]+)/tools/call)",
        [self](const httplib::Request& req, httplib::Response& res) {
            json body;
            if (!parse_json_body(req, res, body)) return;
            try {
                set_json(res, self->call_tool_json(req.matches[1].str(), body));
            } catch (const std::exception& e) {
                set_error(res, e.what(), 500);
            }
        });
}

void McpClientManager::stop_all() {
    std::map<std::string, std::shared_ptr<Runtime>> runtimes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        runtimes = runtimes_;
    }
    for (auto& [_, runtime] : runtimes) {
        if (runtime) runtime->disconnect();
    }
}

McpServerConfig McpClientManager::parse_server_config_json(const json& value,
                                                           bool allow_missing_id) {
    if (!value.is_object()) throw std::runtime_error("MCP server config must be an object");

    McpServerConfig cfg;
    cfg.id = trim(value.value("id", std::string()));
    cfg.name = trim(value.value("name", std::string()));
    cfg.transport = trim(value.value("transport", std::string("stdio")));
    cfg.command = trim(value.value("command", std::string()));
    cfg.working_dir = trim(value.value("working_dir", value.value("workingDir", std::string())));
    cfg.enabled = value.value("enabled", true);
    cfg.timeout_ms = clamp_timeout_ms(value.value("timeout_ms", value.value("timeoutMs", kDefaultTimeoutMs)));

    if (allow_missing_id && cfg.id.empty()) {
        cfg.id = sanitize_id_seed(!cfg.name.empty() ? cfg.name : basename_like(cfg.command));
    }
    if (!valid_id(cfg.id)) throw std::runtime_error("MCP server id must match [A-Za-z0-9_.-]+ and be at most 96 chars");
    if (cfg.name.empty()) cfg.name = cfg.id;
    if (has_control_char(cfg.name)) throw std::runtime_error("MCP server name must not contain control characters");
    if (cfg.transport != "stdio") throw std::runtime_error("PR1 supports only MCP stdio transport");
    if (cfg.command.empty()) throw std::runtime_error("MCP stdio server config requires command");
    if (has_control_char(cfg.command)) throw std::runtime_error("MCP command must not contain control characters");
    if (has_control_char(cfg.working_dir)) throw std::runtime_error("MCP working_dir must not contain control characters");

    if (value.contains("args")) {
        if (!value["args"].is_array()) throw std::runtime_error("MCP args must be an array of strings");
        for (const auto& arg : value["args"]) {
            if (!arg.is_string()) throw std::runtime_error("MCP args must be an array of strings");
            std::string s = arg.get<std::string>();
            if (s.find('\0') != std::string::npos) throw std::runtime_error("MCP args must not contain NUL");
            cfg.args.push_back(std::move(s));
        }
    }

    if (value.contains("env")) {
        if (!value["env"].is_object()) throw std::runtime_error("MCP env must be an object of string values");
        for (auto it = value["env"].begin(); it != value["env"].end(); ++it) {
            if (!valid_env_name(it.key())) throw std::runtime_error("Invalid MCP env var name: " + it.key());
            if (!it.value().is_string()) throw std::runtime_error("MCP env values must be strings");
            std::string v = it.value().get<std::string>();
            if (v.find('\0') != std::string::npos) throw std::runtime_error("MCP env values must not contain NUL");
            cfg.env[it.key()] = std::move(v);
        }
    }

    return cfg;
}

json McpClientManager::config_to_json(const McpServerConfig& config, bool include_env_values) {
    json env = json::object();
    for (const auto& [key, value] : config.env) {
        env[key] = include_env_values ? value : "***";
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

std::string McpClientManager::make_chat_tool_name(const std::string& server_id,
                                                  const std::string& tool_name) {
    auto clean = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '_' || c == '-') out.push_back(static_cast<char>(c));
            else out.push_back('_');
        }
        while (!out.empty() && out.front() == '_') out.erase(out.begin());
        while (!out.empty() && out.back() == '_') out.pop_back();
        if (out.empty()) out = "tool";
        return out;
    };
    const std::string raw = server_id + "\n" + tool_name;
    const std::string suffix = "_" + fnv1a_hex8(raw);
    std::string name = "mcp_" + clean(server_id) + "__" + clean(tool_name);
    if (name.size() > 64) {
        // OpenAI-compatible function names are commonly limited to 64 chars.
        // Preserve readability while keeping a stable hash suffix so two long
        // MCP tool names do not collapse to the same truncated function name.
        const size_t keep = 64 - suffix.size();
        name.resize(keep);
        while (!name.empty() && (name.back() == '_' || name.back() == '-')) name.pop_back();
        name += suffix;
    }
    return name.empty() ? "mcp_tool" : name;
}

json McpClientManager::list_servers_json() const {
    std::vector<std::pair<McpServerConfig, std::shared_ptr<Runtime>>> entries;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries.reserve(configs_.size());
        for (const auto& [id, cfg] : configs_) {
            std::shared_ptr<Runtime> runtime;
            auto it = runtimes_.find(id);
            if (it != runtimes_.end()) runtime = it->second;
            entries.push_back({cfg, runtime});
        }
    }

    json servers = json::array();
    for (const auto& [cfg, runtime] : entries) {
        if (runtime) {
            servers.push_back(runtime->snapshot(false));
        } else {
            json s = config_to_json(cfg, false);
            s["status"] = "disconnected";
            s["connected"] = false;
            s["last_error"] = "";
            s["tools"] = json::array();
            servers.push_back(std::move(s));
        }
    }
    return json{{"servers", servers}};
}

json McpClientManager::list_tools_json() const {
    std::vector<std::pair<McpServerConfig, std::shared_ptr<Runtime>>> entries;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries.reserve(runtimes_.size());
        for (const auto& [id, runtime] : runtimes_) {
            if (!runtime) continue;
            auto cfg_it = configs_.find(id);
            if (cfg_it == configs_.end()) continue;
            entries.push_back({cfg_it->second, runtime});
        }
    }

    json tools = json::array();
    for (const auto& [cfg, runtime] : entries) {
        json snap = runtime->snapshot(false);
        if (!snap.value("connected", false)) continue;
        for (const auto& tool : snap.value("tools", json::array())) {
            if (!tool.is_object() || !tool.contains("name") || !tool["name"].is_string()) continue;
            const std::string tool_name = tool["name"].get<std::string>();
            tools.push_back(json{{"server_id", cfg.id},
                                 {"server_name", cfg.name},
                                 {"name", tool_name},
                                 {"chat_name", make_chat_tool_name(cfg.id, tool_name)},
                                 {"title", tool.value("title", std::string())},
                                 {"description", tool.value("description", std::string())},
                                 {"inputSchema", tool.value("inputSchema", json::object())},
                                 {"tool", tool},
                                 {"openai_tool", openai_tool_from_mcp_tool(cfg, tool)}});
        }
    }
    return json{{"tools", tools}};
}

json McpClientManager::upsert_server_json(const json& body) {
    const json& raw = body.contains("server") && body["server"].is_object() ? body["server"] : body;
    McpServerConfig cfg = parse_server_config_json(raw, true);

    std::shared_ptr<Runtime> old_runtime;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (raw.value("id", std::string()).empty()) {
            cfg.id = next_id_locked(cfg.id);
        }
        configs_[cfg.id] = cfg;
        auto it = runtimes_.find(cfg.id);
        if (it != runtimes_.end()) {
            old_runtime = it->second;
            runtimes_.erase(it);
        }
        save_config_file_locked();
    }
    if (old_runtime) old_runtime->disconnect();

    return json{{"server", config_to_json(cfg, false)}};
}

json McpClientManager::remove_server_json(const std::string& id) {
    std::shared_ptr<Runtime> runtime;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!configs_.count(id)) throw std::runtime_error("Unknown MCP server: " + id);
        configs_.erase(id);
        auto it = runtimes_.find(id);
        if (it != runtimes_.end()) {
            runtime = it->second;
            runtimes_.erase(it);
        }
        save_config_file_locked();
    }
    if (runtime) runtime->disconnect();
    return json{{"removed", id}};
}

json McpClientManager::connect_server_json(const std::string& id) {
    McpServerConfig cfg = config_for_id(id);
    if (!cfg.enabled) throw std::runtime_error("MCP server is disabled: " + id);
    auto runtime = get_or_create_runtime(cfg);
    runtime->connect(cfg);
    return json{{"server", runtime->snapshot(false)}};
}

json McpClientManager::disconnect_server_json(const std::string& id) {
    std::shared_ptr<Runtime> runtime;
    McpServerConfig cfg;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto cfg_it = configs_.find(id);
        if (cfg_it == configs_.end()) throw std::runtime_error("Unknown MCP server: " + id);
        cfg = cfg_it->second;
        auto it = runtimes_.find(id);
        if (it != runtimes_.end()) runtime = it->second;
    }
    if (runtime) runtime->disconnect();
    json out = config_to_json(cfg, false);
    out["status"] = "disconnected";
    out["connected"] = false;
    out["tools"] = json::array();
    return json{{"server", out}};
}

json McpClientManager::refresh_tools_json(const std::string& id) {
    McpServerConfig cfg = config_for_id(id);
    if (!cfg.enabled) throw std::runtime_error("MCP server is disabled: " + id);
    auto runtime = get_or_create_runtime(cfg);
    runtime->connect(cfg);
    runtime->refresh_tools();
    return json{{"server", runtime->snapshot(false)}};
}

json McpClientManager::call_tool_json(const std::string& id, const json& body) {
    if (!body.contains("name") || !body["name"].is_string()) {
        throw std::runtime_error("tools/call body requires string field `name`");
    }
    McpServerConfig cfg = config_for_id(id);
    if (!cfg.enabled) throw std::runtime_error("MCP server is disabled: " + id);
    auto runtime = get_or_create_runtime(cfg);
    runtime->connect(cfg);

    const std::string name = body["name"].get<std::string>();
    json arguments = body.value("arguments", json::object());
    if (!arguments.is_object()) arguments = json::object();
    int timeout_ms = clamp_timeout_ms(body.value("timeout_ms", body.value("timeoutMs", cfg.timeout_ms)));
    json result = runtime->call_tool(name, arguments, timeout_ms);
    return json{{"server_id", id}, {"tool", name}, {"result", result}};
}

std::shared_ptr<McpClientManager::Runtime> McpClientManager::get_or_create_runtime(const McpServerConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = runtimes_.find(config.id);
    if (it != runtimes_.end() && it->second) return it->second;
    auto runtime = std::make_shared<Runtime>(config);
    runtimes_[config.id] = runtime;
    return runtime;
}

McpServerConfig McpClientManager::config_for_id(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = configs_.find(id);
    if (it == configs_.end()) throw std::runtime_error("Unknown MCP server: " + id);
    return it->second;
}

void McpClientManager::load_config_file() {
    std::lock_guard<std::mutex> lock(mutex_);
    configs_.clear();
    std::ifstream in(config_path_);
    if (!in.good()) return;
    try {
        json doc = json::parse(in);
        const json servers = doc.contains("servers") ? doc["servers"] : json::array();
        if (!servers.is_array()) return;
        for (const auto& entry : servers) {
            try {
                McpServerConfig cfg = parse_server_config_json(entry, false);
                configs_[cfg.id] = std::move(cfg);
            } catch (const std::exception& e) {
                LOG(WARNING, "McpClient") << "Skipping invalid MCP server config: " << e.what() << std::endl;
            }
        }
    } catch (const std::exception& e) {
        LOG(WARNING, "McpClient") << "Failed to read MCP config " << config_path_ << ": " << e.what() << std::endl;
    }
}

void McpClientManager::save_config_file_locked() const {
    fs::path path(config_path_);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) throw std::runtime_error("Failed to create MCP config directory: " + ec.message());

    json servers = json::array();
    for (const auto& [_, cfg] : configs_) servers.push_back(config_to_persisted_json(cfg));
    json doc{{"version", 1}, {"servers", servers}};

    std::ofstream out(config_path_, std::ios::trunc);
    if (!out.good()) throw std::runtime_error("Failed to open MCP config for writing: " + config_path_);
    out << std::setw(2) << doc << std::endl;
}

std::string McpClientManager::next_id_locked(const std::string& seed) const {
    std::string base = sanitize_id_seed(seed);
    std::string id = base;
    for (int i = 2; configs_.count(id); ++i) {
        id = base + "-" + std::to_string(i);
    }
    return id;
}

void register_mcp_client_routes(httplib::Server& server, const std::string& cache_dir) {
    static std::mutex managers_mutex;
    static std::map<std::string, std::weak_ptr<McpClientManager>> managers;

    std::shared_ptr<McpClientManager> manager;
    {
        std::lock_guard<std::mutex> lock(managers_mutex);
        auto it = managers.find(cache_dir);
        if (it != managers.end()) manager = it->second.lock();
        if (!manager) {
            manager = std::make_shared<McpClientManager>(cache_dir);
            managers[cache_dir] = manager;
        }
    }
    manager->register_routes(server);
}

}  // namespace lemon
