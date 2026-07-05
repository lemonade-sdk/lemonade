#include <lemon/utils/path_platform.h>
#include <unistd.h>
#include <limits.h>
#include <cstdlib>
#include <stdexcept>

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

        std::string home = get_environment_variable_utf8("HOME");
        if (!home.empty()) {
            return home + "/.cache/lemonade";
        }
        throw std::runtime_error("HOME is not set; cannot resolve Lemonade cache directory");
    }

    std::string get_runtime_dir() override {
        const char* xdg = std::getenv("XDG_RUNTIME_DIR");
        if (xdg && xdg[0] != '\0') {
            std::error_code ec;
            fs::path base(xdg);
            if (fs::is_directory(base, ec) && !ec && access(xdg, W_OK) == 0) {
                fs::path lemon_dir = base / "lemonade";
                ec.clear();
                fs::create_directory(lemon_dir, ec);
                std::error_code ec2;
                if (!ec || fs::is_directory(lemon_dir, ec2)) {
                    return lemon_dir.string();
                }
            }
        }

        // System services can get a RuntimeDirectory= without XDG_RUNTIME_DIR.
        if (const char* runtime_dir = std::getenv("RUNTIME_DIRECTORY");
            runtime_dir && runtime_dir[0] != '\0') {
            std::error_code ec;
            fs::path base(runtime_dir);
            if (fs::is_directory(base, ec) && !ec && access(runtime_dir, W_OK | X_OK) == 0) {
                return base.string();
            }
        }

        throw std::runtime_error("Unable to resolve writable runtime directory from XDG_RUNTIME_DIR or RUNTIME_DIRECTORY");
    }

    std::vector<std::string> get_install_prefixes() override {
        std::vector<std::string> prefixes = {
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
    return std::make_unique<LinuxPathPlatform>();
}

} // namespace lemon::utils
