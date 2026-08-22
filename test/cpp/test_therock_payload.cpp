// Regression tests for the TheRock installed-check (issue #2413, PR #2454).
//
// Guards two behaviors the fix must keep:
//   1. A complete TheRock install (version.txt matching AND a concrete HIP
//      runtime payload present) counts as installed — so installing a second
//      ROCm backend does not re-download the ~3 GB tarball.
//   2. An incomplete install (version.txt present but the runtime payload
//      missing/corrupt) counts as NOT installed — so install_therock falls
//      through to re-download, repairing the broken install instead of
//      silently skipping it (the update_required repair path from #2003).

#include <lemon/backends/backend_utils.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using lemon::backends::BackendUtils;

namespace {

int g_failures = 0;

void check(bool cond, const char* msg) {
    if (cond) {
        std::cout << "[ok] " << msg << std::endl;
    } else {
        std::cerr << "[FAIL] " << msg << std::endl;
        ++g_failures;
    }
}

void set_cache_dir(const std::string& value) {
#ifdef _WIN32
    _putenv_s("LEMONADE_CACHE_DIR", value.c_str());
#else
    setenv("LEMONADE_CACHE_DIR", value.c_str(), /*overwrite=*/1);
#endif
}

void write_stub(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << "stub";
}

// TheRock install dir layout: <cache>/bin/therock/<arch>-<version>/ with
// version.txt plus lib/ (Linux) or bin/ (Windows) holding the HIP runtime.
fs::path make_therock_install(const fs::path& tmp, const std::string& arch,
                              const std::string& version, bool with_payload) {
    const fs::path dir = tmp / "bin" / "therock" / (arch + "-" + version);
    {
        fs::create_directories(dir);
        std::ofstream vf(dir / "version.txt");
        vf << version;
    }
    if (with_payload) {
#ifdef _WIN32
        write_stub(dir / "bin" / "amdhip64.dll");
#else
        write_stub(dir / "lib" / "libamdhip64.so");
#endif
    } else {
        // Skeleton only — the lib/ or bin/ directory exists but the actual
        // HIP runtime file is missing (incomplete/corrupt install).
#ifdef _WIN32
        fs::create_directories(dir / "bin");
#else
        fs::create_directories(dir / "lib");
#endif
    }
    return dir;
}

}  // namespace

int main() {
    fs::path tmp = fs::temp_directory_path() /
                   ("lemon_therock_payload_test_" + std::to_string(
#ifdef _WIN32
                        _getpid()
#else
                        getpid()
#endif
                        ));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    set_cache_dir(tmp.string());

    const std::string arch = "gfx1100";
    const std::string version = "6.4.1";

    // --- has_hip_runtime_payload: the shared "is it really installed" gate ---

    {
        const fs::path complete = make_therock_install(tmp, arch, version, /*with_payload=*/true);
        check(BackendUtils::has_hip_runtime_payload(complete.string()),
              "complete install (version.txt + HIP payload) counts as installed");
    }
    {
        const fs::path incomplete = make_therock_install(tmp, arch, "broken", /*with_payload=*/false);
        check(!BackendUtils::has_hip_runtime_payload(incomplete.string()),
              "incomplete install (dir skeleton, no HIP payload) is NOT installed");
    }
    {
        const fs::path empty_dir = tmp / "bin" / "therock" / "empty";
        fs::create_directories(empty_dir);
        check(!BackendUtils::has_hip_runtime_payload(empty_dir.string()),
              "empty install dir is NOT installed");
    }
#ifdef _WIN32
    {
        // ROCm 7.x version-suffixes the runtime (bin\amdhip64_7.dll); a bogus
        // suffix (amdhip64_backup.dll) must not be mistaken for the runtime.
        const fs::path versioned = tmp / "bin" / "therock" / "v7";
        write_stub(versioned / "bin" / "amdhip64_7.dll");
        check(BackendUtils::has_hip_runtime_payload(versioned.string()),
              "version-suffixed HIP runtime (amdhip64_7.dll) counts as installed");
        const fs::path bogus = tmp / "bin" / "therock" / "bogus";
        write_stub(bogus / "bin" / "amdhip64_backup.dll");
        check(!BackendUtils::has_hip_runtime_payload(bogus.string()),
              "bogus suffix (amdhip64_backup.dll) is NOT a HIP runtime");
    }
#endif

    // --- install_therock early-return: no download when already installed ---

    {
        // A progress callback that fails the test if it is ever invoked — the
        // download path reports progress, the early-return path does not.
        bool download_started = false;
        auto cb = [&download_started](const lemon::DownloadProgress&) {
            download_started = true;
            return true;
        };
        make_therock_install(tmp, arch, version, /*with_payload=*/true);
        // If the payload gate regresses, install_therock tries to download
        // (and reports progress / throws) instead of returning immediately.
        BackendUtils::install_therock(arch, version, cb);
        check(!download_started,
              "install_therock with a complete install returns without downloading");
    }

    fs::remove_all(tmp);
    if (g_failures) {
        std::cerr << g_failures << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "all therock payload checks passed" << std::endl;
    return 0;
}
