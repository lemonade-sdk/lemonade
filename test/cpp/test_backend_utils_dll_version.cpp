// Unit tests for BackendUtils::read_dll_version().
//
// GetFileVersionInfoW returns stale cached data for system32 paths (it
// reported 6.2 for a file that actually contains 10.0). stage_therock_hip_runtime
// works around this by staging System32's amdhip64_7.dll into the backend exe
// directory first and reading the version from that plain path, where the API
// is reliable. These tests verify read_dll_version returns the same value as
// the direct API for a plain path, and that it degrades gracefully.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <lemon/backends/backend_utils.h>

#ifdef _WIN32
#include <windows.h>
#include <winver.h>
#endif

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

#ifdef _WIN32
// Reference implementation: direct GetFileVersionInfoW read of a plain path.
uint64_t read_version_direct(const fs::path& path) {
    const std::wstring wpath = path.wstring();
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(wpath.c_str(), &handle);
    if (size == 0) {
        return 0;
    }
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(wpath.c_str(), handle, size, data.data())) {
        return 0;
    }
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) || ffi == nullptr) {
        return 0;
    }
    return (static_cast<uint64_t>(ffi->dwFileVersionMS) << 32) | ffi->dwFileVersionLS;
}
#endif

}  // namespace

int main() {
#ifdef _WIN32
    // Test 1: read_dll_version on a plain path matches a direct API read.
    {
        const fs::path sys32 = fs::path("C:\\Windows\\System32\\kernel32.dll");
        if (!fs::exists(sys32)) {
            check(false, "kernel32.dll exists on the test host");
            return 1;
        }

        // A plain-path copy is what stage_therock_hip_runtime reads versions
        // from; the API must agree with read_dll_version there.
        const fs::path copy = fs::temp_directory_path() / "lemonade_test_kernel32_copy.dll";
        std::error_code ec;
        fs::copy_file(sys32, copy, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            check(false, "kernel32.dll can be copied to a temp path");
            return 1;
        }

        const uint64_t via_helper = BackendUtils::read_dll_version(copy);
        const uint64_t direct = read_version_direct(copy);
        check(via_helper != 0, "read_dll_version returns a non-zero version for kernel32.dll");
        check(via_helper == direct,
              "read_dll_version matches the direct API on a plain path");

        fs::remove(copy, ec);
    }

    // Test 2: the System32 kernel32.dll itself must also read through the
    // helper (the API may cache system32 paths, but kernel32 is a stable
    // reference file that read_dll_version should still handle).
    {
        const fs::path sys32 = fs::path("C:\\Windows\\System32\\kernel32.dll");
        if (fs::exists(sys32)) {
            const uint64_t v = BackendUtils::read_dll_version(sys32);
            check(v != 0, "read_dll_version reads kernel32.dll from System32");
        }
    }

    // Test 3: missing file yields 0.
    {
        const fs::path missing = fs::temp_directory_path() / "lemonade_does_not_exist.dll";
        fs::remove(missing);
        const uint64_t v = BackendUtils::read_dll_version(missing);
        check(v == 0, "read_dll_version returns 0 for a missing file");
    }
#else
    // Off Windows the helper is a no-op stub.
    {
        const fs::path missing = fs::temp_directory_path() / "lemonade_does_not_exist.dll";
        const uint64_t v = BackendUtils::read_dll_version(missing);
        check(v == 0, "read_dll_version returns 0 on non-Windows platforms");
    }
#endif

    if (g_failures > 0) {
        std::cerr << "Total failures: " << g_failures << std::endl;
        return 1;
    }

    std::cout << "All DLL version reading tests passed!" << std::endl;
    return 0;
}
