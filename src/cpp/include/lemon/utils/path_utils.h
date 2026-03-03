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
 * Find the FLM executable (flm.exe on Windows, flm on Unix).
 * Uses SearchPathA on Windows (same API as CreateProcessA) to search PATH,
 * then falls back to the default installation directory.
 * @return Full path to flm executable, or empty string if not found.
 */
std::string find_flm_executable();

/**
 * Run 'flm validate' command and check if it succeeds.
 * @param flm_path Optional path to flm executable. If empty, will search for it.
 * @param error_message Output parameter for error message if validation fails.
 * @return true if validation succeeds, false otherwise.
 */
bool run_flm_validate(const std::string& flm_path, std::string& error_message);

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
 * Get the cache directory
 */
std::string get_cache_dir();

/**
 * Returns a per-user runtime directory for lemonade's PID/lock/log files.
 * On Unix, uses $XDG_RUNTIME_DIR/lemonade when $XDG_RUNTIME_DIR is set,
 * exists, and is writable (creates the subdirectory if needed).
 * Falls back to /tmp when XDG_RUNTIME_DIR is unset or unusable (e.g. CI).
 * On Windows, returns the system temp directory.
 */
std::string get_runtime_dir();

/**
 * Get the directory where backend executables will be downloaded.
 * This is in the user's cache directory (~/.cache/lemonade/bin on all platforms)
 * to support All Users installations where the install directory may be read-only.
 */
std::string get_downloaded_bin_dir();

} // namespace utils
} // namespace lemon
