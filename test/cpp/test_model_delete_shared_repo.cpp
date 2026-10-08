// Deleting a model must reclaim its repo when nothing else is living in it, and
// must leave the repo alone when something is. The dividing line is what is on
// disk, not what the catalog lists: several entries routinely map to one repo,
// so counting catalog siblings strands the repo forever.

#include "lemon/model_manager.h"
#include "lemon/utils/path_utils.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using lemon::ModelManager;
using lemon::json;
using lemon::utils::path_to_utf8;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static fs::path make_temp_dir() {
    fs::path dir = fs::temp_directory_path();
    dir /= "model_delete_shared_repo_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);
    return dir;
}

static void write_file(const fs::path& path, const std::string& contents = "GGUF") {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

// One HF repo holding `files`, committed under a single snapshot.
static fs::path make_repo(const fs::path& hf_root,
                          const std::string& repo_dir_name,
                          const std::vector<std::string>& files) {
    fs::path repo = hf_root / repo_dir_name;
    for (const auto& file : files) {
        write_file(repo / "snapshots" / "snap" / file);
    }
    write_file(repo / "refs" / "main", "snap");
    return repo;
}

int main() {
    fs::path temp = make_temp_dir();
    fs::path hf_root = temp / "hf";
    fs::create_directories(hf_root);

    lemon::utils::set_cache_dir(path_to_utf8(temp));
    lemon::utils::set_config_dir(path_to_utf8(temp));
    lemon::utils::set_models_dir(path_to_utf8(hf_root));

    // Three repos, each referenced by two user models, so every case below goes
    // down the "catalog sibling exists" road that used to strand the repo.
    fs::path solo_repo = make_repo(hf_root, "models--org--solo", {"a.gguf"});
    fs::path both_repo = make_repo(hf_root, "models--org--both", {"a.gguf", "b.gguf"});
    fs::path partial_repo = make_repo(hf_root, "models--org--partial", {"a.gguf"});
    write_file(partial_repo / "snapshots" / "snap" / "b.gguf.partial", "partial");
    // aux-b's main file is here and intact, but its vae checkpoint never arrived.
    // That makes aux-b incomplete as a model while it still owns a file in the repo.
    fs::path aux_repo = make_repo(hf_root, "models--org--aux", {"a.gguf", "b.gguf"});
    fs::path manifest_repo = make_repo(hf_root, "models--org--manifest", {"a.gguf"});
    write_file(manifest_repo / "snapshots" / "other" / ".download_manifest.json", "{}");

    json user_models = {
        {"solo-a", json{{"checkpoint", "org/solo:a.gguf"}, {"recipe", "llamacpp"}}},
        {"solo-b", json{{"checkpoint", "org/solo:b.gguf"}, {"recipe", "llamacpp"}}},
        {"both-a", json{{"checkpoint", "org/both:a.gguf"}, {"recipe", "llamacpp"}}},
        {"both-b", json{{"checkpoint", "org/both:b.gguf"}, {"recipe", "llamacpp"}}},
        {"partial-a", json{{"checkpoint", "org/partial:a.gguf"}, {"recipe", "llamacpp"}}},
        {"partial-b", json{{"checkpoint", "org/partial:b.gguf"}, {"recipe", "llamacpp"}}},
        {"aux-a", json{{"checkpoint", "org/aux:a.gguf"}, {"recipe", "llamacpp"}}},
        {"aux-b", json{{"checkpoints", json{{"main", "org/aux:b.gguf"},
                                            {"vae", "org/aux:never-downloaded-vae.bin"}}},
                       {"recipe", "llamacpp"}}},
        {"manifest-a", json{{"checkpoint", "org/manifest:a.gguf"}, {"recipe", "llamacpp"}}},
        {"manifest-b", json{{"checkpoint", "org/manifest:b.gguf"}, {"recipe", "llamacpp"}}},
    };
    write_file(temp / "user_models.json", user_models.dump(2));

    {
        ModelManager manager;

        // solo-b is listed but was never downloaded, so nothing else is living
        // in the repo and the whole thing goes.
        manager.delete_model("user.solo-a");
        check("sibling that was never downloaded does not strand the repo",
              !fs::exists(solo_repo));

        // both-b is on disk, so the repo stays and only the deleted model's own
        // file is removed.
        manager.delete_model("user.both-a");
        check("repo survives while another downloaded model occupies it",
              fs::exists(both_repo));
        check("downloaded sibling's file is preserved",
              fs::exists(both_repo / "snapshots" / "snap" / "b.gguf"));
        check("deleted model's own file is removed",
              !fs::exists(both_repo / "snapshots" / "snap" / "a.gguf"));

        // partial-b is mid-download: not on disk as a model, but its .partial is
        // resumable and wiping the repo would throw it away.
        manager.delete_model("user.partial-a");
        check("unfinished download keeps the repo alive",
              fs::exists(partial_repo));
        check("resumable partial is preserved",
              fs::exists(partial_repo / "snapshots" / "snap" / "b.gguf.partial"));

        // aux-b counts as incomplete because its vae is missing, but the file it
        // owns in this repo is real. A model-wide "is it downloaded" test skips
        // aux-b here and takes its main file with the repo.
        manager.delete_model("user.aux-a");
        check("incomplete sibling still holding a file keeps the repo alive",
              fs::exists(aux_repo));
        check("incomplete sibling's intact main file is preserved",
              fs::exists(aux_repo / "snapshots" / "snap" / "b.gguf"));
        check("deleted model's own file is removed from the aux repo",
              !fs::exists(aux_repo / "snapshots" / "snap" / "a.gguf"));

        // No .partial is left here. The manifest is the only evidence that a
        // download stopped part way, and it has to be enough on its own.
        manager.delete_model("user.manifest-a");
        check("manifest without a partial keeps the repo alive",
              fs::exists(manifest_repo));
        check("download manifest is preserved",
              fs::exists(manifest_repo / "snapshots" / "other" / ".download_manifest.json"));
    }

    std::error_code ec;
    fs::remove_all(temp, ec);

    if (g_failures == 0) {
        std::printf("All model delete shared-repo tests passed.\n");
    } else {
        std::printf("%d model delete shared-repo test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
