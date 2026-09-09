#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "lemon/backends/backend_descriptor.h"

namespace fs = std::filesystem;

// Forward declare DownloadProgressCallback to avoid heavy model_manager.h include
namespace lemon {
    struct DownloadProgress;
    using DownloadProgressCallback = std::function<bool(const DownloadProgress&)>;
}

namespace lemon::backends {
    struct InstallParams {
        std::string repo;      // GitHub "org/repo"
        std::string filename;  // Release asset filename
        std::string version_override;  // If set, use this as the release tag instead of backend_versions.json value
    };

    struct BackendSpec {
        const std::string recipe;
        const std::string binary;
        // True when the backend's GitHub release may publish the archive as
        // multiple parts ({base}.partNN-of-MM.tar.gz) alongside a tiny
        // {base}.partcount manifest. The installer probes the manifest only
        // when this is set, so non-split backends incur no extra HTTP
        // requests and no spurious 404 lines in install logs.
        const bool supports_split_archive;

        using InstallParamsFn = InstallParams(*)(const std::string& backend, const std::string& version);
        InstallParamsFn install_params_fn;  // nullptr for backends with no auto-install

        BackendSpec(std::string r, std::string b, InstallParamsFn fn = nullptr,
                    bool split = false)
            : recipe(std::move(r)), binary(std::move(b)),
              supports_split_archive(split), install_params_fn(fn) {}

        std::string log_name() const { return recipe + " Server"; };
    };

    // Build a backend's install/download spec from its descriptor's recipe/binary
    // and the server class T's get_install_params. The construct-on-first-use
    // static gives the registry a stable pointer.
    template <typename T>
    const BackendSpec* make_spec(const BackendDescriptor& d, bool split = false) {
        static const BackendSpec kSpec(d.recipe, d.binary, T::get_install_params, split);
        return &kSpec;
    }

    // Return the backend spec for recipes that use the standard BackendSpec flow.
    // Returns nullptr for recipes that require custom handling (e.g., flm) or unknown recipes.
    const BackendSpec* try_get_spec_for_recipe(const std::string& recipe);

    /**
    * Utility functions for backend management
    */
    class BackendUtils {
    public:
        /**
        * Extract ZIP files (Windows/Linux built-in tools)
        * @param zip_path Path to the ZIP file
        * @param dest_dir Destination directory to extract to
        * @return true if extraction was successful, false otherwise
        */
        static bool extract_zip(const std::string& zip_path, const std::string& dest_dir, const std::string& backend_name);

        /**
        * Extract tar.gz files (Linux/macOS/Windows)
        * @param tarball_path Path to the tar.gz file
        * @param dest_dir Destination directory to extract to
        * @return true if extraction was successful, false otherwise
        */
        static bool extract_tarball(const std::string& tarball_path, const std::string& dest_dir, const std::string& backend_name);

        /**
        * Extract 7z files (uses bsdtar/libarchive on Windows 11 22H2+ and Linux)
        * @param archive_path Path to the .7z file
        * @param dest_dir Destination directory to extract to
        * @return true if extraction was successful, false otherwise
        */
        static bool extract_seven_zip(const std::string& archive_path, const std::string& dest_dir, const std::string& backend_name);


        /**
        * Detect if archive is tar or zip
        * @param tarball_path Path to the archive file
        * @param dest_dir Destination directory to extract to
        * @return true if extraction was successful, false otherwise
        */
        static bool extract_archive(const std::string& archive_path, const std::string& dest_dir, const std::string& backend_name);

        /** Download and install the specified version of the backend from github.
         *  If progress_cb is provided, it receives download progress events instead of console output. */
        static void install_from_github(const BackendSpec& spec,
                                        const std::string& expected_version,
                                        const std::string& repo,
                                        const std::string& filename,
                                        const std::string& backend,
                                        DownloadProgressCallback progress_cb = nullptr);

        /** Get the latest version number for the given recipe/backend */
        static std::string get_backend_version(const std::string& recipe, const std::string& backend);

