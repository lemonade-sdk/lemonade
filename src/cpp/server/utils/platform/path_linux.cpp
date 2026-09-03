#if defined(__linux__) && !defined(__ANDROID__)

#include <lemon/utils/path_platform.h>

#include <cstdlib>
#include <filesystem>
#include <limits.h>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace lemon::utils {

namespace fs = std::filesystem;

class LinuxPathPlatform : public PathPlatform {
public:
    std::string get_environment_variable_utf8(const std::string& name) override {
        const char* value = std::getenv(name.c_str());
        return value ? std::string(value) : "";
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
        throw std::runtime_error("Neither XDG_CACHE_HOME nor HOME is set; cannot resolve Lemonade cache directory");
    }

    std::string get_config_dir(const std::string& g_config_dir) override {
        if (!g_config_dir.empty()) {
            return g_config_dir;
        }

        // Systemd ConfigurationDirectory=lemonade exports the resolved path
        // (CONFIGURATION_DIRECTORY=/etc/lemonade), so use it directly.
        std::string config_dir = get_environment_variable_utf8("CONFIGURATION_DIRECTORY");
        if (!config_dir.empty()) {
            return config_dir;
        }

        std::string xdg_config_home = get_environment_variable_utf8("XDG_CONFIG_HOME");
        if (!xdg_config_home.empty()) {
            return xdg_config_home + "/lemonade";
        }

        std::string home = get_environment_variable_utf8("HOME");
        if (!home.empty()) {
            return home + "/.config/lemonade";
        }
        throw std::runtime_error("Neither XDG_CONFIG_HOME nor HOME is set; cannot resolve Lemonade config directory");
    }

    std::string get_legacy_cache_dir() override {
        std::string home = get_environment_variable_utf8("HOME");
        if (!home.empty()) {
            return home + "/.cache/lemonade";
        }
        return "";
    }

    std::string get_runtime_dir() override {
        // Systemd RuntimeDirectory=lemonade exports the resolved path
        // (RUNTIME_DIRECTORY=/run/lemonade or /run/user/1000/lemonade), so use it directly.
        std::string runtime_dir = get_environment_variable_utf8("RUNTIME_DIRECTORY");
        if (!runtime_dir.empty()) {
            return runtime_dir;
        }

        std::string xdg_runtime_dir = get_environment_variable_utf8("XDG_RUNTIME_DIR");
        if (!xdg_runtime_dir.empty()) {
            return xdg_runtime_dir + "/lemonade";
        }

        // Fallback to /tmp if neither systemd nor XDG_RUNTIME_DIR is available
        // (e.g. running under non-systemd container or unusual environment)
        std::string tmp_dir = "/tmp/lemonade";
        return tmp_dir;
    }

    std::vector<std::string> get_install_prefixes() override {
        std::vector<std::string> prefixes;

        // In Flatpak sandbox, container resources take precedence
        prefixes.push_back("/app/share/lemonade-server");

        std::string xdg_data_home = get_environment_variable_utf8("XDG_DATA_HOME");
        if (!xdg_data_home.empty()) {
            prefixes.push_back(xdg_data_home + "/lemonade-server");
        } else {
            std::string home = get_environment_variable_utf8("HOME");
            if (!home.empty()) {
                prefixes.push_back(home + "/.local/share/lemonade-server");
            }
        }

        std::string xdg_data_dirs = get_environment_variable_utf8("XDG_DATA_DIRS");
        if (!xdg_data_dirs.empty()) {
            std::istringstream ss(xdg_data_dirs);
            std::string dir;
            while (std::getline(ss, dir, ':')) {
                if (!dir.empty()) {
                    prefixes.push_back(dir + "/lemonade-server");
                }
            }
        } else {
            prefixes.push_back("/usr/local/share/lemonade-server");
            prefixes.push_back("/opt/share/lemonade-server");
            prefixes.push_back("/usr/share/lemonade-server");
        }

        return prefixes;
    }

    std::string default_hf_cache_dir() override {
        std::string hf_home = get_environment_variable_utf8("HF_HOME");
        if (!hf_home.empty()) {
            return hf_home + "/hub";
        }

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

#endif // defined(__linux__) && !defined(__ANDROID__)
