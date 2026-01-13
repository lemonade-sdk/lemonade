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
 * Get the directory where backend executables will be downloaded
 */
std::string get_downloaded_bin_dir();

#ifndef _WIN32
/** on Linux we changed the download location, the old location is used for cleanup only */
std::string get_deprecated_downloaded_bin_dir();
#endif

} // namespace utils
} // namespace lemon


