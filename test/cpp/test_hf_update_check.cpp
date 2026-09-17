#include "lemon/registry_files.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace rf = lemon::registry_files;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static void make_file(const fs::path& path, const std::string& content = "x") {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream(path, std::ios::out | std::ios::trunc) << content;
}

static fs::path make_temp_test_dir() {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / ("lemonade_hf_update_check_test_" + std::to_string(tick));
}

static bool has(const std::vector<std::string>& files, const std::string& name) {
    return std::find(files.begin(), files.end(), name) != files.end();
}

static rf::HfFileMetadata metadata(std::size_t size, const std::string& content_id) {
    rf::HfFileMetadata m;
    m.size = size;
    m.content_id = content_id;
    m.hash_algorithm = "sha256";
    m.hash_value = content_id;
    return m;
}

int main() {
    const fs::path base_dir = make_temp_test_dir();
    std::error_code ec;
    fs::remove_all(base_dir, ec);
    fs::create_directories(base_dir, ec);

    // --- select_main_repo_files: GGUF variant + auto-included config/tokenizer ---
    // Pull and the update check share this selection, so a change confined to
    // an auto-selected tokenizer/config file is always compared.
    {
        const std::vector<std::string> repo_files = {
            "model-Q4_K_M.gguf", "model-Q2_K.gguf", "mmproj-F16.gguf",
            "config.json", "tokenizer.json", "README.md",
        };
        const auto files =
            rf::select_main_repo_files("vendor/repo", "llamacpp", "model-Q4_K_M.gguf", repo_files);
        check("selects the resolved variant", has(files, "model-Q4_K_M.gguf"));
        check("auto-includes config.json and tokenizer.json",
              has(files, "config.json") && has(files, "tokenizer.json"));
        check("sibling variant is not selected", !has(files, "model-Q2_K.gguf"));
        check("unrelated files are not selected",
              !has(files, "mmproj-F16.gguf") && !has(files, "README.md"));
    }

    // A sharded GGUF selected by quantization name expands to the whole shard
    // family, mirroring pull.
    {
        const std::vector<std::string> repo_files = {
            "big-Q4_K_M-00001-of-00003.gguf", "big-Q4_K_M-00002-of-00003.gguf",
            "big-Q4_K_M-00003-of-00003.gguf", "model-Q2_K.gguf", "config.json",
        };
        const auto files = rf::select_main_repo_files("vendor/repo", "llamacpp", "Q4_K_M", repo_files);
        check("shard family is expanded from the quantization name",
              has(files, "big-Q4_K_M-00001-of-00003.gguf") &&
                  has(files, "big-Q4_K_M-00002-of-00003.gguf") &&
                  has(files, "big-Q4_K_M-00003-of-00003.gguf"));
        check("sibling quant is not selected", !has(files, "model-Q2_K.gguf"));
        check("config.json added on top of shards", has(files, "config.json"));
    }

    // No variant and no backend-specific selection means the whole repository.
    {
        const std::vector<std::string> repo_files = {"a.gguf", "config.json"};
        const auto files = rf::select_main_repo_files("vendor/repo", "llamacpp", "", repo_files);
        check("empty variant selects the whole repository", files.size() == repo_files.size());
    }

    // Direct single-file variants are validated against the repository listing.
    {
        const std::vector<std::string> repo_files = {"model.safetensors", "config.json"};
        const auto files =
            rf::select_main_repo_files("vendor/repo", "llamacpp", "model.safetensors", repo_files);
        check("direct file variant is selected",
              has(files, "model.safetensors") && has(files, "config.json"));
        bool threw = false;
        try {
            rf::select_main_repo_files("vendor/repo", "llamacpp", "missing.safetensors", repo_files);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check("a missing direct file throws like pull", threw);
    }

    // A directory checkpoint (Moonshine-style) selects its subtree from the
    // given listing — which for the update check is the NEW revision's tree, so
    // a file added upstream inside the directory is part of the comparison.
    {
        const std::vector<std::string> latest_files = {
            "medium-streaming-en/quantized/model.onnx",
            "medium-streaming-en/quantized/tokens.json",
            "medium-streaming-en/quantized/added_upstream.onnx",
            "README.md",
        };
        const auto files = rf::select_main_repo_files(
            "vendor/repo", "moonshine", "medium-streaming-en/quantized", latest_files);
        check("directory checkpoint selects its subtree", files.size() == 3);
        check("a file added upstream inside the directory is selected",
              has(files, "medium-streaming-en/quantized/added_upstream.onnx"));
        check("files outside the directory are not selected", !has(files, "README.md"));
    }

    // --- select_main_repo_files_union: a removed file must not silently drop out ---
    {
        const std::vector<std::string> old_files = {
            "medium-streaming-en/quantized/model.onnx",
            "medium-streaming-en/quantized/tokens.json",
        };
        const std::vector<std::string> new_files = {
            "medium-streaming-en/quantized/model.onnx",
        };
        const auto files = rf::select_main_repo_files_union(
            "vendor/repo", "moonshine", "medium-streaming-en/quantized", old_files, new_files);
        check("a file removed from the new revision stays in the union",
              has(files, "medium-streaming-en/quantized/tokens.json"));
        check("the surviving file is still in the union",
              has(files, "medium-streaming-en/quantized/model.onnx"));
    }

    // A dropped GGUF shard is the same bug class, not just directory checkpoints.
    {
        const std::vector<std::string> old_files = {
            "big-Q4_K_M-00001-of-00002.gguf", "big-Q4_K_M-00002-of-00002.gguf", "config.json",
        };
        const std::vector<std::string> new_files = {
            "big-Q4_K_M-00001-of-00002.gguf", "config.json",
        };
        const auto files = rf::select_main_repo_files_union(
            "vendor/repo", "llamacpp", "Q4_K_M", old_files, new_files);
        check("a shard removed from the new revision stays in the union",
              has(files, "big-Q4_K_M-00002-of-00002.gguf"));
    }

    // Overlap isn't duplicated; the earlier added-file fix still works through the union.
    {
        const std::vector<std::string> old_files = {"model-Q4_K_M.gguf", "config.json"};
        const std::vector<std::string> new_files = {
            "model-Q4_K_M.gguf", "config.json", "tokenizer.json",
        };
        const auto files = rf::select_main_repo_files_union(
            "vendor/repo", "llamacpp", "model-Q4_K_M.gguf", old_files, new_files);
        std::size_t model_count = 0;
        for (const auto& f : files) {
            if (f == "model-Q4_K_M.gguf") ++model_count;
        }
        check("a file present on both sides is not duplicated", model_count == 1);
        check("a file added upstream is still included", has(files, "tokenizer.json"));
    }

    // A missing direct-file variant on either side still throws.
    {
        const std::vector<std::string> old_files = {"model.safetensors", "config.json"};
        const std::vector<std::string> new_files = {"config.json"};
        bool threw = false;
        try {
            rf::select_main_repo_files_union(
                "vendor/repo", "llamacpp", "model.safetensors", old_files, new_files);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check("a direct file missing from either side throws like pull", threw);
    }

    // --- merge_reuse_comparison_files: pull keeps same-repository auxiliary
    // checkpoints in the reuse comparison instead of replacing the set ---
    {
        // files already carries the current main selection plus an mmproj that
        // lives in the same repository (pull appends it to files_to_download).
        const std::vector<std::string> base_files = {
            "model-Q4_K_M.gguf", "config.json", "mmproj-F16.gguf",
        };
        const std::vector<std::string> old_files = {
            "model-Q4_K_M.gguf", "config.json", "mmproj-F16.gguf",
        };
        const std::vector<std::string> new_files = {"model-Q4_K_M.gguf", "config.json"};
        const auto merged = rf::merge_reuse_comparison_files(
            base_files, "vendor/repo", "llamacpp", "model-Q4_K_M.gguf", old_files, new_files);
        check("a same-repository auxiliary checkpoint is kept in the comparison",
              has(merged, "mmproj-F16.gguf"));
        check("a main artifact is not dropped", has(merged, "model-Q4_K_M.gguf"));
        check("an auto-selected config is kept", has(merged, "config.json"));
    }
    {
        // Overlap between base_files and the main union isn't duplicated.
        const std::vector<std::string> base_files = {"model-Q4_K_M.gguf", "mmproj-F16.gguf"};
        const std::vector<std::string> old_files = {"model-Q4_K_M.gguf", "config.json"};
        const std::vector<std::string> new_files = {"model-Q4_K_M.gguf", "config.json"};
        const auto merged = rf::merge_reuse_comparison_files(
            base_files, "vendor/repo", "llamacpp", "model-Q4_K_M.gguf", old_files, new_files);
        std::size_t model_count = 0;
        for (const auto& f : merged) {
            if (f == "model-Q4_K_M.gguf") ++model_count;
        }
        check("overlap with the main union is not duplicated", model_count == 1);
        check("the config file is still merged in", has(merged, "config.json"));
    }

    // --- pull reuse: a same-repository auxiliary checkpoint that changed must
    // prevent snapshot reuse even when the main artifact is unchanged ---
    {
        const fs::path snapshot = base_dir / "reuse_aux" / "snapshots" / "aaaa1111";
        make_file(snapshot / "model-Q4_K_M.gguf", "11111");
        make_file(snapshot / "config.json", "x");
        make_file(snapshot / "mmproj-F16.gguf", "old");

        auto key = [](const std::string& f) { return rf::hf_file_metadata_key("vendor/repo", f); };
        std::map<std::string, rf::HfFileMetadata> current, previous;
        current[key("model-Q4_K_M.gguf")] = metadata(5, "lfs:aaa");
        previous[key("model-Q4_K_M.gguf")] = metadata(5, "lfs:aaa");
        current[key("config.json")] = metadata(1, "git:bbb");
        previous[key("config.json")] = metadata(1, "git:bbb");
        current[key("mmproj-F16.gguf")] = metadata(3, "lfs:new");
        previous[key("mmproj-F16.gguf")] = metadata(3, "lfs:old");

        // Main artifact unchanged, same-repo mmproj changed: reuse must fail so
        // the pull does not silently keep the stale snapshot.
        check("a changed same-repo auxiliary checkpoint blocks snapshot reuse",
              !rf::can_reuse_previous_hf_snapshot(
                  "vendor/repo",
                  {"model-Q4_K_M.gguf", "config.json", "mmproj-F16.gguf"},
                  snapshot, current, previous));

        // Control: identical mmproj reuses.
        auto same_mmproj = previous;
        same_mmproj[key("mmproj-F16.gguf")] = metadata(3, "lfs:new");
        check("an unchanged same-repo auxiliary checkpoint reuses",
              rf::can_reuse_previous_hf_snapshot(
                  "vendor/repo",
                  {"model-Q4_K_M.gguf", "config.json", "mmproj-F16.gguf"},
                  snapshot, current, same_mmproj));
    }

    // --- group_aux_checkpoint_variants: auxiliary checkpoints get their own
    // repository entries in the update check ---
    {
        const std::map<std::string, std::string> checkpoints = {
            {"main", "vendor/main:Q4_K_M"},
            {"mmproj", "vendor/aux:mmproj-F16.gguf"},
            {"draft", "vendor/aux:draft-Q4_K_M.gguf"},
            {"npu_cache", "vendor/cache:should-not-appear"},
        };
        const auto grouped = rf::group_aux_checkpoint_variants(checkpoints);
        check("auxiliary checkpoints are grouped by their own repository",
              grouped.size() == 1 && grouped.count("vendor/aux") == 1);
        check("all auxiliary variants are listed",
              grouped.at("vendor/aux").size() == 2 &&
                  has(grouped.at("vendor/aux"), "mmproj-F16.gguf") &&
                  has(grouped.at("vendor/aux"), "draft-Q4_K_M.gguf"));
    }
    {
        const std::map<std::string, std::string> checkpoints = {
            {"main", "vendor/main:Q4_K_M"},
            {"mmproj", "vendor/main:mmproj-F16.gguf"},
        };
        const auto grouped = rf::group_aux_checkpoint_variants(checkpoints);
        check("a same-repository auxiliary checkpoint groups under the main repo",
              grouped.size() == 1 && grouped.count("vendor/main") == 1 &&
                  has(grouped.at("vendor/main"), "mmproj-F16.gguf"));
    }
    {
        const std::map<std::string, std::string> checkpoints = {
            {"main", "vendor/main:Q4_K_M"},
            {"mmproj", "no-variant-shape"},
        };
        check("a checkpoint without repo:variant shape is skipped",
              rf::group_aux_checkpoint_variants(checkpoints).empty());
    }

    // --- active_local_snapshot: the comparison baseline is the snapshot on
    // disk, never the processed-at-pull sha ---
    // Regression: pull at upstream commit B reused snapshot A (artifacts
    // unchanged), so refs/main=A, the processed provenance says B, and B's
    // snapshot directory was removed locally. When upstream later commits C,
    // comparing against B cannot work — snapshots/B does not exist. The local
    // baseline must stay A no matter what the provenance records.
    {
        const fs::path cache = base_dir / "models--vendor--repo";
        make_file(cache / "snapshots" / "aaaa1111" / "model.gguf");
        make_file(cache / "refs" / "main", "aaaa1111\n");
        std::ofstream(cache / ".lemonade_registry.json")
            << R"({"processed_models":{"m":{"selection":"sel","snapshot_id":"bbbb2222"}}})";

        const std::string resolved =
            (cache / "snapshots" / "aaaa1111" / "model.gguf").string();
        check("baseline derives from the resolved path",
              rf::snapshot_id_from_resolved_path(resolved, cache) == "aaaa1111");
        check("baseline is the on-disk snapshot, not the processed sha",
              rf::active_local_snapshot(resolved, cache) == "aaaa1111");
        check("baseline falls back to refs/main",
              rf::active_local_snapshot("", cache) == "aaaa1111");
        check("a resolved path outside snapshots yields no baseline",
              rf::snapshot_id_from_resolved_path((base_dir / "elsewhere.gguf").string(), cache).empty());
        check("no resolved path and no refs yields no baseline",
              rf::active_local_snapshot("", base_dir / "models--vendor--empty").empty());
    }

    // --- can_reuse_previous_hf_snapshot: identical content at both revisions ---
    {
        const fs::path snapshot = base_dir / "reuse" / "snapshots" / "aaaa1111";
        make_file(snapshot / "model.gguf", "12345");
        make_file(snapshot / "config.json", "x");
        make_file(snapshot / "dir" / "added_upstream.onnx", "yy");

        auto key = [](const std::string& f) { return rf::hf_file_metadata_key("vendor/repo", f); };
        std::map<std::string, rf::HfFileMetadata> current, previous;
        current[key("model.gguf")] = metadata(5, "lfs:aaa");
        previous[key("model.gguf")] = metadata(5, "lfs:aaa");
        current[key("config.json")] = metadata(1, "git:bbb");
        previous[key("config.json")] = metadata(1, "git:bbb");

        check("identical artifacts are reusable",
              rf::can_reuse_previous_hf_snapshot("vendor/repo", {"model.gguf", "config.json"},
                                                 snapshot, current, previous));

        // A file present in the new revision's selection but unknown at the
        // previous revision — e.g. added upstream inside a directory checkpoint —
        // must report changed.
        current[key("dir/added_upstream.onnx")] = metadata(2, "lfs:ccc");
        check("a file added upstream is detected",
              !rf::can_reuse_previous_hf_snapshot(
                  "vendor/repo", {"model.gguf", "config.json", "dir/added_upstream.onnx"},
                  snapshot, current, previous));

        auto mismatched = previous;
        mismatched[key("model.gguf")] = metadata(5, "lfs:zzz");
        check("a content change is detected",
              !rf::can_reuse_previous_hf_snapshot("vendor/repo", {"model.gguf"}, snapshot,
                                                  current, mismatched));

        auto missing_current = current;
        missing_current.erase(key("model.gguf"));
        check("a file missing from the new revision's metadata is detected",
              !rf::can_reuse_previous_hf_snapshot("vendor/repo", {"model.gguf"}, snapshot,
                                                  missing_current, previous));

        check("a file missing on disk is detected",
              !rf::can_reuse_previous_hf_snapshot("vendor/repo", {"ghost.gguf"}, snapshot,
                                                  current, previous));

        make_file(snapshot / "model.gguf.partial", "12");
        check("a partial download is detected",
              !rf::can_reuse_previous_hf_snapshot("vendor/repo", {"model.gguf"}, snapshot,
                                                  current, previous));
        fs::remove(snapshot / "model.gguf.partial", ec);

        auto wrong_size = current;
        wrong_size[key("model.gguf")] = metadata(99, "lfs:aaa");
        check("a size mismatch against the local file is detected",
              !rf::can_reuse_previous_hf_snapshot("vendor/repo", {"model.gguf"}, snapshot,
                                                  wrong_size, wrong_size));

        check("an empty selection is not reusable",
              !rf::can_reuse_previous_hf_snapshot("vendor/repo", {}, snapshot, current, previous));
        check("a nonexistent snapshot is not reusable",
              !rf::can_reuse_previous_hf_snapshot("vendor/repo", {"model.gguf"},
                                                  base_dir / "snapshots" / "nope",
                                                  current, previous));
    }

    // --- select_metadata: selected files filtered out of merged tree pages ---
    {
        const std::map<std::string, rf::HfFileMetadata> tree_entries = {
            {"model-Q4_K_M.gguf", metadata(5, "lfs:aaa")},
            {"model-Q2_K.gguf", metadata(5, "lfs:bbb")},
            {"config.json", metadata(1, "git:ccc")},
        };
        const auto selected = rf::select_metadata(
            "vendor/repo", {"model-Q4_K_M.gguf", "config.json", "ghost.gguf"}, tree_entries);
        check("selection keeps only the requested files",
              selected.size() == 2 &&
                  selected.count(rf::hf_file_metadata_key("vendor/repo", "model-Q4_K_M.gguf")) &&
                  selected.count(rf::hf_file_metadata_key("vendor/repo", "config.json")));
        check("a selected file absent from the tree is omitted",
              !selected.count(rf::hf_file_metadata_key("vendor/repo", "ghost.gguf")));
        check("unrequested tree entries are ignored",
              !selected.count(rf::hf_file_metadata_key("vendor/repo", "model-Q2_K.gguf")));
        check("empty selection yields empty metadata",
              rf::select_metadata("vendor/repo", {}, tree_entries).empty());
    }

    // --- DeterminationTracker: verified only once every repository a model
    // spans has completed a determination ---
    {
        rf::DeterminationTracker tracker;
        tracker.add_pending("model-a");
        tracker.add_pending("model-a");
        check("a model with two pending repositories starts unverified",
              !tracker.is_verified("model-a") && tracker.pending_count("model-a") == 2);

        check("determining the first repository does not verify the model yet",
              !tracker.mark_determined("model-a"));
        check("still unverified with one repository outstanding",
              !tracker.is_verified("model-a") && tracker.pending_count("model-a") == 1);

        check("determining the second repository verifies the model",
              tracker.mark_determined("model-a"));
        check("verified once every repository has completed",
              tracker.is_verified("model-a") && tracker.pending_count("model-a") == 0);
    }
    {
        rf::DeterminationTracker tracker;
        tracker.add_pending("model-b");
        check("mark_determined returns true exactly once",
              tracker.mark_determined("model-b"));
        check("a repeated mark_determined on an already-verified model is a no-op, not an underflow",
              !tracker.mark_determined("model-b") && tracker.is_verified("model-b") &&
                  tracker.pending_count("model-b") == 0);
    }
    {
        rf::DeterminationTracker tracker;
        check("a model with no pending repositories is never spuriously verified",
              !tracker.mark_determined("model-c") && !tracker.is_verified("model-c"));
    }

    fs::remove_all(base_dir, ec);

    if (g_failures == 0) {
        std::printf("\nAll hf update check tests passed\n");
        return 0;
    }
    std::printf("\n%d hf update check test(s) FAILED\n", g_failures);
    return 1;
}
