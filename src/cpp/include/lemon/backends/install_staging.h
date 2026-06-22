#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

// Header-only, dependency-light staging/atomic-swap helpers for backend
// installation. Kept free of the heavier backend_utils.cpp dependencies so the
// crash-safety invariant can be unit-tested in isolation (test/cpp/test_install_atomicity.cpp).
namespace lemon::backends {

    /**
     * Recursively search `dir` for a regular file named `binary_name` (or
     * `binary_name + ".exe"` on Windows). Returns the full path, or "" if not
     * found or `dir` does not exist.
     */
    inline std::string find_executable_in_dir(const std::string& dir,
                                              const std::string& binary_name) {
        namespace fs = std::filesystem;
        if (!fs::exists(dir)) {
            return "";
        }
#ifdef _WIN32
        const std::string binary_name_exe = binary_name + ".exe";
#endif
        for (const fs::directory_entry& dir_entry : fs::recursive_directory_iterator(dir)) {
            if (dir_entry.is_regular_file()) {
                const auto& fname = dir_entry.path().filename();
                if (fname == binary_name
#ifdef _WIN32
                    || fname == binary_name_exe
#endif
                ) {
                    return dir_entry.path().string();
                }
            }
        }
        return "";
    }

    /**
     * Atomically promote a fully-prepared staging directory to `install_dir`.
     *
     * Verifies that `staging_dir` contains `binary_name` before touching the
     * currently-installed copy. The caller MUST create `staging_dir` as a
     * sibling of `install_dir` so the renames stay on one filesystem.
     *
     * The promotion keeps a recoverable copy of the previous install at all
     * times so a failed swap can never lose both installs:
     *   1. move the existing install aside to `install_dir + ".old"`;
     *   2. rename `staging_dir` into `install_dir`;
     *   3. only then delete the `.old` backup.
     * If step 2 fails the `.old` backup is rolled back into place, restoring the
     * previously-working install.
     *
     * Outcomes:
     *   - returns the path to the installed executable on success;
     *   - returns "" when `staging_dir` does not contain `binary_name` (nothing
     *     was promoted; `staging_dir` is removed, `install_dir` is untouched);
     *   - throws std::runtime_error when the filesystem swap itself fails — the
     *     previous install is left in place (or rolled back), and `staging_dir`
     *     is left for the caller to clean up. This is distinct from the "" case
     *     so the caller can report an accurate error.
     */
    inline std::string commit_staged_install(const std::string& staging_dir,
                                             const std::string& install_dir,
                                             const std::string& binary_name) {
        namespace fs = std::filesystem;

        // Verify the freshly-staged tree actually contains the backend
        // executable before we touch the currently-installed copy.
        std::string staged_exe = find_executable_in_dir(staging_dir, binary_name);
        if (staged_exe.empty()) {
            std::error_code ec;
            fs::remove_all(staging_dir, ec);  // drop the bad staging tree; keep install_dir
            return "";
        }

        const std::string backup_dir = install_dir + ".old";
        std::error_code ec;
        fs::remove_all(backup_dir, ec);  // clear any stale backup from a prior aborted swap

        // Move the existing install aside (if any) so it survives a failed
        // promotion. We never remove it outright — it is renamed to .old and
        // only deleted once the new tree is verified in place.
        const bool had_install = fs::exists(install_dir);
        if (had_install) {
            fs::rename(install_dir, backup_dir, ec);
            if (ec) {
                // Could not move the existing install aside; leave it untouched.
                throw std::runtime_error(
                    "backend install swap failed: could not back up existing install at "
                    + install_dir + " (" + ec.message() + ")");
            }
        }

        // Promote the staged tree into place.
        fs::rename(staging_dir, install_dir, ec);
        if (ec) {
            // Promotion failed. Roll the backup back so the previously-working
            // install is restored rather than lost.
            if (had_install) {
                std::error_code rollback_ec;
                fs::rename(backup_dir, install_dir, rollback_ec);
            }
            throw std::runtime_error(
                "backend install swap failed: could not promote staged install to "
                + install_dir + " (" + ec.message() + ")");
        }

        // New install is in place; drop the backup.
        fs::remove_all(backup_dir, ec);

        return find_executable_in_dir(install_dir, binary_name);
    }

}  // namespace lemon::backends
