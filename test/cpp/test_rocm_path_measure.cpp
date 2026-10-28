// Tests for the ROCm path-length guard, exercised through the BackendUtils entry points.

// rocBLAS access-violates rather than reporting a failed open when one of its Tensile solution
// files sits past Windows' 259-character limit, so the guard has to be right about three things:
// (a) it must see files whose paths exceed the limit,
// (b) it must not report a pass when it could not read the tree, and
// (c) the figure it records must survive the cache directory moving.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>

#include <lemon/backends/backend_utils.h>
#include <lemon/error_types.h>
#include <lemon/utils/path_utils.h>

namespace fs = std::filesystem;
using lemon::backends::BackendUtils;

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

// Fixture scaffolding only -- the product code under test is not reimplemented.
// Long paths need the Win32 prefix just to be created.
fs::path with_long_prefix(const fs::path& p) {
#ifdef _WIN32
    std::error_code ec;
    fs::path absolute = fs::absolute(p, ec);
    if (ec) {
        absolute = p;
    }
    absolute.make_preferred();
    const std::wstring native = absolute.native();
    if (native.rfind(L"\\\\?\\", 0) == 0) {
        return absolute;
    }
    return fs::path(L"\\\\?\\" + native);
#else
    return p;
#endif
}

bool write_file(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(with_long_prefix(p.parent_path()), ec);
    if (ec) {
        return false;
    }
#ifdef _WIN32
    std::ofstream f(with_long_prefix(p).native());
#else
    std::ofstream f(p);
#endif
    if (!f) {
        return false;
    }
    f << "x";
    return true;
}

// <cache>/bin/therock/<arch>-<version>/bin/rocblas/library/gfx1151/
fs::path rocblas_dir(const fs::path& cache, const std::string& arch, const std::string& version) {
    return cache / "bin" / "therock" / (arch + "-" + version) / "bin" / "rocblas" / "library" /
           "gfx1151";
}

bool refuses(const std::string& arch, const std::string& version, std::string& msg) {
    try {
        BackendUtils::ensure_rocm_tensile_reachable(arch, version, /*wheel_layout=*/false);
    } catch (const lemon::ConfigurationException& e) {
        msg = e.what();
        return true;
    }
    msg.clear();
    return false;
}

}  // namespace

int main() {
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "lemon_rocm_guard";
    fs::remove_all(with_long_prefix(base), ec);

    const std::string arch = "gfx1151";
    const std::string tensile = "TensileLibrary_Type_fallback_gfx1151.dat";

    // Nothing installed: nothing to measure, and nothing to refuse.
    {
        const fs::path cache = base / "empty";
        fs::create_directories(cache, ec);
        lemon::utils::set_cache_dir(cache.string());
        const auto depth = BackendUtils::measure_rocblas_max_path(arch, "0.0.1", false);
        expect(depth.has_value() && *depth == 0, "no ROCm install measures as 0");
        std::string msg;
        expect(!refuses(arch, "0.0.1", msg), "no ROCm install does not refuse a load");
    }

    // A short cache dir with a normal tree: measured, and well under the limit.
    size_t short_depth = 0;
    {
        const fs::path cache = base / "s";
        // hipBLASLt ships TensileLibrary* files too, deeper, and tolerates an
        // unopenable one. Including it would refuse cache dirs rocBLAS is fine
        // with, so it must not drive the measurement.
        const fs::path hip = cache / "bin" / "therock" / (arch + "-1.0.0") / "bin" / "hipblaslt" /
                             "library" / "gfx1151" / "TensileLibrary_a_much_much_longer_name.dat";
        if (!write_file(rocblas_dir(cache, arch, "1.0.0") / tensile) || !write_file(hip)) {
            std::cerr << "[SKIP] could not build fixture" << std::endl;
            return 0;
        }
        lemon::utils::set_cache_dir(cache.string());
        const auto depth = BackendUtils::measure_rocblas_max_path(arch, "1.0.0", false);
        expect(depth.has_value() && *depth > 0, "a normal tree is measured");
        short_depth = depth.value_or(0);

        std::string msg;
        expect(!refuses(arch, "1.0.0", msg), "a short cache dir is accepted");
    }

    // The same tree under a deliberately long cache dir. The depth must be
    // identical -- that is what makes the recorded figure survive the cache
    // directory moving -- and the load must now be refused.
    {
        const fs::path cache = base / std::string(180, 'd');
        if (!write_file(rocblas_dir(cache, arch, "2.0.0") / tensile)) {
            std::cerr << "[SKIP] could not build long fixture" << std::endl;
        } else {
            lemon::utils::set_cache_dir(cache.string());
            const auto depth = BackendUtils::measure_rocblas_max_path(arch, "2.0.0", false);
            expect(depth.has_value() && *depth == short_depth,
                   "depth is relative to the install dir, so it is unchanged under a long "
                   "cache dir");

            std::string msg;
            const bool refused = refuses(arch, "2.0.0", msg);
#ifdef _WIN32
            expect(refused, "an over-long cache dir is refused");
            expect(msg.find("0 or fewer") == std::string::npos,
                   "the refusal quotes a real limit, not a clamped zero");
#else
            expect(!refused, "MAX_PATH is not enforced off Windows");
#endif
        }
    }

#ifdef _WIN32
    // A directory inside the tree is itself past MAX_PATH. Without the Win32
    // long-path prefix the walk aborts here, and reporting that as 0 would be
    // read as a pass -- this case must never be permitted.
    {
        const fs::path cache = base / "deep";
        fs::path dir = rocblas_dir(cache, arch, "3.0.0");
        while (dir.native().size() < 300) {
            dir /= std::string(40, 'n');
        }
        if (!write_file(dir / tensile)) {
            std::cerr << "[SKIP] filesystem refused a >260 char directory" << std::endl;
        } else {
            lemon::utils::set_cache_dir(cache.string());
            const auto depth = BackendUtils::measure_rocblas_max_path(arch, "3.0.0", false);
            expect(depth.has_value(), "a tree containing a directory past MAX_PATH is still read");
            expect(depth.value_or(0) > 219, "the over-long entry is measured, not skipped");

            std::string msg;
            expect(refuses(arch, "3.0.0", msg),
                   "a tree past MAX_PATH is refused, never silently passed");
        }
    }
#endif

    fs::remove_all(with_long_prefix(base), ec);

    if (g_failures > 0) {
        std::cerr << g_failures << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "All ROCm path guard tests passed" << std::endl;
    return 0;
}
