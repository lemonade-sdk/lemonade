// Standalone test for lemon::backends::commit_staged_install.
//
// Guards the crash-safety invariant for backend installation: a failed or
// incomplete install (interrupted download, truncated/garbage archive, wrong
// contents) must NEVER destroy a previously-working binary. The fix stages the
// new install in a sibling directory and only swaps it into place once the
// executable is verified present; before the fix, install_from_github removed
// the working install_dir up front, so a failed download left no usable binary.
//
// Compile with:
//   g++ -std=c++17 -I src/cpp/include test/cpp/test_install_atomicity.cpp -o install_atomicity_test
//   cl /std:c++17 /EHsc /I src/cpp/include test/cpp/test_install_atomicity.cpp

#include "lemon/backends/install_staging.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using lemon::backends::commit_staged_install;

struct TestResult {
    int passed = 0;
    int failed = 0;

    void check(bool cond, const std::string& name) {
        if (cond) {
            printf("[PASS] %s\n", name.c_str());
            ++passed;
        } else {
            printf("[FAIL] %s\n", name.c_str());
            ++failed;
        }
    }
};

// Unique temp directory for one sub-test, so runs don't collide.
static fs::path make_temp_root(const std::string& tag) {
    static int counter = 0;
    fs::path root = fs::temp_directory_path() / ("install_atomicity_test_" + tag + "_");
    root += std::to_string(std::hash<std::string>{}(
        std::to_string(std::time(nullptr)) + tag + std::to_string(counter++)));
    fs::create_directories(root);
    return root;
}

static void write_file(const fs::path& p, const std::string& contents) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << contents;
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s;
}

// 1) Happy path: a valid staging tree replaces the old install atomically.
static void test_successful_swap(TestResult& r) {
    fs::path root = make_temp_root("ok");
    fs::path install_dir = root / "llamacpp";
    fs::path staging_dir = root / "llamacpp.staging";  // sibling, as the installer creates it
    const std::string binary = "llama-server";

    // Old working install with a sentinel "version".
    write_file(install_dir / "bin" / binary, "OLD");
    write_file(install_dir / "version.txt", "v1");

    // New staged install (executable found recursively under bin/).
    write_file(staging_dir / "bin" / binary, "NEW");
    write_file(staging_dir / "version.txt", "v2");

    std::string exe = commit_staged_install(staging_dir.string(), install_dir.string(), binary);

    r.check(!exe.empty(), "successful swap returns installed exe path");
    r.check(fs::exists(install_dir / "bin" / binary), "installed executable present after swap");
    r.check(read_file(install_dir / "bin" / binary) == "NEW", "installed binary is the NEW one");
    r.check(read_file(install_dir / "version.txt") == "v2", "version.txt is the staged one");
    r.check(!fs::exists(staging_dir), "staging dir consumed by the rename");
    r.check(!fs::exists(install_dir.string() + ".old"), "no .old backup left after a successful swap");

    fs::remove_all(root);
}

// 2) REGRESSION GUARD: a staging tree without the executable must NOT destroy
//    the existing working install.
static void test_failed_install_preserves_working_binary(TestResult& r) {
    fs::path root = make_temp_root("fail");
    fs::path install_dir = root / "llamacpp";
    fs::path staging_dir = root / "llamacpp.staging";
    const std::string binary = "llama-server";

    // Old working install.
    write_file(install_dir / "bin" / binary, "OLD");
    write_file(install_dir / "version.txt", "v1");

    // Botched staging: extracted something, but the expected executable is
    // missing (e.g. truncated/garbage archive, wrong asset).
    write_file(staging_dir / "README.txt", "partial");

    std::string exe = commit_staged_install(staging_dir.string(), install_dir.string(), binary);

    r.check(exe.empty(), "failed verification returns empty");
    r.check(fs::exists(install_dir / "bin" / binary), "OLD working binary still present");
    r.check(read_file(install_dir / "bin" / binary) == "OLD", "OLD binary contents intact");
    r.check(read_file(install_dir / "version.txt") == "v1", "OLD version.txt intact");
    r.check(!fs::exists(staging_dir), "bad staging dir cleaned up");

    fs::remove_all(root);
}

// 3) Fresh install: no prior install_dir exists.
static void test_fresh_install(TestResult& r) {
    fs::path root = make_temp_root("fresh");
    fs::path install_dir = root / "llamacpp";
    fs::path staging_dir = root / "llamacpp.staging";
    const std::string binary = "llama-server";

    write_file(staging_dir / "bin" / binary, "NEW");

    std::string exe = commit_staged_install(staging_dir.string(), install_dir.string(), binary);

    r.check(!exe.empty(), "fresh install returns exe path");
    r.check(fs::exists(install_dir / "bin" / binary), "fresh install places executable");
    r.check(!fs::exists(staging_dir), "staging consumed on fresh install");

    fs::remove_all(root);
}

// 4) REGRESSION GUARD (POSIX): if the filesystem swap itself fails, the working
//    install must survive and the failure must be reported (thrown), not
//    silently swallowed. We force the backup rename to fail by making the parent
//    directory read-only, which blocks creating install_dir + ".old".
//    Windows permission semantics differ, so this is gated to POSIX.
#ifndef _WIN32
static void test_swap_failure_preserves_working_binary(TestResult& r) {
    fs::path root = make_temp_root("swapfail");
    fs::path install_dir = root / "llamacpp";
    fs::path staging_dir = root / "llamacpp.staging";
    const std::string binary = "llama-server";

    write_file(install_dir / "bin" / binary, "OLD");
    write_file(staging_dir / "bin" / binary, "NEW");

    // Make the parent read-only so the .old backup rename fails.
    fs::permissions(root, fs::perms::owner_read | fs::perms::owner_exec,
                    fs::perm_options::replace);

    bool threw = false;
    std::string exe;
    try {
        exe = commit_staged_install(staging_dir.string(), install_dir.string(), binary);
    } catch (const std::exception&) {
        threw = true;
    }

    // Restore write permission so we can inspect / clean up.
    fs::permissions(root, fs::perms::owner_all, fs::perm_options::replace);

    r.check(threw, "swap failure throws (distinct from the verify-fail empty return)");
    r.check(fs::exists(install_dir / "bin" / binary), "OLD working binary survives a failed swap");
    r.check(read_file(install_dir / "bin" / binary) == "OLD", "OLD binary contents intact after failed swap");

    fs::permissions(root, fs::perms::owner_all, fs::perm_options::replace);
    fs::remove_all(root);
}
#endif

int main() {
    TestResult r;
    printf("=== commit_staged_install atomicity tests ===\n");
    test_successful_swap(r);
    test_failed_install_preserves_working_binary(r);
    test_fresh_install(r);
#ifndef _WIN32
    test_swap_failure_preserves_working_binary(r);
#endif
    printf("\n%d passed, %d failed\n", r.passed, r.failed);
    return r.failed == 0 ? 0 : 1;
}
