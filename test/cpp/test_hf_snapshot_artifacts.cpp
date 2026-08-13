#include "lemon/hf_snapshot.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static void make_file(const fs::path& path) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream(path, std::ios::out | std::ios::trunc) << "x";
}

static fs::path make_temp_test_dir() {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / ("lemonade_hf_snapshot_test_" + std::to_string(tick));
}

static bool has(const std::vector<std::string>& files, const std::string& name) {
    return std::find(files.begin(), files.end(), name) != files.end();
}

int main() {
    const fs::path base_dir = make_temp_test_dir();
    std::error_code ec;
    fs::remove_all(base_dir, ec);
    fs::create_directories(base_dir, ec);

    // A repository hosting many variants of the same model, mirroring the
    // multi-artifact GGUF repos that made a bare snapshot-id comparison flag
    // every model whenever any sibling variant changed.
    const fs::path snapshot = base_dir / "models--vendor--repo" / "snapshots" / "aaaa1111";
    make_file(snapshot / "model-Q4_K_M.gguf");
    make_file(snapshot / "model-Q2_K.gguf");
    make_file(snapshot / "mmproj-F16.gguf");
    make_file(snapshot / "config.json");

    {
        const std::map<std::string, std::string> resolved = {
            {"main", (snapshot / "model-Q4_K_M.gguf").string()},
        };
        const auto files = lemon::hf_snapshot::model_artifacts(resolved, snapshot);
        check("selects only the resolved variant", files.size() == 1);
        check("selected path is snapshot-relative", has(files, "model-Q4_K_M.gguf"));
        check("sibling variant is not selected", !has(files, "model-Q2_K.gguf"));
    }

    {
        const std::map<std::string, std::string> resolved = {
            {"main", (snapshot / "model-Q4_K_M.gguf").string()},
            {"mmproj", (snapshot / "mmproj-F16.gguf").string()},
            {"config", (snapshot / "config.json").string()},
        };
        const auto files = lemon::hf_snapshot::model_artifacts(resolved, snapshot);
        check("every resolved role is selected",
              files.size() == 3 && has(files, "model-Q4_K_M.gguf") &&
                  has(files, "mmproj-F16.gguf") && has(files, "config.json"));
    }

    // A sharded GGUF resolves to one shard, so the whole family must be selected
    // or a change confined to another shard would be reported as "no update".
    {
        const fs::path shard_snapshot = base_dir / "sharded" / "snapshots" / "bbbb2222";
        make_file(shard_snapshot / "big-Q4_K_M-00001-of-00003.gguf");
        make_file(shard_snapshot / "big-Q4_K_M-00002-of-00003.gguf");
        make_file(shard_snapshot / "big-Q4_K_M-00003-of-00003.gguf");
        make_file(shard_snapshot / "big-Q4_K_M-00001-of-00005.gguf");
        make_file(shard_snapshot / "other-00001-of-00003.gguf");

        const std::map<std::string, std::string> resolved = {
            {"main", (shard_snapshot / "big-Q4_K_M-00001-of-00003.gguf").string()},
        };
        const auto files = lemon::hf_snapshot::model_artifacts(resolved, shard_snapshot);
        check("shard family is expanded from a single resolved shard",
              files.size() == 3 && has(files, "big-Q4_K_M-00001-of-00003.gguf") &&
                  has(files, "big-Q4_K_M-00002-of-00003.gguf") &&
                  has(files, "big-Q4_K_M-00003-of-00003.gguf"));
        check("a different shard total is a different family",
              !has(files, "big-Q4_K_M-00001-of-00005.gguf"));
        check("a different shard base is a different family",
              !has(files, "other-00001-of-00003.gguf"));
    }

    // Directory checkpoints (multi-file ONNX-style artifacts) select their whole
    // subtree, including nested files.
    {
        const fs::path dir_snapshot = base_dir / "dir" / "snapshots" / "cccc3333";
        make_file(dir_snapshot / "artifacts" / "model.onnx");
        make_file(dir_snapshot / "artifacts" / "nested" / "tokens.json");
        make_file(dir_snapshot / "unrelated.txt");

        const std::map<std::string, std::string> resolved = {
            {"main", (dir_snapshot / "artifacts").string()},
        };
        const auto files = lemon::hf_snapshot::model_artifacts(resolved, dir_snapshot);
        check("directory checkpoint selects its subtree",
              files.size() == 2 && has(files, "artifacts/model.onnx") &&
                  has(files, "artifacts/nested/tokens.json"));
        check("directory checkpoint uses forward slashes",
              std::none_of(files.begin(), files.end(),
                           [](const std::string& f) { return f.find('\\') != std::string::npos; }));
        check("file outside the checkpoint directory is not selected",
              !has(files, "unrelated.txt"));
    }

    // Auxiliary checkpoints in another repository are verified under their own
    // repository entry, so they are skipped instead of failing the whole set.
    {
        const fs::path other_snapshot = base_dir / "models--vendor--other" / "snapshots" / "dddd4444";
        make_file(other_snapshot / "mmproj-F16.gguf");

        const std::map<std::string, std::string> resolved = {
            {"main", (snapshot / "model-Q4_K_M.gguf").string()},
            {"mmproj", (other_snapshot / "mmproj-F16.gguf").string()},
        };
        const auto files = lemon::hf_snapshot::model_artifacts(resolved, snapshot);
        check("checkpoint from another repository is skipped",
              files.size() == 1 && has(files, "model-Q4_K_M.gguf"));
    }

    // Every "cannot establish the set" path must return empty so the caller
    // falls back to assuming the model changed rather than silently hiding an
    // available update.
    {
        const std::map<std::string, std::string> resolved = {
            {"main", (snapshot / "model-Q4_K_M.gguf").string()},
            {"draft", (snapshot / "missing.gguf").string()},
        };
        check("a missing resolved file yields no selection",
              lemon::hf_snapshot::model_artifacts(resolved, snapshot).empty());
    }
    {
        check("empty resolved paths yield no selection",
              lemon::hf_snapshot::model_artifacts({}, snapshot).empty());
    }
    {
        const std::map<std::string, std::string> resolved = {
            {"main", (snapshot / "model-Q4_K_M.gguf").string()},
        };
        check("a nonexistent snapshot yields no selection",
              lemon::hf_snapshot::model_artifacts(resolved, base_dir / "snapshots" / "nope").empty());
        check("an empty snapshot path yields no selection",
              lemon::hf_snapshot::model_artifacts(resolved, fs::path()).empty());
    }
    {
        const std::map<std::string, std::string> resolved = {
            {"main", (snapshot / "model-Q4_K_M.gguf").string()},
            {"draft", ""},
        };
        const auto files = lemon::hf_snapshot::model_artifacts(resolved, snapshot);
        check("an unresolved role is ignored without losing the rest",
              files.size() == 1 && has(files, "model-Q4_K_M.gguf"));
    }

    fs::remove_all(base_dir, ec);

    if (g_failures == 0) {
        std::printf("\nAll hf_snapshot artifact tests passed\n");
        return 0;
    }
    std::printf("\n%d hf_snapshot artifact test(s) FAILED\n", g_failures);
    return 1;
}
