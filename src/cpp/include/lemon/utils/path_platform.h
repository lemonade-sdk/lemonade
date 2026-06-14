#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <memory>

namespace lemon::utils {

namespace fs = std::filesystem;

// Abstract interface for platform-specific path operations
class PathPlatform {
public:
    virtual ~PathPlatform() = default;

    // Environment variable handling with UTF-8 encoding
    virtual std::string get_environment_variable_utf8(const std::string& name) = 0;

    // Path encoding conversion
    virtual fs::path path_from_utf8(const std::string& path) = 0;
    virtual std::string path_to_utf8(const fs::path& path) = 0;

    // Directory resolution
    virtual std::string get_executable_dir() = 0;
    virtual std::string get_cache_dir(const std::string& g_cache_dir) = 0;
    virtual std::string get_runtime_dir() = 0;

    // Platform-specific install prefixes for resource lookup
    virtual std::vector<std::string> get_install_prefixes() = 0;

    // Platform-specific HuggingFace cache directory default
    virtual std::string default_hf_cache_dir() = 0;
};

// Factory function to create platform-specific implementation
std::unique_ptr<PathPlatform> create_path_platform();

} // namespace lemon::utils
