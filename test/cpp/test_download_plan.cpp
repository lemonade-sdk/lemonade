#include "lemon/models/artifact_transfer.h"
#include "lemon/utils/path_utils.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;
using lemon::models::ArtifactTransfer;
using lemon::models::DownloadFile;
using lemon::models::DownloadPlan;
using lemon::models::download_plan_from_manifest;
using lemon::models::FileTransfer;
using lemon::models::LocalCopyFileTransfer;
using lemon::models::plan_filesystem_requirements;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

static void check_eq(const std::string& name, const std::string& actual,
                     const std::string& expected) {
    const bool ok = actual == expected;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) {
        std::printf("       expected: %s\n       actual:   %s\n", expected.c_str(), actual.c_str());
        ++g_failures;
    }
}

static void check_num(const std::string& name, std::uint64_t actual, std::uint64_t expected) {
    const bool ok = actual == expected;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) {
        std::printf("       expected: %llu\n       actual:   %llu\n",
                    static_cast<unsigned long long>(expected),
                    static_cast<unsigned long long>(actual));
        ++g_failures;
    }
}

static fs::path make_temp_dir(const char* tag) {
    fs::path dir = fs::temp_directory_path();
    dir /= std::string("download_plan_") + tag + "_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);
    return dir;
}

static void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

static std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string file_url(const fs::path& path) {
    return "file://" + lemon::utils::path_to_utf8(path);
}

// A transfer with only the local scheme proves a plan can complete without any
// HTTP at all.
static ArtifactTransfer local_only_transfer() {
    std::vector<std::unique_ptr<FileTransfer>> transfers;
    transfers.push_back(std::make_unique<LocalCopyFileTransfer>());
    return ArtifactTransfer(std::move(transfers));
}

static std::uint64_t total_needed(const std::vector<lemon::models::FilesystemRequirement>& reqs) {
    std::uint64_t total = 0;
    for (const auto& req : reqs) total += req.bytes_needed;
    return total;
}

static void test_plan_from_manifest() {
    const json manifest = {
        {"download_path", "/cache/main"},
        {"files_count", 2},
        {"files",
         {{{"name", "a.gguf"}, {"url", "https://h/a.gguf"}, {"size", 100}},
          {{"name", "sub/b.bin"},
           {"url", "https://h/b.bin"},
           {"size", 7},
           {"download_path", "/cache/other"},
           {"hash", {{"algorithm", "sha256"}, {"value", "cafe"}}}}}}};

    const DownloadPlan plan = download_plan_from_manifest(manifest);
    check_eq("plan target directory", plan.download_path, "/cache/main");
    check("file count comes from the files array", plan.files.size() == 2);

    check_eq("file name", plan.files[0].name, "a.gguf");
    check_eq("file url", plan.files[0].url, "https://h/a.gguf");
    check_num("file size", plan.files[0].size, 100);
    check("a file without its own target falls back later", plan.files[0].download_path.empty());
    check("no digest when the manifest has none", plan.files[0].expected_hash.empty());

    check_eq("per-file target is carried", plan.files[1].download_path, "/cache/other");
    check_eq("hash algorithm", plan.files[1].expected_hash_algorithm, "sha256");
    check_eq("hash value", plan.files[1].expected_hash, "cafe");
}

static void test_plan_bare_sha256() {
    const json manifest = {{"download_path", "/cache"},
                           {"files_count", 1},
                           {"files",
                            {{{"name", "m.bin"},
                              {"url", "https://h/m.bin"},
                              {"size", 1},
                              {"sha256", "abc123"}}}}};

    const DownloadPlan plan = download_plan_from_manifest(manifest);
    check_eq("a bare sha256 names the algorithm", plan.files[0].expected_hash_algorithm, "sha256");
    check_eq("a bare sha256 carries the value", plan.files[0].expected_hash, "abc123");
}

