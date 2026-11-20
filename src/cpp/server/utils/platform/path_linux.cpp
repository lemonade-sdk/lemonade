#include <lemon/utils/path_platform.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <limits.h>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace lemon::utils {

namespace fs = std::filesystem;

class LinuxPathPlatform : public PathPlatform {
public:
    std::string get_environment_variable_utf8(const std::string& name) override {
        const char* value = std::getenv(name.c_str());
        return value ? std::string(value) : "";
    }

    void set_environment_variable_utf8(const std::string& name,
                                       const std::string& value) override {
        if (setenv(name.c_str(), value.c_str(), 1) != 0) {
            throw std::runtime_error("Failed to set environment variable: " + name);
        }
    }

    void unset_environment_variable_utf8(const std::string& name) override {
        if (unsetenv(name.c_str()) != 0) {
            throw std::runtime_error("Failed to unset environment variable: " + name);
        }
    }


    fs::path path_from_utf8(const std::string& path) override {
        return fs::path(path);
    }

    std::string path_to_utf8(const fs::path& path) override {
        return path.string();
    }

    std::string get_executable_dir() override {
        char buffer[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (len != -1) {
            buffer[len] = '\0';
            fs::path exe_path(buffer);
            return exe_path.parent_path().string();
        }
        throw std::runtime_error("Unable to resolve executable directory");
    }

    std::string get_cache_dir(const std::string& g_cache_dir) override {
        if (!g_cache_dir.empty()) {
            return g_cache_dir;
        }

        // Systemd CacheDirectory=lemonade already exports the resolved path
        // (CACHE_DIRECTORY=/var/cache/lemonade), so use it directly.
        std::string cache_dir = get_environment_variable_utf8("CACHE_DIRECTORY");
        if (!cache_dir.empty()) {
            return cache_dir;
        }

        std::string xdg_cache_home = get_environment_variable_utf8("XDG_CACHE_HOME");
        if (!xdg_cache_home.empty()) {
            return xdg_cache_home + "/lemonade";
        }

        std::string home = get_environment_variable_utf8("HOME");
        if (!home.empty()) {
            return home + "/.cache/lemonade";
        }
        throw std::runtime_error("HOME is not set; cannot resolve Lemonade cache directory");
    }

    std::string get_config_dir(const std::string& g_config_dir) override {
        if (!g_config_dir.empty()) {
            return g_config_dir;
        }

        // Checked before HOME to avoid nested /var/lib/lemonade/.config/lemonade
        // when systemd sets both STATE_DIRECTORY and HOME to /var/lib/lemonade.
        std::string state_dir = get_environment_variable_utf8("STATE_DIRECTORY");
        if (!state_dir.empty()) {
            return state_dir;
        }

        std::string xdg_config_home = get_environment_variable_utf8("XDG_CONFIG_HOME");
        if (!xdg_config_home.empty()) {
            return xdg_config_home + "/lemonade";
        }

        std::string home = get_environment_variable_utf8("HOME");
        if (!home.empty()) {
            return home + "/.config/lemonade";
        }
        throw std::runtime_error("HOME is not set; cannot resolve Lemonade config directory");
    }

    std::string get_legacy_cache_dir() override {
        std::string home = get_environment_variable_utf8("HOME");
        if (!home.empty()) {
            return home + "/.cache/lemonade";
        }
        return "";
    }

    std::string get_runtime_dir() override {
        auto is_usable = [](const fs::path& dir, int mode) {
            std::error_code ec;
            return fs::is_directory(dir, ec) && !ec && access(dir.c_str(), mode) == 0;
        };

        // systemd RuntimeDirectory=lemonade exports the resolved path.
        std::string runtime_dir = get_environment_variable_utf8("RUNTIME_DIRECTORY");
        if (!runtime_dir.empty() && is_usable(runtime_dir, W_OK | X_OK)) {
            return runtime_dir;
        }

        std::string xdg_runtime_dir = get_environment_variable_utf8("XDG_RUNTIME_DIR");
        if (!xdg_runtime_dir.empty() && is_usable(xdg_runtime_dir, W_OK)) {
            fs::path lemon_dir = fs::path(xdg_runtime_dir) / "lemonade";
            std::error_code ec;
            fs::create_directory(lemon_dir, ec);
            std::error_code ec2;
            if ((!ec || fs::is_directory(lemon_dir, ec2)) && is_usable(lemon_dir, W_OK | X_OK)) {
                return lemon_dir.string();
            }
        }

        // No session runtime dir (containers, tarball runs). Per-user so locks
        // and logs are not shared; mkdir/lstat rejects a pre-created or
        // symlinked path owned by someone else.
        fs::path fallback = fs::path("/tmp") / ("lemonade-" + std::to_string(getuid()));
        if (::mkdir(fallback.c_str(), 0700) != 0 && errno != EEXIST) {
            throw std::runtime_error("Unable to create runtime directory " + fallback.string());
        }
        struct stat st {};
        if (::lstat(fallback.c_str(), &st) == 0 && S_ISDIR(st.st_mode) &&
            st.st_uid == getuid() && (st.st_mode & (S_IWGRP | S_IWOTH)) == 0 &&
            is_usable(fallback, W_OK | X_OK)) {
            return fallback.string();
        }

        throw std::runtime_error("Unable to resolve writable runtime directory");
    }

    std::vector<std::string> get_install_prefixes() override {
        std::vector<std::string> prefixes;

        auto add_prefix = [&prefixes](const std::string& prefix) {
            if (std::find(prefixes.begin(), prefixes.end(), prefix) == prefixes.end()) {
                prefixes.push_back(prefix);
            }
        };

        // Flatpak resources live under /app.
        add_prefix("/app/share/lemonade-server");

        std::string xdg_data_home = get_environment_variable_utf8("XDG_DATA_HOME");
        if (!xdg_data_home.empty()) {
            add_prefix(xdg_data_home + "/lemonade-server");
        } else {
            std::string home = get_environment_variable_utf8("HOME");
            if (!home.empty()) {
                add_prefix(home + "/.local/share/lemonade-server");
            }
        }

        std::string xdg_data_dirs = get_environment_variable_utf8("XDG_DATA_DIRS");
        if (!xdg_data_dirs.empty()) {
            std::istringstream ss(xdg_data_dirs);
            std::string dir;
            while (std::getline(ss, dir, ':')) {
                if (!dir.empty()) {
                    add_prefix(dir + "/lemonade-server");
                }
            }
        }

        // Always searched: XDG_DATA_DIRS may omit the standard system paths.
        add_prefix("/usr/local/share/lemonade-server");
        add_prefix("/opt/share/lemonade-server");
        add_prefix("/usr/share/lemonade-server");

        return prefixes;
    }

    std::string default_hf_cache_dir() override {
        std::string xdg_cache = get_environment_variable_utf8("XDG_CACHE_HOME");
        if (!xdg_cache.empty()) {
            return xdg_cache + "/huggingface/hub";
        }

        std::string home = get_environment_variable_utf8("HOME");
        if (!home.empty()) {
            return home + "/.cache/huggingface/hub";
        }
        throw std::runtime_error("Neither XDG_CACHE_HOME nor HOME is set; cannot resolve HuggingFace cache directory");
    }
};

std::unique_ptr<PathPlatform> create_path_platform() {
    return std::make_unique<LinuxPathPlatform>();
}

} // namespace lemon::utils
