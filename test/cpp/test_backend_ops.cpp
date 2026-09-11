#include "lemon/backends/backend_ops.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(const char* name, bool condition) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) {
        ++failures;
    }
}

fs::path make_temp_dir() {
    fs::path dir = fs::temp_directory_path();
    dir /= "lemonade_backend_ops_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);
    return dir;
}

void write_text(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path);
    file << content;
}

lemon::backends::CheckpointResolveContext make_context(const fs::path& cache) {
    lemon::backends::CheckpointResolveContext ctx;
    ctx.hf_cache = cache.parent_path().string();
    ctx.model_cache_path = cache.string();
    ctx.repo_id = "org/vision";
    ctx.main_repo_id = "org/vision";
    ctx.variant = "mmproj-F16.gguf";
    ctx.type = "mmproj";
    ctx.checkpoint = "org/vision:mmproj-F16.gguf";
    ctx.registry_source = "huggingface";
    return ctx;
}

void test_interrupted_active_snapshot_falls_back() {
    const fs::path cache = make_temp_dir();
    const fs::path old_snapshot = cache / "snapshots" / "old-complete";
    const fs::path interrupted_snapshot = cache / "snapshots" / "new-interrupted";
    const fs::path expected = old_snapshot / "mmproj-F16.gguf";
    const fs::path misleading = interrupted_snapshot / "mmproj-F16.gguf";

    write_text(expected, "complete auxiliary checkpoint");
    write_text(interrupted_snapshot / ".download_manifest.json", "{}");
    write_text(misleading, "part of an interrupted update");
    write_text(cache / "refs" / "main", "new-interrupted\n");

    lemon::ModelInfo info;
    lemon::backends::BackendOps ops;
    const std::string resolved = ops.resolve_checkpoint_path(info, make_context(cache));

    check("auxiliary resolution skips the active snapshot with a live manifest",
          resolved == expected.string());
    check("auxiliary resolution never returns the interrupted snapshot",
          resolved != misleading.string());

    fs::remove_all(cache);
}

void test_completed_active_snapshot_remains_preferred() {
    const fs::path cache = make_temp_dir();
    const fs::path old_snapshot = cache / "snapshots" / "old-complete";
    const fs::path active_snapshot = cache / "snapshots" / "active-complete";
    const fs::path expected = active_snapshot / "mmproj-F16.gguf";

    write_text(old_snapshot / "mmproj-F16.gguf", "older auxiliary checkpoint");
    write_text(expected, "active auxiliary checkpoint");
    write_text(cache / "refs" / "main", "active-complete\n");

    lemon::ModelInfo info;
    lemon::backends::BackendOps ops;
    const std::string resolved = ops.resolve_checkpoint_path(info, make_context(cache));

    check("a completed active snapshot remains preferred over older snapshots",
          resolved == expected.string());

    fs::remove_all(cache);
}

void test_only_interrupted_snapshot_is_not_resolved() {
    const fs::path cache = make_temp_dir();
    const fs::path interrupted_snapshot = cache / "snapshots" / "interrupted";

    write_text(interrupted_snapshot / ".download_manifest.json", "{}");
    write_text(interrupted_snapshot / "mmproj-F16.gguf", "incomplete update");
    write_text(cache / "refs" / "main", "interrupted\n");

    lemon::ModelInfo info;
    lemon::backends::BackendOps ops;
    const std::string resolved = ops.resolve_checkpoint_path(info, make_context(cache));

    check("a cache containing only an interrupted snapshot resolves to no file",
          resolved.empty());

    fs::remove_all(cache);
}

}  // namespace

int main() {
    test_interrupted_active_snapshot_falls_back();
    test_completed_active_snapshot_remains_preferred();
    test_only_interrupted_snapshot_is_not_resolved();

    std::printf("%d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
