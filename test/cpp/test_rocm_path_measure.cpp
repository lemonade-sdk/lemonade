// Unit tests for the ROCm path-length measurement.
//
// rocBLAS access-violates rather than reporting a failed open when one of its
// Tensile solution files sits past Windows' 259-character limit, so the whole
// guard rests on the measurement seeing those files. The regression that
// matters is a status query on an over-long path failing and the entry being
// skipped, which would under-report exactly when it must not.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

#include <lemon/backends/backend_utils.h>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void expect(bool cond, const std::string& what) {
    if (cond) {
        std::cout << "[ok] " << what << std::endl;
    } else {
        std::cerr << "[FAIL] " << what << std::endl;
        ++g_failures;
    }
}

// Mirrors scan_rocblas_tensile_paths() in backend_utils.cpp: select by shape
// (TensileLibrary* under a "rocblas" directory) and walk without stat'ing,
// because a status query on a path past MAX_PATH fails.
bool is_rocblas_tensile_file(const fs::path& p) {
    if (p.filename().string().rfind("TensileLibrary", 0) != 0) {
        return false;
    }
    for (const fs::path& part : p.parent_path()) {
        if (part == "rocblas") {
            return true;
        }
    }
    return false;
}

size_t measure(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return 0;
    }
    size_t longest = 0;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (is_rocblas_tensile_file(it->path())) {
            longest = std::max(longest, it->path().native().size());
        }
    }
    return ec ? 0 : longest;
}

// Mirrors remove_rocm_tree() in backend_utils.cpp. MSVC's std::filesystem does not apply the
// long-path prefix, so plain remove_all() cannot delete the very trees this code exists to clean up.
bool remove_tree(const fs::path& dir, bool prefixed, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    if (prefixed) {
        fs::path target = fs::absolute(dir, ec);
        if (ec) {
            target = dir;
            ec.clear();
        }
        const std::wstring native = target.make_preferred().native();
        if (native.rfind(L"\\\\?\\", 0) != 0) {
            target = fs::path(L"\\\\?\\" + native);
        }
        fs::remove_all(target, ec);
        return !ec;
    }
#else
    (void)prefixed;
#endif
    fs::remove_all(dir, ec);
    return !ec;
}

bool write_file(const fs::path& p, std::error_code& ec) {
    fs::create_directories(p.parent_path(), ec);
    if (ec) {
        return false;
    }
#ifdef _WIN32
    // Long paths need the \\?\ prefix to be created at all; the point of the
    // test is that measurement still sees them afterwards.
    const std::wstring w = L"\\\\?\\" + p.native();
    std::ofstream f(w);
#else
    std::ofstream f(p);
#endif
    if (!f) {
        return false;
    }
    f << "x";
    return true;
}

}  // namespace

int main() {
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "lemon_rocm_path_measure";
    fs::remove_all(base, ec);

    // Missing directory measures as 0 rather than throwing.
    expect(measure(base / "does-not-exist") == 0, "missing directory measures 0");

    // Only rocBLAS Tensile files count.
    const fs::path roc = base / "tree" / "bin" / "rocblas" / "library" / "gfx1151";
    const fs::path hip = base / "tree" / "bin" / "hipblaslt" / "library" / "gfx1151";
    const fs::path roc_file = roc / "TensileLibrary_Type_fallback_gfx1151.dat";
    const fs::path hip_file = hip / "TensileLibrary_much_much_longer_name_gfx1151.dat";
    const fs::path other = roc / "README.txt";
    if (!write_file(roc_file, ec) || !write_file(hip_file, ec) || !write_file(other, ec)) {
        std::cerr << "[SKIP] could not create fixture tree: " << ec.message() << std::endl;
        fs::remove_all(base, ec);
        return 0;
    }
    expect(hip_file.native().size() > roc_file.native().size(),
           "fixture: the hipBLASLt path is the longer of the two");
    expect(measure(base / "tree") == roc_file.native().size(),
           "measures the rocBLAS file and ignores hipBLASLt and non-Tensile files");

#ifdef _WIN32
    // The regression this test exists for: an entry whose full path exceeds MAX_PATH must still be measured.
    // Build one by padding the filename, which is how the real failure looks.
    // The directory stays well under the limit, only the Tensile filenames push the total past 259.
    const fs::path long_dir = base / "overlong" / "rocblas" / "library";
    fs::create_directories(long_dir, ec);
    const size_t want = 275;
    const std::string stem = "TensileLibrary_";
    const size_t pad = want - (long_dir.native().size() + 1 + stem.size() + 4);
    const fs::path overlong = long_dir / (stem + std::string(pad, 'n') + ".dat");
    if (write_file(overlong, ec) && overlong.native().size() == want) {
        const size_t got = measure(base / "overlong");
        expect(got == want,
               "path past MAX_PATH is measured, not skipped (got " + std::to_string(got) +
                   ", wanted " + std::to_string(want) + ")");
        expect(got > 259, "the over-long path is correctly seen as a violation");

        // Why the walk must not stat: a status query on this path fails, so a
        // stat-gated loop would skip the entry and under-report.
        std::error_code stat_ec;
        const bool is_file = fs::is_regular_file(overlong, stat_ec);
        std::cout << "       [diag] is_regular_file -> " << is_file << " ec=" << stat_ec.value()
                  << " (" << stat_ec.message() << ")" << std::endl;

        // Deleting the tree needs the long-path prefix for the same reason.
        std::error_code plain_ec;
        remove_tree(long_dir, /*prefixed=*/false, plain_ec);
        const bool survived = fs::exists(long_dir, plain_ec);
        expect(survived, "plain remove_all cannot delete a tree with a path past MAX_PATH");

        std::error_code pfx_ec;
        remove_tree(long_dir, /*prefixed=*/true, pfx_ec);
        expect(!fs::exists(long_dir, pfx_ec),
               "prefixed remove_all deletes it");
    } else {
        std::cout << "[SKIP] filesystem refused a >259 char path; cannot test the regression"
                  << std::endl;
    }
#endif

    fs::remove_all(base, ec);

    if (g_failures > 0) {
        std::cerr << g_failures << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "All ROCm path measurement tests passed" << std::endl;
    return 0;
}
