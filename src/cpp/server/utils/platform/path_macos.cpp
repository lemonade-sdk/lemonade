#include <lemon/utils/path_platform.h>
#include <unistd.h>
#include <limits.h>
#include <sys/types.h>
#include <pwd.h>
#include <mach-o/dyld.h>
#include <cstdlib>
#include <stdexcept>

namespace lemon::utils {

namespace fs = std::filesystem;

class MacOSPathPlatform : public PathPlatform {
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
        uint32_t size = sizeof(buffer);
        if (_NSGetExecutablePath(buffer, &size) == 0) {
            fs::path exe_path(buffer);
            return exe_path.parent_path().string();
        }
        throw std::runtime_error("Unable to resolve executable directory");
    }

    std::string get_cache_dir(const std::string& g_cache_dir) override {
        if (!g_cache_dir.empty()) {
            return g_cache_dir;
        }

        if (geteuid() != 0) {
            std::string home = get_environment_variable_utf8("HOME");
            if (!home.empty()) {
                std::string cache_dir = home + "/.cache/lemonade";
                fs::path cache_path = path_from_utf8(cache_dir);
                if (!fs::exists(cache_path)) {
                    fs::create_directories(cache_path);
                }
                return cache_dir;
            }
            struct passwd* pw = getpwuid(getuid());
            if (pw) {
                std::string cache_dir = std::string(pw->pw_dir) + "/.cache/lemonade";
                fs::path cache_path = path_from_utf8(cache_dir);
                if (!fs::exists(cache_path)) {
                    fs::create_directories(cache_path);
                }
                return cache_dir;
            }
        }

        {
            std::string cache_dir = "/Library/Application Support/lemonade/.cache";
            fs::path cache_path = path_from_utf8(cache_dir);
            if (!fs::exists(cache_path)) {
                fs::create_directories(cache_path);
            }
            return cache_dir;
        }
    }

    std::string get_runtime_dir() override {
        std::error_code ec;
        fs::path base = fs::temp_directory_path(ec);
        if (!ec && !base.empty()) {
            fs::path lemon_dir = base / "lemonade";
            ec.clear();
            fs::create_directory(lemon_dir, ec);
            if (!ec || fs::is_directory(lemon_dir)) {
                return lemon_dir.string();
            }
        }
        throw std::runtime_error("Unable to resolve writable runtime directory on macOS");
    }

    std::vector<std::string> get_install_prefixes() override {
        std::vector<std::string> prefixes = {
            "/Library/Application Support/Lemonade",
            "/usr/local/share/lemonade-server",
            "/opt/share/lemonade-server",
            "/usr/share/lemonade-server"
        };

        // Also check user's local install directory
        const char* home = std::getenv("HOME");
        if (home) {
            std::string user_local = std::string(home) + "/.local/share/lemonade-server";
            prefixes.insert(prefixes.begin(), user_local);
        }

        return prefixes;
    }

    std::string default_hf_cache_dir() override {
        std::string home = get_environment_variable_utf8("HOME");
        if (!home.empty()) {
            return home + "/.cache/huggingface/hub";
        }
        throw std::runtime_error("HOME is not set; cannot resolve HuggingFace cache directory");
    }
};

std::unique_ptr<PathPlatform> create_path_platform() {
    return std::make_unique<MacOSPathPlatform>();
}

} // namespace lemon::utils
