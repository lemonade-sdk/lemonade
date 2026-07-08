// Unit tests for lemon::backends::BackendUtils::resolve_rocm_root() and its
// pure line-selection helper pick_rocm_root_candidates().
//
// The rocm-sdk and platform-default branches are host-dependent and can't be
// driven deterministically, so these tests exercise the ROCM_PATH branch with a
// temp dir containing a fake HIP runtime (amdhip64.dll on Windows,
// libamdhip64.so elsewhere) and otherwise assert only invariants that hold
// regardless of host state. pick_rocm_root_candidates is pure, so its line
// selection is tested directly.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <lemon/backends/backend_utils.h>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using lemon::backends::BackendUtils;

namespace {

int g_failures = 0;

// assert() is compiled out under NDEBUG (the default Release preset), so these
// tests use an explicit checker that records failures and drives the exit code,
// matching the sibling tests in this directory.
void check(bool cond, const char* msg) {
    if (cond) {
        std::cout << "[ok] " << msg << std::endl;
    } else {
        std::cerr << "[FAIL] " << msg << std::endl;
        ++g_failures;
    }
}

void set_rocm_path(const std::string& value) {
#ifdef _WIN32
    _putenv_s("ROCM_PATH", value.c_str());
#else
    setenv("ROCM_PATH", value.c_str(), /*overwrite=*/1);
#endif
}

void clear_rocm_path() {
#ifdef _WIN32
    _putenv("ROCM_PATH=");  // "name=" removes the variable on Windows
#else
    unsetenv("ROCM_PATH");
#endif
}

void write_stub(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << "stub";
}

// Write a fake HIP runtime where resolve_rocm_root probes. `primary` picks the
// first probed subdir, otherwise the fallback one.
void write_hip_runtime_stub(const fs::path& root, bool primary) {
#ifdef _WIN32
    write_stub(root / (primary ? "bin" : "lib") / "amdhip64.dll");
#else
    write_stub(root / (primary ? "lib" : "lib64") / "libamdhip64.so");
#endif
}

}  // namespace