        /**
         * Resolve the ROCm install root, honoring an externally-installed ROCm
         * before the bundled default. Resolution order, returning the first root
         * that contains the HIP runtime (Windows: amdhip64.dll or
         * amdhip64_<version>.dll under bin\ or lib\; Linux:
         * lib{,64}/libamdhip64.so):
         *   1. ROCM_PATH environment variable
         *   2. `rocm-sdk path --root` (when rocm-sdk is on PATH)
         *   3. Platform default (Windows: HIP_PATH set by the AMD HIP SDK;
         *      Linux: /opt/rocm)
         * Returns std::nullopt when none validate. When resolved_explicitly is
         * non-null, it is set to true when the root came from ROCM_PATH or
         * rocm-sdk (a user-selected ROCm) and false for the platform default.
         */
        static std::optional<fs::path> resolve_rocm_root(bool* resolved_explicitly = nullptr);

        /**
         * Trim each line and keep those that name an absolute path, preserving
         * order. `rocm-sdk path --root`'s stdout can be interleaved with the
         * child's stderr (warnings), so the wanted path is not necessarily the
         * first line. Pure string logic; the caller validates each candidate.
         */
        static std::vector<std::string> pick_rocm_root_candidates(
            const std::vector<std::string>& lines);

        /**
         * Read the ROCm version string from a resolved install root, probing the
         * known version-file locations ({root}/.info/version,
         * {root}/share/rocm/version, {root}/version). Returns the trimmed first
         * line of the first file found, or "" when none exist.
         */
        static std::string read_rocm_version_from_root(const fs::path& root);

        /** Get TheRock installation directory for a specific architecture and version */
        static std::string get_therock_install_dir(const std::string& arch, const std::string& version);

        /** See backend_utils.cpp:install_rocm_runtime() for install method details */
        static void install_rocm_runtime(const std::string& arch, const std::string& version,
                                         DownloadProgressCallback progress_cb = nullptr);

        static bool install_therock_wheels(const std::string& arch, const std::string& version,
                                           DownloadProgressCallback progress_cb = nullptr);

        /** Directory holding the lemonade-managed ROCm wheel venv for arch/version. */
        static std::string get_therock_wheel_dir(const std::string& arch, const std::string& version);

        /** True when the pip-wheel runtime for arch/version is still usable. See impl for liveness check vs tarball fallback. */
        static bool therock_wheel_runtime_alive(const std::string& arch,
                                                const std::string& version);

        /** True for a concrete gfx target (e.g. gfx1151, gfx90a) that maps to a
         *  rocm-sdk-device wheel; false for family placeholders like gfx110X,
         *  which have no device wheel and fall back to the tarball. Exposed for
         *  unit testing. */
        static bool is_concrete_gfx_arch(const std::string& arch);

        /** Parse a pip "Downloading <name> (<size>)" progress line. On a match,
         *  sets filename to the wheel's basename and bytes_total to the parsed
         *  size (0 when the size is absent or unparseable) and returns true;
         *  returns false when the line is not a pip download line. pip prints
         *  units as kB/MB/GB (uppercase), so matching is case-insensitive.
         *  Exposed for unit testing. */
        static bool parse_pip_download_line(const std::string& line,
                                            std::string& filename,
                                            size_t& bytes_total);

        /** Download and install TheRock ROCm tarball for the specified architecture (Linux only) */
        static void install_therock(const std::string& arch, const std::string& version,
                                   DownloadProgressCallback progress_cb = nullptr);

        /** Clean up stale TheRock versions, keeping only the pinned ones */
        static void cleanup_old_therock_versions();

        /** Back-compat single-directory accessor. See get_therock_lib_paths() for why multi-dir is needed. */
        static std::string get_therock_lib_path(const std::string& rocm_arch);

