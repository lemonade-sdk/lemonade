#pragma once

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
 * Get the cache directory
 */
std::string get_cache_dir();

/**
 * Get the directory where backend executables will be downloaded.
 * This is in the user's cache directory (~/.cache/lemonade/bin on all platforms)
 * to support All Users installations where the install directory may be read-only.
 */
std::string get_downloaded_bin_dir();

} // namespace utils
} // namespace lemon