static void test_space_accounting() {
    const fs::path dir = make_temp_dir("space");

    DownloadPlan empty;
    empty.download_path = lemon::utils::path_to_utf8(dir);
    check("an empty plan needs nothing", plan_filesystem_requirements(empty).empty());

    DownloadPlan one;
    one.download_path = lemon::utils::path_to_utf8(dir);
    one.files.push_back(DownloadFile{"missing.bin", file_url(dir / "src.bin"), 1000, {}, {}, {}});
    auto reqs = plan_filesystem_requirements(one);
    check_num("a missing file needs its full size", reqs.empty() ? 0 : reqs[0].bytes_needed, 1000);

    // A completed file with no partial is already paid for.
    write_file(dir / "missing.bin", std::string(1000, 'x'));
    check("a completed file needs nothing", total_needed(plan_filesystem_requirements(one)) == 0);

    // A partial is credited against the file's cost.
    fs::remove(dir / "missing.bin");
    write_file(dir / "missing.bin.partial", std::string(400, 'x'));
    check_num("a partial is credited", total_needed(plan_filesystem_requirements(one)), 600);

    // Credit is capped at the manifest size, never negative.
    write_file(dir / "missing.bin.partial", std::string(1500, 'x'));
    check_num("credit is capped at the file size", total_needed(plan_filesystem_requirements(one)),
              0);

    // A size=0 entry alongside a partial must not underflow.
    DownloadPlan zero;
    zero.download_path = lemon::utils::path_to_utf8(dir);
    zero.files.push_back(DownloadFile{"missing.bin", file_url(dir / "src.bin"), 0, {}, {}, {}});
    check("a size zero entry with a partial needs nothing",
          total_needed(plan_filesystem_requirements(zero)) == 0);

    // Two files in one directory are checked as a combined total.
    fs::remove(dir / "missing.bin.partial");
    DownloadPlan two;
    two.download_path = lemon::utils::path_to_utf8(dir);
    two.files.push_back(DownloadFile{"one.bin", file_url(dir / "s1"), 100, {}, {}, {}});
    two.files.push_back(DownloadFile{"two.bin", file_url(dir / "s2"), 200, {}, {}, {}});
    const auto combined = plan_filesystem_requirements(two);
    check("files in one directory fold into one entry", combined.size() == 1);
    check_num("one directory sums its files", combined.empty() ? 0 : combined[0].bytes_needed, 300);

    // Two existing targets on one filesystem are accounted together, whether or
    // not their space readings were identical enough to fold into one entry.
    fs::create_directories(dir / "nested");
    DownloadPlan split;
    split.download_path = lemon::utils::path_to_utf8(dir);
    split.files.push_back(DownloadFile{"one.bin", file_url(dir / "s1"), 100, {}, {}, {}});
    split.files.push_back(DownloadFile{"two.bin", file_url(dir / "s2"), 200, {}, {},
                                       lemon::utils::path_to_utf8(dir / "nested")});
    check_num("targets on one filesystem total together",
              total_needed(plan_filesystem_requirements(split)), 300);

    // A target directory that does not exist yet cannot be measured, so it is
    // not accounted at all. Preserved from the pre-refactor check; fixing it
    // means measuring the nearest existing ancestor instead.
    DownloadPlan unmeasured;
    unmeasured.download_path = lemon::utils::path_to_utf8(dir / "not-created-yet");
    unmeasured.files.push_back(DownloadFile{"one.bin", file_url(dir / "s1"), 100, {}, {}, {}});
    check("an unmeasurable target is not accounted",
          total_needed(plan_filesystem_requirements(unmeasured)) == 0);

    fs::remove_all(dir);
}

static void test_copies_without_http() {
    const fs::path dir = make_temp_dir("copy");
    const std::string content = "lemonade artifact transfer fixture";
    const fs::path source = dir / "source.bin";
    write_file(source, content);

    DownloadPlan plan;
    plan.download_path = lemon::utils::path_to_utf8(dir / "cache");
    plan.files.push_back(DownloadFile{"model.bin", file_url(source), content.size(), {}, {}, {}});

    const ArtifactTransfer transfer = local_only_transfer();
    transfer.execute(plan, {}, nullptr);

    check_eq("bytes land in the cache", read_file(dir / "cache" / "model.bin"), content);
    check_eq("the source is left in place", read_file(source), content);
    check("no partial is left behind", !fs::exists(dir / "cache" / "model.bin.partial"));

    fs::remove_all(dir);
}