int main() {
    fs::path tmp = fs::temp_directory_path() /
                   ("lemon_rocm_root_test_" + std::to_string(
#ifdef _WIN32
                        _getpid()
#else
                        getpid()
#endif
                        ));
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    const fs::path valid_root = tmp / "valid";
    write_hip_runtime_stub(valid_root, /*primary=*/true);

    const fs::path valid_root_alt = tmp / "valid_alt";
    write_hip_runtime_stub(valid_root_alt, /*primary=*/false);

    const fs::path invalid_root = tmp / "invalid";
    fs::create_directories(invalid_root / "lib");  // no HIP runtime

#ifdef _WIN32
    // ROCm 7.x version-suffixes the runtime (bin\amdhip64_7.dll) instead of the
    // plain bin\amdhip64.dll used by 5.x/6.x.
    const fs::path valid_root_versioned = tmp / "valid_versioned";
    write_stub(valid_root_versioned / "bin" / "amdhip64_7.dll");

    // A non-version suffix must not be mistaken for the HIP runtime.
    const fs::path bogus_suffix_root = tmp / "bogus_suffix";
    write_stub(bogus_suffix_root / "bin" / "amdhip64_backup.dll");
#endif

    // pick_rocm_root_candidates: pure selection of absolute-path lines from
    // `rocm-sdk path --root` output, whose stdout may be interleaved with the
    // child's stderr (warnings). No filesystem access.
    {
#ifdef _WIN32
        const std::string abs_path = "C:\\opt\\rocm";
        const std::string abs_path_crlf = "C:\\opt\\rocm\r";
#else
        const std::string abs_path = "/opt/rocm";
        const std::string abs_path_crlf = "/opt/rocm\r";
#endif
        const auto warn_then_path =
            BackendUtils::pick_rocm_root_candidates({"WARNING deprecated", abs_path});
        check(warn_then_path.size() == 1 && warn_then_path.front() == abs_path,
              "pick_rocm_root_candidates skips a leading stderr warning");

        const auto with_blanks =
            BackendUtils::pick_rocm_root_candidates({"", "   ", abs_path});
        check(with_blanks.size() == 1 && with_blanks.front() == abs_path,
              "pick_rocm_root_candidates skips blank lines");

        const auto none =
            BackendUtils::pick_rocm_root_candidates({"WARNING deprecated", "relative/dir"});
        check(none.empty(),
              "pick_rocm_root_candidates returns empty when no line is an absolute path");

        const auto crlf = BackendUtils::pick_rocm_root_candidates({abs_path_crlf});
        check(crlf.size() == 1 && crlf.front() == abs_path,
              "pick_rocm_root_candidates trims trailing CR");
    }

    {
        bool explicit_source = false;
        set_rocm_path(valid_root.string());
        auto root = BackendUtils::resolve_rocm_root(&explicit_source);
        check(root.has_value(), "ROCM_PATH (primary subdir) resolves");
        check(root.has_value() && fs::equivalent(*root, valid_root),
              "ROCM_PATH (primary subdir) resolves to the given root");
        check(explicit_source, "ROCM_PATH (primary subdir) is marked explicit");
    }

    {
        bool explicit_source = false;
        set_rocm_path(valid_root_alt.string());
        auto root = BackendUtils::resolve_rocm_root(&explicit_source);
        check(root.has_value(), "ROCM_PATH (fallback subdir) resolves");
        check(root.has_value() && fs::equivalent(*root, valid_root_alt),
              "ROCM_PATH (fallback subdir) resolves to the given root");
        check(explicit_source, "ROCM_PATH (fallback subdir) is marked explicit");
    }

#ifdef _WIN32
    {
        bool explicit_source = false;
        set_rocm_path(valid_root_versioned.string());
        auto root = BackendUtils::resolve_rocm_root(&explicit_source);
        check(root.has_value() && fs::equivalent(*root, valid_root_versioned),
              "ROCM_PATH with version-suffixed amdhip64_7.dll resolves");
        check(explicit_source,
              "ROCM_PATH with version-suffixed amdhip64_7.dll is marked explicit");
    }

    {
        set_rocm_path(bogus_suffix_root.string());
        std::error_code ec;
        auto root = BackendUtils::resolve_rocm_root(nullptr);
        check(!root.has_value() || !fs::equivalent(*root, bogus_suffix_root, ec),
              "ROCM_PATH with amdhip64_backup.dll does not resolve to itself");
    }
#endif

    // A ROCM_PATH missing the HIP runtime must fall through, never resolve to
    // itself, and never be reported as explicit.
    {
        bool explicit_source = false;
        set_rocm_path(invalid_root.string());
        auto root = BackendUtils::resolve_rocm_root(&explicit_source);
        check(!root.has_value() || !fs::equivalent(*root, invalid_root),
              "invalid ROCM_PATH does not resolve to itself");
        if (!root.has_value()) {
            check(!explicit_source, "invalid ROCM_PATH does not mark explicit");
        }
    }

    // A non-existent ROCM_PATH must fall through without the fs probes throwing.
    {
        bool explicit_source = false;
        const fs::path missing = tmp / "does-not-exist";
        set_rocm_path(missing.string());
        auto root = BackendUtils::resolve_rocm_root(&explicit_source);
        std::error_code ec;
        check(!root.has_value() || !fs::equivalent(*root, missing, ec),
              "non-existent ROCM_PATH falls through without throwing");
        if (!root.has_value()) {
            check(!explicit_source,
                  "non-existent ROCM_PATH does not mark explicit");
        }
    }

    {
        set_rocm_path(valid_root.string());
        auto root = BackendUtils::resolve_rocm_root(nullptr);
        check(root.has_value(), "nullptr resolved_explicitly is accepted");
    }

    // With no ROCM_PATH the result is host-dependent; only assert the no-ROCm
    // host resolves to nullopt and clears the explicit flag.
    {
        clear_rocm_path();
        bool explicit_source = true;  // sentinel; must be overwritten to false
        auto root = BackendUtils::resolve_rocm_root(&explicit_source);
        if (!root.has_value()) {
            check(!explicit_source,
                  "no ROCM_PATH and no host ROCm -> nullopt, not explicit");
        } else {
            std::cout << "[skip] host has ROCm at " << root->string()
                      << "; fallback branch not isolatable" << std::endl;
        }
    }

    clear_rocm_path();
    fs::remove_all(tmp);
    if (g_failures == 0) {
        std::cout << "All rocm_root_resolution tests passed" << std::endl;
    } else {
        std::cerr << g_failures << " rocm_root_resolution test(s) failed"
                  << std::endl;
    }
    return g_failures ? 1 : 0;
}
