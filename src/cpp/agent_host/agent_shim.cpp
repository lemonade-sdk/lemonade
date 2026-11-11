#include "lemon/agents/agent_shim.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include "lemon/utils/path_utils.h"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace lemon::agents {

namespace {

std::string read_head(const fs::path& file, size_t limit) {
    std::ifstream in(file, std::ios::binary);
    std::string data(limit, '\0');
    in.read(data.data(), static_cast<std::streamsize>(limit));
    data.resize(static_cast<size_t>(in.gcount()));
    return data;
}

std::string read_all(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string posix_quote(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    return out + "'";
}

bool is_executable_file(const fs::path& file) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        return false;
    }
#ifdef _WIN32
    return true;
#else
    return access(file.c_str(), X_OK) == 0;
#endif
}

} // namespace

fs::path shim_path(const AgentPaths& paths, const std::string& binary_name) {
#ifdef _WIN32
    return paths.bin_dir / (binary_name + ".cmd");
#else
    return paths.bin_dir / binary_name;
#endif
}

std::string render_posix_shim(const std::string& agent_id, const fs::path& target) {
    return "#!/bin/sh\n"
           "# " + std::string(kShimMarker) + " (agent=" + agent_id + ")\n"
           ": \"${PI_SKIP_VERSION_CHECK=1}\"; export PI_SKIP_VERSION_CHECK\n"
           ": \"${PI_SELF_UPDATE_COMMAND=lemonade agents install " + agent_id +
           "}\"; export PI_SELF_UPDATE_COMMAND\n"
           "exec " + posix_quote(lemon::utils::path_to_utf8(target)) + " \"$@\"\n";
}

std::string render_cmd_shim(const std::string& agent_id, const fs::path& target,
                            const std::vector<std::pair<std::string, std::string>>& env_dirs) {
    std::string path = lemon::utils::path_to_utf8(target);
    std::string prefix;
    for (const auto& [name, dir] : env_dirs) {
        if (!dir.empty() && path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 &&
            (path[dir.size()] == '\\' || path[dir.size()] == '/')) {
            prefix = "%" + name + "%";
            path = path.substr(dir.size());
            break;
        }
    }
    // cmd expands %VAR% even inside quotes, so a literal % is doubled.
    std::string quoted = prefix;
    for (char c : path) {
        quoted += c == '%' ? std::string("%%") : std::string(1, c);
    }
    return "@echo off\r\n"
           "rem " + std::string(kShimMarker) + " (agent=" + agent_id + ")\r\n"
           "setlocal\r\n"
           "if not defined PI_SKIP_VERSION_CHECK set \"PI_SKIP_VERSION_CHECK=1\"\r\n"
           "if not defined PI_SELF_UPDATE_COMMAND set \"PI_SELF_UPDATE_COMMAND=lemonade agents "
           "install " + agent_id + "\"\r\n"
           "\"" + quoted + "\" %*\r\n"
           "exit /b %ERRORLEVEL%\r\n";
}

std::string render_launch_agent_plist(const std::string& host_exe, const std::string& lemond_url) {
    std::string command = "set -a;";
    for (const char* file : {"lemond.conf", "agent-host.env"}) {
        const std::string path = "\"$HOME/.config/lemonade/" + std::string(file) + "\"";
        command += " [ -f " + path + " ] && . " + path + ";";
    }
    command += " set +a; exec " + posix_quote(host_exe) + " --persistent --lemond " +
               posix_quote(lemond_url);
    std::string escaped;
    for (char c : command) {
        switch (c) {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        default:
            escaped += c;
        }
    }
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\"><dict>\n"
           "  <key>Label</key><string>ai.lemonadeserver.agent-host</string>\n"
           "  <key>ProgramArguments</key><array><string>/bin/sh</string><string>-c</string>"
           "<string>" + escaped + "</string></array>\n"
           "  <key>RunAtLoad</key><true/>\n"
           "  <key>KeepAlive</key><true/>\n"
           "</dict></plist>\n";
}