static void test_creates_parent_directories() {
    const fs::path dir = make_temp_dir("nested");
    write_file(dir / "src.gguf", "GGUF payload");

    DownloadPlan plan;
    plan.download_path = lemon::utils::path_to_utf8(dir / "cache");
    plan.files.push_back(
        DownloadFile{"deep/inner/model.gguf", file_url(dir / "src.gguf"), 12, {}, {}, {}});

    local_only_transfer().execute(plan, {}, nullptr);
    check("parent directories are created for a nested name",
          fs::exists(dir / "cache" / "deep" / "inner" / "model.gguf"));

    fs::remove_all(dir);
}

static void test_unknown_scheme_fails_before_bytes() {
    const fs::path dir = make_temp_dir("scheme");

    DownloadPlan plan;
    plan.download_path = lemon::utils::path_to_utf8(dir);
    plan.files.push_back(DownloadFile{"m.bin", "ftp://host/m.bin", 10, {}, {}, {}});

    bool threw = false;
    try {
        local_only_transfer().execute(plan, {}, nullptr);
    } catch (const std::exception& e) {
        threw = true;
        const std::string message = e.what();
        check("the error names the scheme", message.find("ftp") != std::string::npos);
    }
    check("an unhandled scheme is refused", threw);
    check("nothing was written for an unhandled scheme", !fs::exists(dir / "m.bin"));

    fs::remove_all(dir);
}

static void test_cancellation() {
    const fs::path dir = make_temp_dir("cancel");
    const std::string content = "payload that should never be copied";
    write_file(dir / "source.bin", content);

    DownloadPlan plan;
    plan.download_path = lemon::utils::path_to_utf8(dir / "cache");
    plan.files.push_back(DownloadFile{"model.bin", file_url(dir / "source.bin"), content.size(),
                                      {}, {}, {}});

    bool threw = false;
    std::string message;
    try {
        local_only_transfer().execute(
            plan, {}, [](const lemon::DownloadProgress&) { return false; });
    } catch (const std::exception& e) {
        threw = true;
        message = e.what();
    }
    check("a cancelled download throws", threw);
    check_eq("the cancellation message is stable", message, std::string("Download cancelled"));
    check("no target file was created", !fs::exists(dir / "cache" / "model.bin"));
    check_eq("the source survived the cancelled copy", read_file(dir / "source.bin"), content);

    fs::remove_all(dir);
}

static void test_skips_completed_file() {
    const fs::path dir = make_temp_dir("skip");
    const std::string content = "already here";
    write_file(dir / "source.bin", content);
    write_file(dir / "cache" / "model.bin", content);

    DownloadPlan plan;
    plan.download_path = lemon::utils::path_to_utf8(dir / "cache");
    plan.files.push_back(DownloadFile{"model.bin", file_url(dir / "source.bin"), content.size(),
                                      {}, {}, {}});

    int events = 0;
    int final_percent = -1;
    local_only_transfer().execute(plan, {}, [&](const lemon::DownloadProgress& progress) {
        ++events;
        final_percent = progress.percent;
        return true;
    });

    check("a skipped file still reports completion", events >= 2);
    check_num("the final report is 100 percent", static_cast<std::uint64_t>(final_percent), 100);
    check_eq("the skipped file is untouched", read_file(dir / "cache" / "model.bin"), content);

    fs::remove_all(dir);
}

