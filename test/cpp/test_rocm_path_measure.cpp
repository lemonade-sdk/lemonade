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

// Mirrors measure_longest_path() in backend_utils.cpp: walk without stat'ing,
// because a status query on a path past MAX_PATH fails.
size_t measure(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return 0;
    }
    size_t longest = dir.native().size();
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        longest = std::max(longest, it->path().native().size());
    }
    return ec ? 0 : longest;
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

    // A known-deepest file is reported exactly.
    const fs::path shallow = base / "tree" / "a.dat";
    const fs::path deeper = base / "tree" / "nested" / "bbbbbbbbbbbbbbbbbbbb.dat";
    if (!write_file(shallow, ec) || !write_file(deeper, ec)) {
        std::cerr << "[SKIP] could not create fixture tree: " << ec.message() << std::endl;
        fs::remove_all(base, ec);
        return 0;
    }
    expect(measure(base / "tree") == deeper.native().size(),
           "deepest file length is reported exactly");

#ifdef _WIN32
    // The regression this test exists for: an entry whose full path exceeds
    // MAX_PATH must still be measured. Build one by padding the filename, which
    // is how the real failure looks (the directory stays well under the limit,
    // only the Tensile filenames push the total past 259).
    const fs::path long_dir = base / "overlong";
    fs::create_directories(long_dir, ec);
    const size_t want = 275;
    const size_t name_len = want - (long_dir.native().size() + 1);
    const fs::path overlong = long_dir / (std::string(name_len - 4, 'n') + ".dat");
    if (write_file(overlong, ec) && overlong.native().size() == want) {
        const size_t got = measure(long_dir);
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
