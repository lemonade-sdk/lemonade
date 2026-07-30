#pragma once

#include <filesystem>
#include <string>

namespace lemon {
namespace utils {

/**
 * Get the directory where the executable is located.
 * This allows us to find resources relative to the executable,
 * regardless of the current working directory.
 */
std::string get_executable_dir();

/**
 * Get the path to a resource file relative to the executable directory.
 * @param relative_path Path relative to executable (e.g., "resources/server_models.json")
 */
std::string get_resource_path(const std::string& relative_path);

/**
 * Validate that a path is safe to embed in a shell command.
 * Rejects paths containing shell metacharacters that could enable command injection.
 * Only allows: alphanumeric, path separators, dots, hyphens, underscores,
 * spaces, colons (drive letters), tildes (short names), and parentheses.
 */
bool is_safe_executable_path(const std::string& path);

/**
 * Heuristic: does this string look like a filesystem path (vs. a keyword or
 * version tag)? Returns true if it contains a path separator or is absolute.
 * Used to disambiguate `*_bin` config values like "b8664" (version tag) from
 * "/opt/llama" (path). Existence is NOT checked here.
 */
bool looks_like_path(const std::string& v);

/**
 * Get an environment variable as UTF-8 text.
 */
std::string get_environment_variable_utf8(const std::string& name);

/**
 * Convert a UTF-8 path string to a std::filesystem::path.
 */
std::filesystem::path path_from_utf8(const std::string& path);

/**
 * Convert a std::filesystem::path to a UTF-8 string.
 */
std::string path_to_utf8(const std::filesystem::path& path);

/**
 * Finds an executable in the system's PATH.
 * @param executable_name The name of the executable to find (e.g., "llama-server", "python").
 * @return Full path to the executable, or empty string if not found.
 */
std::string find_executable_in_path(const std::string& executable_name);

/**
 * Set the lemonade cache directory. Must be called once at startup before
 * get_cache_dir(). After this call, get_cache_dir() returns this path.
 */
void set_cache_dir(const std::string& dir);

/**
 * Get the lemonade cache directory.
 * Returns the path set by set_cache_dir(), or falls back to
 * platform-specific defaults if set_cache_dir() was never called.
 */
std::string get_cache_dir();

/**
 * Set the models directory for HuggingFace model cache.
 * Must be called at startup with the value from config.json.
 */
void set_models_dir(const std::string& dir);

/**
 * Get the platform-specific default HuggingFace cache directory.
 * (~/.cache/huggingface/hub on Linux/macOS, %USERPROFILE%\.cache\huggingface\hub on Windows)
 */
std::string default_hf_cache_dir();

/**
 * Get the models directory (HuggingFace cache).
 * Returns the path set by set_models_dir(), or falls back to
 * default_hf_cache_dir().
 */
std::string get_hf_cache_dir();

/**
 * Returns a per-user runtime directory for lemonade's PID/lock/log files.
 * On macOS, uses the system temporary directory and creates a lemonade
 * subdirectory.
 * On Unix, uses $XDG_RUNTIME_DIR/lemonade when $XDG_RUNTIME_DIR is set,
 * exists, and is writable (creates the subdirectory if needed); otherwise
 * throws.
 * On Windows, returns the system temp directory.
 */
std::string get_runtime_dir();

/**
 * Get the directory where backend executables will be downloaded.
 * This is in the user's cache directory (~/.cache/lemonade/bin on all platforms)
 * to support All Users installations where the install directory may be read-only.
 */
std::string get_downloaded_bin_dir();

bool atomic_replace_file(const std::filesystem::path& src,
                         const std::filesystem::path& dest,
                         std::error_code& ec);

} // namespace utils
} // namespace lemon