static void test_hash_verification() {
    const fs::path dir = make_temp_dir("hash");
    const std::string content = "lemonade artifact transfer fixture";
    const char* digest = "312dcf0b9f534789e17e00cf4249eb68cb531bd8a0c321206bcb91bd099db64e";
    write_file(dir / "source.bin", content);

    DownloadPlan good;
    good.download_path = lemon::utils::path_to_utf8(dir / "cache");
    good.files.push_back(
        DownloadFile{"model.bin", file_url(dir / "source.bin"), content.size(), "sha256", digest,
                     {}});
    local_only_transfer().execute(good, {}, nullptr);
    check("a matching digest accepts the file", fs::exists(dir / "cache" / "model.bin"));

    fs::remove_all(dir / "cache");
    DownloadPlan bad;
    bad.download_path = lemon::utils::path_to_utf8(dir / "cache");
    bad.files.push_back(DownloadFile{"model.bin", file_url(dir / "source.bin"), content.size(),
                                     "sha256", std::string(64, 'a'), {}});

    bool threw = false;
    std::string message;
    try {
        local_only_transfer().execute(bad, {}, nullptr);
    } catch (const std::exception& e) {
        threw = true;
        message = e.what();
    }
    check("a mismatching digest fails", threw);
    check("the failure names the file", message.find("model.bin") != std::string::npos);
    check("the final file is not put in place on a mismatch",
          !fs::exists(dir / "cache" / "model.bin"));
    check("the staged file is not left behind on a mismatch",
          !fs::exists(dir / "cache" / "model.bin.partial"));
    check_eq("the source is untouched by a failed verification", read_file(dir / "source.bin"),
             content);

    fs::remove_all(dir);
}

static void test_gguf_magic_rejected() {
    const fs::path dir = make_temp_dir("gguf");
    // An HTML error page saved under a .gguf name is exactly what llama.cpp
    // would fail on later.
    write_file(dir / "source.html", "<html>404 Not Found</html>");

    DownloadPlan plan;
    plan.download_path = lemon::utils::path_to_utf8(dir / "cache");
    plan.files.push_back(
        DownloadFile{"model.gguf", file_url(dir / "source.html"), 24, {}, {}, {}});

    bool threw = false;
    std::string message;
    try {
        local_only_transfer().execute(plan, {}, nullptr);
    } catch (const std::exception& e) {
        threw = true;
        message = e.what();
    }
    check("a .gguf without GGUF magic fails validation", threw);
    check("the failure reports validation", message.find("validation failed") != std::string::npos);
    check("the invalid .gguf is removed so the next pull starts clean",
          !fs::exists(dir / "cache" / "model.gguf"));

    fs::remove_all(dir);
}

static void test_gguf_with_magic_accepted() {
    const fs::path dir = make_temp_dir("gguf_ok");
    write_file(dir / "source.gguf", std::string("GGUF") + std::string(16, '\0'));

    DownloadPlan plan;
    plan.download_path = lemon::utils::path_to_utf8(dir / "cache");
    plan.files.push_back(
        DownloadFile{"model.gguf", file_url(dir / "source.gguf"), 20, {}, {}, {}});

    local_only_transfer().execute(plan, {}, nullptr);
    check("a .gguf with GGUF magic is accepted", fs::exists(dir / "cache" / "model.gguf"));

    fs::remove_all(dir);
}

static void test_removes_invalid_cached_gguf_before_download() {
    const fs::path dir = make_temp_dir("gguf_cache");
    write_file(dir / "source.gguf", std::string("GGUF") + std::string(8, '\0'));
    // A poisoned cache entry must be replaced, not treated as complete.
    write_file(dir / "cache" / "model.gguf", "<html>error page</html>");

    DownloadPlan plan;
    plan.download_path = lemon::utils::path_to_utf8(dir / "cache");
    plan.files.push_back(
        DownloadFile{"model.gguf", file_url(dir / "source.gguf"), 12, {}, {}, {}});

    local_only_transfer().execute(plan, {}, nullptr);
    check_eq("the poisoned cache entry is overwritten",
             read_file(dir / "cache" / "model.gguf").substr(0, 4), std::string("GGUF"));

    fs::remove_all(dir);
}

int main() {
    test_plan_from_manifest();
    test_plan_bare_sha256();
    test_space_accounting();
    test_copies_without_http();
    test_creates_parent_directories();
    test_unknown_scheme_fails_before_bytes();
    test_cancellation();
    test_skips_completed_file();
    test_hash_verification();
    test_gguf_magic_rejected();
    test_gguf_with_magic_accepted();
    test_removes_invalid_cached_gguf_before_download();

    if (g_failures == 0) {
        std::printf("All download plan tests passed.\n");
    } else {
        std::printf("%d download plan test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
