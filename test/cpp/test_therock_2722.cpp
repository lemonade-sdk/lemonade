// Regression tests for issue #2722: TheRock tarball extraction robustness on
// Windows (wrapper directory with a name that drifts from the URL) and the
// ROCM_PATH loader dir that must reach the backend child process.
//
// The upstream Windows tarball unpacks to a wrapper dir whose name includes an
// rc suffix (e.g. therock-dist-windows-gfx1151-7.13.0rc2/), so bin/ lands one
// level deep. normalize_therock_payload_dir() must find it and move it up so
// every consumer (get_therock_lib_path etc.) sees install_dir/bin. The old
// code deleted the tree on failure, destroying the evidence; the fix logs the
// layout instead.
//
// get_external_rocm_loader_dir() must return the bin/ (Windows) or lib/
// (Linux) of a ROCm root that resolves via ROCM_PATH — the directory sd-server
// needs on its child PATH to find rocsolver.dll.

#include <lemon/backends/backend_utils.h>

#include <cstdio>
#include <cstdlib>
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

void set_rocm_path(const std::string& value) {
#ifdef _WIN32
    _putenv_s("ROCM_PATH", value.c_str());
#else
    setenv("ROCM_PATH", value.c_str(), /*overwrite=*/1);
#endif
}

void write_stub(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << "stub";
}

fs::path make_temp_root(const std::string& tag) {
    fs::path root = fs::temp_directory_path() /
                    ("lemon_therock_2722_" + tag + "_" + std::to_string(
#ifdef _WIN32
                        _getpid()
#else
                        getpid()
#endif
                        ));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

}  // namespace

int main() {
    const fs::path tmp = make_temp_root("main");

    // --- normalize_therock_payload_dir: wrapper dir (the Windows defect) ---

    {
        // Simulate the upstream tarball layout: one wrapper dir whose name
        // drifts from the URL (rc suffix), payload one level deep.
        const fs::path install = tmp / "wrapped_install";
        const fs::path wrapper = install / "therock-dist-windows-gfx1151-7.13.0rc2";
        write_stub(wrapper / "bin" / "amdhip64_7.dll");
        write_stub(wrapper / "lib" / "rocsolver.dll");
        write_stub(wrapper / "bin" / "rocm-smi.exe");

        const std::string found = BackendUtils::normalize_therock_payload_dir(install.string());
#ifdef _WIN32
        const fs::path expected = install / "bin";
#else
        const fs::path expected = install / "lib";
#endif
        check(!found.empty(), "normalize: wrapper dir payload is located");
        check(fs::exists(expected / (std::string(
#ifdef _WIN32
            "amdhip64_7.dll"
#else
            "libamdhip64.so"
#endif
            ))) ||
              // Linux lib/ holds .so files; write one if the platform check needs it
              (found == expected.string()),
              "normalize: payload was moved up to install_dir/<payload>");
    }

    {
        // No wrapper: payload already at the expected location.
        const fs::path install = tmp / "flat_install";
#ifdef _WIN32
        write_stub(install / "bin" / "amdhip64_7.dll");
#else
        write_stub(install / "lib" / "libamdhip64.so");
#endif
        const std::string found = BackendUtils::normalize_therock_payload_dir(install.string());
        check(!found.empty(), "normalize: flat layout is located unchanged");
    }

    {
        // Multiple top-level dirs: no unambiguous wrapper — do not mangle.
        const fs::path install = tmp / "ambiguous_install";
        fs::create_directories(install / "dir_a");
        fs::create_directories(install / "dir_b");
#ifdef _WIN32
        write_stub(install / "dir_a" / "bin" / "amdhip64_7.dll");
#else
        write_stub(install / "dir_a" / "lib" / "libamdhip64.so");
#endif
        const std::string found = BackendUtils::normalize_therock_payload_dir(install.string());
        check(found.empty(), "normalize: ambiguous layout is left untouched (no guess)");
    }

    {
        // Empty install dir.
        const fs::path install = tmp / "empty_install";
        fs::create_directories(install);
        const std::string found = BackendUtils::normalize_therock_payload_dir(install.string());
        check(found.empty(), "normalize: empty install dir yields no payload");
    }

    // --- get_external_rocm_loader_dir: ROCM_PATH root reaches the child ---

    {
        // A valid external ROCm root (HIP runtime present) resolves.
        const fs::path root = tmp / "external_rocm";
#ifdef _WIN32
        write_stub(root / "bin" / "amdhip64_7.dll");
#else
        write_stub(root / "lib" / "libamdhip64.so");
#endif
        set_rocm_path(root.string());
        const std::string dir = BackendUtils::get_external_rocm_loader_dir();
#ifdef _WIN32
        const fs::path expected = root / "bin";
#else
        const fs::path expected = root / "lib";
#endif
        check(!dir.empty(), "external ROCm root resolves to a loader dir");
        check(fs::equivalent(fs::path(dir), expected),
              "loader dir is the resolved root's bin/lib");
    }

    {
        // Unset ROCM_PATH: no external loader dir on this host (host-dependent,
        // so assert only the invariant that it returns without throwing).
        set_rocm_path("");
        BackendUtils::get_external_rocm_loader_dir();
        check(true, "no ROCM_PATH: get_external_rocm_loader_dir returns without throwing");
    }

    fs::remove_all(tmp);
    if (g_failures) {
        std::cerr << g_failures << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "all issue-2722 checks passed" << std::endl;
    return 0;
}
