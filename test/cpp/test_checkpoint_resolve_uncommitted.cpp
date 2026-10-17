// Regression test for issue #3075: the shared checkpoint resolver must not
// resolve auxiliary checkpoints (mmproj, draft, ...) out of a snapshot whose
// download is still in progress.
//
// Scenario from the issue: an update is interrupted after refs/main flips to
// the new snapshot, leaving a live .download_manifest.json in it. The small
// auxiliary files in that snapshot may be fully downloaded, but the snapshot
// is uncommitted and the completeness check rejects any path inside it — so
// resolving there hides a model whose previous snapshot is complete. The
// GGUF resolver already skips such snapshots; the shared BackendOps resolver
// must too, and fall back to the completed snapshot.

#include "lemon/backends/backend_ops.h"

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

void write_file(const fs::path& path, const std::string& contents = "x") {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << contents;
}

} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "lemonade-test-checkpoint-resolve";
    std::error_code ec;
    fs::remove_all(root, ec);

    const fs::path cache = root / "models--test--repo";
    const fs::path old_snapshot = cache / "snapshots" / "aaa111";
    const fs::path new_snapshot = cache / "snapshots" / "bbb222";
    const std::string variant = "mmproj-f16.gguf";

    write_file(cache / "refs" / "main", "bbb222\n");
    write_file(old_snapshot / variant);
    write_file(new_snapshot / variant);
    write_file(new_snapshot / ".download_manifest.json", "{}\n");

    lemon::backends::BackendOps ops;
    lemon::ModelInfo info;
    lemon::backends::CheckpointResolveContext ctx;
    ctx.hf_cache = root.string();
    ctx.model_cache_path = cache.string();
    ctx.repo_id = "test/repo";
    ctx.main_repo_id = "test/repo";
    ctx.variant = variant;
    ctx.type = "mmproj";
    ctx.registry_source = "huggingface";

    // Active snapshot is mid-download: resolve from the completed one.
    check("uncommitted active snapshot is skipped for the completed one",
          ops.resolve_checkpoint_path(info, ctx) ==
              (old_snapshot / variant).string());

    // Once the manifest clears, the active snapshot is preferred again.
    fs::remove(new_snapshot / ".download_manifest.json");
    check("committed active snapshot is preferred",
          ops.resolve_checkpoint_path(info, ctx) ==
              (new_snapshot / variant).string());

    // With the manifest back and no completed copy left, the checkpoint is
    // simply not downloaded — not resolved into the uncommitted snapshot.
    write_file(new_snapshot / ".download_manifest.json", "{}\n");
    fs::remove(old_snapshot / variant);
    check("only an uncommitted copy resolves as not downloaded",
          ops.resolve_checkpoint_path(info, ctx).empty());

    fs::remove_all(root, ec);
    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
