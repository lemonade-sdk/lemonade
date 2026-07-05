// Standalone test for lemon::utils::is_ggml_hip_plugin_available().
// Compile with: g++ -std=c++17 -I src/cpp/include \
//                   test/cpp/test_ggml_hip_path.cpp \
//                   src/cpp/server/utils/path_utils.cpp \
//                   src/cpp/server/utils/json_utils.cpp \
//                   src/cpp/server/utils/process_manager.cpp -o hip_path_test

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <lemon/utils/path_utils.h>

#ifdef __linux__
#include <unistd.h>
#endif

// Regression tests for the LEMONADE_GGML_HIP_PATH override consumed by
// lemon::utils::is_ggml_hip_plugin_available(). The override lets non-FHS
// installs (NixOS, custom prefixes) point directly at libggml-hip.so.
//
// The override is Linux-only, and because the path is user-controlled the
// implementation must (a) match a libggml-hip*.so* basename and (b) use the
// non-throwing std::filesystem overload so an odd path reports "unavailable"
// instead of raising filesystem_error.

namespace fs = std::filesystem;
using lemon::utils::is_ggml_hip_plugin_available;

namespace {

void set_hip_path(const std::string& value) {
    setenv("LEMONADE_GGML_HIP_PATH", value.c_str(), /*overwrite=*/1);
}

void clear_hip_path() { unsetenv("LEMONADE_GGML_HIP_PATH"); }

}  // namespace

int main() {
#ifndef __linux__
    // The override has no effect off Linux; the function always returns false.
    clear_hip_path();
    assert(!is_ggml_hip_plugin_available());
    std::cout << "[skip] LEMONADE_GGML_HIP_PATH override is Linux-only"
              << std::endl;
    return 0;
#else
    clear_hip_path();
    // If the host already has a system HIP plugin at an FHS path, the negative
    // cases below would still report "available" through the FHS fallback, so
    // those assertions can't isolate the override. Detect that and skip them.
    const bool baseline_available = is_ggml_hip_plugin_available();

    fs::path tmp =
        fs::temp_directory_path() / ("lemon_hip_test_" + std::to_string(getpid()));
    fs::create_directories(tmp);
    const auto write_stub = [](const fs::path& p) {
        std::ofstream(p) << "stub";
    };

    const fs::path valid_so = tmp / "libggml-hip.so";
    const fs::path versioned_so = tmp / "libggml-hip.so.0";
    const fs::path upper_so = tmp / "LIBGGML-HIP.SO";
    const fs::path wrong_name = tmp / "libsomethingelse.so";
    const fs::path not_a_lib = tmp / "notes.txt";
    write_stub(valid_so);
    write_stub(versioned_so);
    write_stub(upper_so);
    write_stub(wrong_name);
    write_stub(not_a_lib);

    set_hip_path(valid_so.string());
    assert(is_ggml_hip_plugin_available());
    std::cout << "[ok] valid libggml-hip.so override -> available" << std::endl;

    set_hip_path(versioned_so.string());
    assert(is_ggml_hip_plugin_available());
    std::cout << "[ok] versioned libggml-hip.so.0 override -> available"
              << std::endl;

    set_hip_path(upper_so.string());
    assert(is_ggml_hip_plugin_available());
    std::cout << "[ok] case-insensitive basename override -> available"
              << std::endl;

    if (!baseline_available) {
        set_hip_path((tmp / "missing" / "libggml-hip.so").string());
        assert(!is_ggml_hip_plugin_available());
        std::cout << "[ok] missing override path -> not available" << std::endl;

        set_hip_path(tmp.string());
        assert(!is_ggml_hip_plugin_available());
        std::cout << "[ok] directory override -> not available" << std::endl;

        set_hip_path(wrong_name.string());
        assert(!is_ggml_hip_plugin_available());
        std::cout << "[ok] wrong-name file override -> not available"
                  << std::endl;

        set_hip_path(not_a_lib.string());
        assert(!is_ggml_hip_plugin_available());
        std::cout << "[ok] non-.so file override -> not available" << std::endl;

        // Using an existing file as a path component yields ENOTDIR, which the
        // throwing is_regular_file() overload would turn into a filesystem_error
        // (crashing this test). The non-throwing overload must just report
        // "not available". The basename still matches, isolating the behavior.
        set_hip_path((valid_so / "libggml-hip.so").string());
        assert(!is_ggml_hip_plugin_available());
        std::cout << "[ok] non-directory path component -> not available "
                     "(no throw)"
                  << std::endl;
    } else {
        std::cout << "[skip] host has a system HIP plugin; negative override "
                     "cases are not distinguishable"
                  << std::endl;
    }

    clear_hip_path();
    fs::remove_all(tmp);
    std::cout << "All ggml_hip_path tests passed" << std::endl;
    return 0;
#endif
}
