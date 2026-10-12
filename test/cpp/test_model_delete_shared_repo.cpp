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

    // ThinkSound and ONNX Runtime resolve "main" to a folder rather than a file.
    // dir-b's download is mid-flight in the same snapshot folder.
    fs::path dir_repo = make_repo(hf_root, "models--org--dir", {"model.safetensors"});
    write_file(dir_repo / "snapshots" / "snap" / "other.bin.partial", "partial");

    // Same, with no refs/main: the backend then falls back to the repo root, so
    // the resolved "variant path" is the whole repo directory.
    fs::path rootfb_repo = hf_root / "models--org--rootfb";
    write_file(rootfb_repo / "snapshots" / "snap" / "model.safetensors");
    write_file(rootfb_repo / "snapshots" / "snap" / "other.bin.partial", "partial");

    // cancel-a's own download was interrupted and never produced a file. cancel-b
    // is listed but was never downloaded, so nothing else lives here.
    fs::path cancel_repo = hf_root / "models--org--cancel";
    write_file(cancel_repo / "snapshots" / "snap" / "a.gguf.partial", "partial");
    write_file(cancel_repo / "snapshots" / "snap" / ".download_manifest.json",
               R"({"files":[{"name":"a.gguf"}]})");

    // Only busy-b's download lives here, and it has not produced a file yet.
    fs::path busy_repo = hf_root / "models--org--busy";
    write_file(busy_repo / "snapshots" / "snap" / "other.gguf.partial", "partial");

    // stale-b's multi-file download stopped between files: every .partial was
    // already renamed away, so its manifest is the only resume marker left.
    // stale-a owns nothing here, and its shards do not match stale-b's variant.
    fs::path stale_repo = hf_root / "models--org--stale";
    write_file(stale_repo / "snapshots" / "snap" / "b-00001-of-00002.gguf");
    write_file(stale_repo / "snapshots" / "snap" / ".download_manifest.json",
               R"({"files":[{"name":"b-00001-of-00002.gguf"},)"
               R"({"name":"b-00002-of-00002.gguf"}]})");

    // Two catalog entries that name the exact same checkpoint.
    fs::path twin_repo = make_repo(hf_root, "models--org--twin", {"x.gguf"});

    // quant-a's checkpoint names a bare quantization, so the file it downloads
    // is much longer than the variant. Its own interrupted download must still
    // be recognized as its own.
    fs::path quant_repo = hf_root / "models--org--quant";
    write_file(quant_repo / "snapshots" / "snap" / "gemma-3-4b-it-Q4_K_M.gguf.partial",
               "partial");
    write_file(quant_repo / "snapshots" / "snap" / ".download_manifest.json",
               R"({"files":[{"name":"gemma-3-4b-it-Q4_K_M.gguf"}]})");

    // whole-a has no variant, so it claims every file here and no name can tell
    // the marker apart. Nothing else in the catalog names this repo.
    fs::path whole_repo = hf_root / "models--org--whole";
    write_file(whole_repo / "snapshots" / "snap" / "anything.gguf.partial", "partial");

    // Same, but shared-whole-b also names this repo, so the marker may be its
    // download and the repo has to survive.
    fs::path whole_shared_repo = hf_root / "models--org--wholeshared";
    write_file(whole_shared_repo / "snapshots" / "snap" / "b.gguf.partial", "partial");

    // Repos used by the late-arrival cases further down.
    fs::path late_repo = make_repo(hf_root, "models--org--late", {"a.gguf"});
    fs::path arrive_repo = hf_root / "models--org--arrive";

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
        {"dir-a", json{{"checkpoint", "org/dir"}, {"recipe", "thinksound"}}},
        {"rootfb-a", json{{"checkpoint", "org/rootfb"}, {"recipe", "thinksound"}}},
        {"cancel-a", json{{"checkpoint", "org/cancel:a.gguf"}, {"recipe", "llamacpp"}}},
        {"cancel-b", json{{"checkpoint", "org/cancel:b.gguf"}, {"recipe", "llamacpp"}}},
        {"busy-a", json{{"checkpoint", "org/busy:a.gguf"}, {"recipe", "llamacpp"}}},
        {"busy-b", json{{"checkpoint", "org/busy:other.gguf"}, {"recipe", "llamacpp"}}},
        {"stale-a", json{{"checkpoint", "org/stale:a.gguf"}, {"recipe", "llamacpp"}}},
        {"stale-b", json{{"checkpoint", "org/stale:b.gguf"}, {"recipe", "llamacpp"}}},
        {"twin-a", json{{"checkpoint", "org/twin:x.gguf"}, {"recipe", "llamacpp"}}},
        {"twin-b", json{{"checkpoint", "org/twin:x.gguf"}, {"recipe", "llamacpp"}}},
        {"quant-a", json{{"checkpoint", "org/quant:Q4_K_M"}, {"recipe", "llamacpp"}}},
        {"whole-a", json{{"checkpoint", "org/whole"}, {"recipe", "llamacpp"}}},
        {"wholeshared-a", json{{"checkpoint", "org/wholeshared"}, {"recipe", "llamacpp"}}},
        {"wholeshared-b", json{{"checkpoint", "org/wholeshared:b.gguf"},
                               {"recipe", "llamacpp"}}},
        {"late-a", json{{"checkpoint", "org/late:a.gguf"}, {"recipe", "llamacpp"}}},
        {"late-b", json{{"checkpoint", "org/late:b.gguf"}, {"recipe", "llamacpp"}}},
        {"arrive-a", json{{"checkpoint", "org/arrive:a.gguf"}, {"recipe", "llamacpp"}}},
        {"arrive-b", json{{"checkpoint", "org/arrive:b.gguf"}, {"recipe", "llamacpp"}}},
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

        // dir-a's "main" is the snapshot folder, not a file in it. Clearing the
        // folder wholesale would destroy the very partial that sent this delete
        // down the variant-only path.
        manager.delete_model("user.dir-a");
        check("directory checkpoint keeps another checkpoint's partial",
              fs::exists(dir_repo / "snapshots" / "snap" / "other.bin.partial"));
        check("directory checkpoint still gives up its own file",
              !fs::exists(dir_repo / "snapshots" / "snap" / "model.safetensors"));

        // With no refs/main the backend resolves "main" to the repo root, so a
        // wholesale removal here would take the entire repo.
        manager.delete_model("user.rootfb-a");
        check("repo-root fallback does not remove the repo",
              fs::exists(rootfb_repo));
        check("repo-root fallback keeps the unfinished download",
              fs::exists(rootfb_repo / "snapshots" / "snap" / "other.bin.partial"));
        check("repo-root fallback still gives up its own file",
              !fs::exists(rootfb_repo / "snapshots" / "snap" / "model.safetensors"));

        // cancel-a has no file at all, so the delete takes the incomplete-cache
        // path. cancel-b is a catalog sibling with nothing on disk and must not
        // strand the leftovers of cancel-a's own interrupted download.
        manager.delete_model("user.cancel-a");
        check("incomplete-cache path reclaims a repo an empty sibling only lists",
              !fs::exists(cancel_repo));

        // busy-a also has no file, but the only resume marker here belongs to
        // busy-b's download.
        manager.delete_model("user.busy-a");
        check("incomplete-cache path keeps another model's download",
              fs::exists(busy_repo / "snapshots" / "snap" / "other.gguf.partial"));

        // stale-a has no file either. A manifest with no .partial beside it is
        // still a live download, and the files it already fetched are real.
        manager.delete_model("user.stale-a");
        check("manifest without a partial keeps another model's download",
              fs::exists(stale_repo / "snapshots" / "snap" / ".download_manifest.json"));
        check("files already fetched by that download are preserved",
              fs::exists(stale_repo / "snapshots" / "snap" / "b-00001-of-00002.gguf"));

        // twin-b names the very same file, so deleting twin-a must leave it.
        manager.delete_model("user.twin-a");
        check("a file two models resolve to is not removed with one of them",
              fs::exists(twin_repo / "snapshots" / "snap" / "x.gguf"));

        // A bare quantization is most of the catalog. Matching the variant
        // against the file name alone would call this download a stranger's and
        // strand everything it left behind.
        manager.delete_model("user.quant-a");
        check("a quantization-only variant still owns its own download",
              !fs::exists(quant_repo));

        // No variant at all, and nothing else names the repo, so the marker can
        // only belong to this model.
        manager.delete_model("user.whole-a");
        check("a variantless model owns the only download in its repo",
              !fs::exists(whole_repo));

        // No variant, but wholeshared-b names this repo too, so the marker may
        // be its download.
        manager.delete_model("user.wholeshared-a");
        check("a variantless model does not claim a download another model may own",
              fs::exists(whole_shared_repo / "snapshots" / "snap" / "b.gguf.partial"));
    }

    // Files that arrive after the model cache was built. The Hugging Face cache
    // is shared, so another application can fill a repo at any moment and nothing
    // tells Lemonade it happened. Each case needs its own manager: deleting a user
    // model invalidates the cache, which would rebuild it and hide the staleness.
    {
        ModelManager manager;
        (void)manager.get_model_info("user.late-a");  // builds the cache

        write_file(late_repo / "snapshots" / "snap" / "b.gguf");

        manager.delete_model("user.late-a");
        check("sibling downloaded after the cache was built keeps the repo alive",
              fs::exists(late_repo));
        check("late sibling's file is preserved",
              fs::exists(late_repo / "snapshots" / "snap" / "b.gguf"));
        check("late sibling case still removes the deleted model's file",
              !fs::exists(late_repo / "snapshots" / "snap" / "a.gguf"));
    }

    {
        ModelManager manager;
        (void)manager.get_model_info("user.arrive-a");  // builds the cache

        // The deleted model's own files arrive late too, so its cached resolved
        // path is empty and would send the delete down the incomplete-cache path.
        write_file(arrive_repo / "snapshots" / "snap" / "a.gguf");
        write_file(arrive_repo / "snapshots" / "snap" / "b.gguf");
        write_file(arrive_repo / "refs" / "main", "snap");

        manager.delete_model("user.arrive-a");
        check("model downloaded after the cache was built gives up its own file",
              !fs::exists(arrive_repo / "snapshots" / "snap" / "a.gguf"));
        check("late arrival keeps its sibling's file",
              fs::exists(arrive_repo / "snapshots" / "snap" / "b.gguf"));
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