        /** All TheRock runtime dirs (in loader-path order). The pip-wheel runtime splits HIP and BLAS libs across _rocm_sdk_core/bin and _rocm_sdk_libraries/bin (recorded in runtime_paths.txt); consumers need both on the loader path. */
        static std::vector<std::string> get_therock_lib_paths(const std::string& rocm_arch);

        /** Join runtime library directories into a single loader-path string
         *  (';' on Windows, ':' elsewhere), converting each to an absolute path.
         *  LLVM paths must already be included in dirs (see
         *  get_therock_lib_paths). Returns "" for empty input. */
        static std::string join_runtime_dirs(const std::vector<std::string>& dirs);

        /** Stage TheRock's amdhip64_7.dll next to a ROCm backend exe, unless
         *  System32 already ships a newer runtime. No-op off Windows. */
        static bool stage_therock_hip_runtime(const std::string& rocm_arch,
                                              const fs::path& target_dir);

        /** Read a DLL's VS_FIXEDFILEINFO version (packed MS<<32|LS). Windows
         *  only; returns 0 elsewhere. GetFileVersionInfoW returns stale data
         *  for system32 paths, so callers read versions from plain paths. */
        static uint64_t read_dll_version(const fs::path& dll);

        /** Get the path to the backend's binary. Gives precedence to the path set through environment variables, if set. Throws if not found. */
        static std::string get_backend_binary_path(const BackendSpec& spec, const std::string& backend);

        /** Get the path where the version indicator is installed. Does not check existence. */
        static std::string get_installed_version_file(const BackendSpec& spec, const std::string& backend);

        /** Get the install directory for the backend. Generally only used internally by BackendUtils */
        static std::string get_install_directory(const std::string& dir_name, const std::string& backend);

        /** Find the executable in the installation directory. Generally only used internally by BackendUtils */
        static std::string find_executable_in_install_dir(const std::string& install_dir, const std::string& binary_name);

        /** Checks the environment for a variable following the scheme LEMONADE_BACKEND_VARIANT_BIN and return its value, if available. Generally only used internally by BackendUtils */
        static std::string find_external_backend_binary(const std::string& recipe, const std::string& backend);

        /**
         * Returns the raw user-supplied *_bin value for this (recipe, backend),
         * e.g. "builtin" / "latest" / "b8664" / "/path/to/bin" / "". Empty string when
         * the environment override and config key are both unset. Environment takes
         * precedence over RuntimeConfig. Does not validate or resolve.
         */
        static std::string get_bin_config_value(const std::string& recipe, const std::string& backend);

        /**
         * Build the (config_section, bin_key) pair used to look up a *_bin value
         * for a (recipe, backend). Handles the rocm channel collapse and the
         * "server_bin" singleton. Used by the lookup, install, and validation paths.
         */
        static void build_bin_config_key(const std::string& recipe,
                                         const std::string& backend,
                                         std::string& out_section,
                                         std::string& out_bin_key);

        /**
         * Append shared CUDA environment variables to env_vars.
         *
         * Sets CUDA_VISIBLE_DEVICES to device indices matching the installed CUDA arch
         * (unless skip_visible_devices is true or the variable is already set in the
         * environment). On Linux, always sets __NV_PRIME_RENDER_OFFLOAD=1 so the NVIDIA
         * dGPU is activated on Optimus/PRIME systems in On-Demand mode.
         *
         * Call this for every CUDA backend subprocess launch. Pass
         * skip_visible_devices=true when the caller provides an explicit device
         * selection through a backend-specific mechanism (e.g. llama.cpp --device).
         */
        static void apply_cuda_env_vars(
            std::vector<std::pair<std::string, std::string>>& env_vars,
            const std::string& log_tag,
            bool skip_visible_devices = false);

        /**
         * Validates that the device selection string prefix matches the selected backend.
         * Throws std::invalid_argument if a device prefix contradicts the backend choice
         * (e.g. passing target_device "ROCm2" to a "vulkan" or "cuda" backend).
         */
        static void validate_device_backend_match(
            const std::string& backend,
            const std::string& target_device);
    };
} // namespace lemon::backends