std::string render_systemd_dropin(const std::string& host_exe, const std::string& lemond_url) {
    // systemd expands % specifiers and $ variables even inside quotes.
    auto quote = [](const std::string& value) {
        std::string out = "\"";
        for (char c : value) {
            if (c == '"' || c == '\\') {
                out += '\\';
            }
            out += c == '%' ? std::string("%%") : c == '$' ? std::string("$$") : std::string(1, c);
        }
        return out + "\"";
    };
    return "[Service]\n"
           "ExecStart=\n"
           "ExecStart=" + quote(host_exe) + " --persistent --lemond " + quote(lemond_url) + "\n";
}

bool is_lemonade_shim(const fs::path& file) {
    return read_head(file, 512).find(kShimMarker) != std::string::npos;
}

ShimState shim_state(const fs::path& file) {
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(file, ec))) {
        return ShimState::Absent;
    }
    return fs::is_regular_file(fs::symlink_status(file, ec)) && is_lemonade_shim(file)
               ? ShimState::Ours
               : ShimState::Foreign;
}

const char* shim_state_name(ShimState state) {
    switch (state) {
    case ShimState::Ours:
        return "ours";
    case ShimState::Foreign:
        return "foreign";
    case ShimState::Absent:
        break;
    }
    return "absent";
}

bool write_shim(const fs::path& file, const std::string& content) {
    const ShimState state = shim_state(file);
    if (state == ShimState::Foreign) {
        return false;
    }
    // Rewriting a shim that a running pi.cmd is reading fails on Windows.
    if (state == ShimState::Ours && read_all(file) == content) {
        return true;
    }

    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec) {
        throw std::runtime_error("Can't create " + lemon::utils::path_to_utf8(file.parent_path()) + ": " +
                                 ec.message());
    }
    fs::path tmp = file;
    tmp += ".lemonade-tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << content;
        out.close();
        if (!out) {
            fs::remove(tmp, ec);
            throw std::runtime_error("Can't write " + lemon::utils::path_to_utf8(tmp));
        }
    }
    fs::permissions(tmp,
                    fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                        fs::perms::others_read | fs::perms::others_exec,
                    ec);
    fs::rename(tmp, file, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        throw std::runtime_error("Can't write " + lemon::utils::path_to_utf8(file) + ": " + ec.message());
    }
    return true;
}

bool remove_shim(const fs::path& file, std::error_code* error) {
    if (shim_state(file) != ShimState::Ours) {
        return false;
    }
    std::error_code ec;
    const bool removed = fs::remove(file, ec);
    if (error != nullptr) {
        *error = ec;
    }
    return removed;
}

std::vector<fs::path> path_dirs(const std::string& path_value) {
#ifdef _WIN32
    const char separator = ';';
#else
    const char separator = ':';
#endif
    std::vector<fs::path> dirs;
    std::stringstream stream(path_value);
    for (std::string dir; std::getline(stream, dir, separator);) {
        // An empty entry means the current directory, which is never searched.
        const fs::path base = lemon::utils::path_from_utf8(dir);
        if (!dir.empty() && base.is_absolute()) {
            fs::path normal = base.lexically_normal();
            // "/a/b/" keeps an empty last element, which "/a/b" doesn't.
            if (!normal.has_filename() && normal != normal.root_path()) {
                normal = normal.parent_path();
            }
            dirs.push_back(normal);
        }
    }
    return dirs;
}

std::vector<fs::path> path_matches(const std::string& name, const std::string& path_value,
                                   const std::string& pathext) {
#ifdef _WIN32
    std::vector<std::string> extensions;
    std::stringstream ext_stream(pathext.empty() ? ".COM;.EXE;.BAT;.CMD" : pathext);
    for (std::string ext; std::getline(ext_stream, ext, ';');) {
        if (!ext.empty()) {
            extensions.push_back(ext);
        }
    }
#else
    const std::vector<std::string> extensions = {""};
    (void)pathext;
#endif
    std::vector<fs::path> matches;
    for (const fs::path& base : path_dirs(path_value)) {
        for (const auto& ext : extensions) {
            const fs::path candidate = base / (name + ext);
            if (is_executable_file(candidate)) {
                matches.push_back(candidate);
            }
        }
    }
    return matches;
}

} // namespace lemon::agents
