// Unit tests for lemon::backends::BackendUtils::resolve_rocm_root().
//
// resolve_rocm_root() lets Lemonade reuse an externally-installed ROCm instead
// of downloading its own TheRock runtime. It resolves the install root in
// priority order: ROCM_PATH -> `rocm-sdk path --root` -> /opt/rocm, returning
// the first directory that ships lib{,64}/libamdhip64.so.
//
// These tests drive the ROCM_PATH and fallback paths with temp dirs containing
// a fake libamdhip64.so. They cannot deterministically exercise the rocm-sdk
// or /opt/rocm branches (host-dependent), so they only assert invariants that
// hold regardless of host state.

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
    write_stub(valid_root / "lib" / "libamdhip64.so");

    const fs::path valid_root_lib64 = tmp / "valid64";
    write_stub(valid_root_lib64 / "lib64" / "libamdhip64.so");

    const fs::path invalid_root = tmp / "invalid";
    fs::create_directories(invalid_root / "lib");  // no libamdhip64.so

    // Case 1: ROCM_PATH points at a valid root (lib/) -> resolves it, explicit.
    {
        bool explicit_source = false;
        set_rocm_path(valid_root.string());
        auto root = BackendUtils::resolve_rocm_root(&explicit_source);
        check(root.has_value(), "ROCM_PATH (lib/) resolves");
        check(root.has_value() && fs::equivalent(*root, valid_root),
              "ROCM_PATH (lib/) resolves to the given root");
        check(explicit_source, "ROCM_PATH (lib/) is marked explicit");
    }

    // Case 2: ROCM_PATH points at a valid root (lib64/) -> resolves it.
    {
        bool explicit_source = false;
        set_rocm_path(valid_root_lib64.string());
        auto root = BackendUtils::resolve_rocm_root(&explicit_source);
        check(root.has_value(), "ROCM_PATH (lib64/) resolves");
        check(root.has_value() && fs::equivalent(*root, valid_root_lib64),
              "ROCM_PATH (lib64/) resolves to the given root");
        check(explicit_source, "ROCM_PATH (lib64/) is marked explicit");
    }

    // Case 3: ROCM_PATH set but missing libamdhip64.so -> must NOT return that
    // path; falls through to rocm-sdk / /opt/rocm (host-dependent, may be none).
    {
        bool explicit_source = false;
        set_rocm_path(invalid_root.string());
        auto root = BackendUtils::resolve_rocm_root(&explicit_source);
        check(!root.has_value() || !fs::equivalent(*root, invalid_root),
              "invalid ROCM_PATH does not resolve to itself");
        // The invalid path itself must never be reported as an explicit match.
        if (!root.has_value()) {
            check(!explicit_source, "invalid ROCM_PATH does not mark explicit");
        }
    }

    // Case 4: ROCM_PATH set to a non-existent path -> same fall-through, no
    // throw from the non-throwing filesystem probes.
    {
        bool explicit_source = false;
        set_rocm_path((tmp / "does-not-exist").string());
        auto root = BackendUtils::resolve_rocm_root(&explicit_source);
        check(!root.has_value() || !fs::equivalent(*root, tmp / "does-not-exist"),
              "non-existent ROCM_PATH falls through without throwing");
        if (!root.has_value()) {
            check(!explicit_source,
                  "non-existent ROCM_PATH does not mark explicit");
        }
    }

    // Case 5: nullptr out-param is accepted.
    {
        set_rocm_path(valid_root.string());
        auto root = BackendUtils::resolve_rocm_root(nullptr);
        check(root.has_value(), "nullptr resolved_explicitly is accepted");
    }

    // Case 6: no ROCM_PATH and (assuming) no rocm-sdk/opt-rocm on this host ->
    // resolver is well-behaved (returns a value only if the host genuinely has
    // ROCm at /opt/rocm or via rocm-sdk). We only assert it does not throw and,
    // if it resolves, the result is not explicit unless rocm-sdk supplied it.
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
