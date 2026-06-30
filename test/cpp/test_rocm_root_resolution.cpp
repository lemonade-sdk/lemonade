// Unit tests for lemon::backends::BackendUtils::resolve_rocm_root().
//
// The rocm-sdk and /opt/rocm branches are host-dependent and can't be driven
// deterministically, so these tests exercise the ROCM_PATH branch with a temp
// dir containing a fake libamdhip64.so and otherwise assert only invariants
// that hold regardless of host state.

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

    {
        bool explicit_source = false;
        set_rocm_path(valid_root.string());
        auto root = BackendUtils::resolve_rocm_root(&explicit_source);
        check(root.has_value(), "ROCM_PATH (lib/) resolves");
        check(root.has_value() && fs::equivalent(*root, valid_root),
              "ROCM_PATH (lib/) resolves to the given root");
        check(explicit_source, "ROCM_PATH (lib/) is marked explicit");
    }

    {
        bool explicit_source = false;
        set_rocm_path(valid_root_lib64.string());
        auto root = BackendUtils::resolve_rocm_root(&explicit_source);
        check(root.has_value(), "ROCM_PATH (lib64/) resolves");
        check(root.has_value() && fs::equivalent(*root, valid_root_lib64),
              "ROCM_PATH (lib64/) resolves to the given root");
        check(explicit_source, "ROCM_PATH (lib64/) is marked explicit");
    }

    // A ROCM_PATH missing libamdhip64.so must fall through, never resolve to
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
        set_rocm_path((tmp / "does-not-exist").string());
        auto root = BackendUtils::resolve_rocm_root(&explicit_source);
        check(!root.has_value() || !fs::equivalent(*root, tmp / "does-not-exist"),
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
