// Deleting a model must reclaim its repo when nothing else is living in it, and
// must leave the repo alone when something is. The dividing line is what is on
// disk, not what the catalog lists: several entries routinely map to one repo,
// so counting catalog siblings strands the repo forever.

#include "lemon/config_file.h"
#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
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

    // Backend filtering drops a model whose recipe the host cannot run, and the
    // fixtures below reach for recipes that resolve a checkpoint to a directory
    // — none of which macOS supports. Which files a delete may remove does not
    // depend on what this machine can load.
    json config_json = lemon::ConfigFile::base_defaults();
    config_json["disable_model_filtering"] = true;
    lemon::RuntimeConfig config(config_json);
    lemon::RuntimeConfig::set_global(&config);

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

    // The same pair, but the download never finished. Neither model has a
    // resolved file, so nothing occupies the repo, and the marker matches the
    // deleted model's variant exactly -- which is also the twin's.
    fs::path twinp_repo = hf_root / "models--org--twinp";
    write_file(twinp_repo / "snapshots" / "snap" / "x.gguf.partial", "partial");
    write_file(twinp_repo / "snapshots" / "snap" / ".download_manifest.json",
               R"({"files":[{"name":"x.gguf"}]})");

    // The same twin pair, but the manifest records which model started the
    // download. A name settles what no file name can, so the leftovers of
    // twinown-a's own cancelled download are not the twin's to keep.
    fs::path twinown_repo = hf_root / "models--org--twinown";
    write_file(twinown_repo / "snapshots" / "snap" / "x.gguf.partial", "partial");
    write_file(twinown_repo / "snapshots" / "snap" / ".download_manifest.json",
               R"({"model_name":"user.twinown-a","files":[{"name":"x.gguf"}]})");

    // The same shape from the other side: the name belongs to the twin.
    fs::path twinoth_repo = hf_root / "models--org--twinoth";
    write_file(twinoth_repo / "snapshots" / "snap" / "x.gguf.partial", "partial");
    write_file(twinoth_repo / "snapshots" / "snap" / ".download_manifest.json",
               R"({"model_name":"user.twinoth-b","files":[{"name":"x.gguf"}]})");

    // bareown-b names the whole repo, so no file name can be told apart from
    // its claim. The manifest still names the download bareown-a's.
    fs::path bareown_repo = hf_root / "models--org--bareown";
    write_file(bareown_repo / "snapshots" / "snap" / "mine.gguf.partial", "partial");
    write_file(bareown_repo / "snapshots" / "snap" / ".download_manifest.json",
               R"({"model_name":"user.bareown-a","files":[{"name":"mine.gguf"}]})");

    // mansh-c holds the repo open, so the delete takes the selective path. The
    // manifest predates the recorded name and covers mansh-b's file, because a
    // later download wrote over the one mansh-a left.
    fs::path mansh_repo = make_repo(hf_root, "models--org--mansh", {"occupant.gguf"});
    write_file(mansh_repo / "snapshots" / "snap" / "a.gguf.partial", "partial");
    write_file(mansh_repo / "snapshots" / "snap" / ".download_manifest.json",
               R"({"files":[{"name":"b.gguf"}]})");

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

    // Selective cleanup walks every snapshot entry, so it only runs while the
    // repo is occupied. The sibling file below is what keeps it that way.

    // A path variant, the shape every Anima/Comfy-Org entry uses. The marker
    // sits under a snapshot revision, so the variant is not the whole path.
    fs::path path_repo = make_repo(hf_root, "models--org--pathvar", {"sibling.gguf"});
    write_file(path_repo / "snapshots" / "snap" / "split_files" / "diffusion_models" /
               "anima-base-v1.0.safetensors.partial", "partial");

    // A quantization-only variant, where a same-named directory is not a marker
    // and the real file carries the token in a sharded stem.
    fs::path shard_repo = make_repo(hf_root, "models--org--shard", {"sibling.gguf"});
    write_file(shard_repo / "snapshots" / "snap" / "Q4_K_M" / "weights.bin", "weights");
    write_file(shard_repo / "snapshots" / "snap" / "Q4_K_M-00001-of-00002.gguf.partial",
               "partial");

    // A sharded checkpoint resolves to its first shard, so that is the only
    // path the occupant scan can report. shardsib-b's other shard is just as
    // much in use, and a lone first shard is not a model anyone can load.
    fs::path shardsib_repo = make_repo(hf_root, "models--org--shardsib",
        {"model.safetensors", "sib-00001-of-00002.gguf", "sib-00002-of-00002.gguf"});

    // The same blind spot from the other side: selfshard-a is the model being
    // deleted, and removing only the path resolution names strands the rest.
    fs::path selfshard_repo = make_repo(hf_root, "models--org--selfshard",
        {"a-00001-of-00002.gguf", "a-00002-of-00002.gguf", "other.gguf"});

    // Some repos carry the quant token in the middle of the file name, which is
    // the shape resolve_gguf_path()'s quant-token fallback exists for. Matching
    // on a suffix alone calls this model's own download a stranger's.
    fs::path midquant_repo = hf_root / "models--org--midquant";
    write_file(midquant_repo / "snapshots" / "snap" / "Model-IQ4_XS-Q8nextn.gguf.partial",
               "partial");
    write_file(midquant_repo / "snapshots" / "snap" / ".download_manifest.json",
               R"({"files":[{"name":"Model-IQ4_XS-Q8nextn.gguf"}]})");

    // Resolution can come back empty while the files are still here, and then
    // nothing reports the repo as occupied. A repo with no refs/main is one way.
    fs::path norefs_repo = hf_root / "models--org--norefs";
    write_file(norefs_repo / "snapshots" / "snap" / "a.gguf");
    write_file(norefs_repo / "snapshots" / "snap" / "b.gguf");

    // A snapshot that still holds a manifest is the other way: every file in it
    // stays hidden until the download commits, including the complete ones that
    // an earlier download already committed there.
    fs::path coex_repo = make_repo(hf_root, "models--org--coex", {"a.gguf"});
    write_file(coex_repo / "snapshots" / "snap" / "b.gguf.partial", "partial");
    write_file(coex_repo / "snapshots" / "snap" / ".download_manifest.json",
               R"({"model_name":"user.coex-b","files":[{"name":"b.gguf"}]})");

    // orph-a stopped first and left a partial. orph-b then started in the same
    // snapshot and wrote over the manifest, so the only name here is orph-b's.
    // A name proves whose download the manifest describes. It does not make
    // every unfinished file in the repo that model's to take.
    fs::path orph_repo = hf_root / "models--org--orph";
    write_file(orph_repo / "snapshots" / "snap" / "a.gguf.partial", "partial");
    write_file(orph_repo / "snapshots" / "snap" / "b.gguf.partial", "partial");
    write_file(orph_repo / "snapshots" / "snap" / ".download_manifest.json",
               R"({"model_name":"user.orph-b","files":[{"name":"b.gguf"}]})");

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
        {"twinp-a", json{{"checkpoint", "org/twinp:x.gguf"}, {"recipe", "llamacpp"}}},
        {"twinp-b", json{{"checkpoint", "org/twinp:x.gguf"}, {"recipe", "llamacpp"}}},
        {"twinown-a", json{{"checkpoint", "org/twinown:x.gguf"}, {"recipe", "llamacpp"}}},
        {"twinown-b", json{{"checkpoint", "org/twinown:x.gguf"}, {"recipe", "llamacpp"}}},
        {"twinoth-a", json{{"checkpoint", "org/twinoth:x.gguf"}, {"recipe", "llamacpp"}}},
        {"twinoth-b", json{{"checkpoint", "org/twinoth:x.gguf"}, {"recipe", "llamacpp"}}},
        {"bareown-a", json{{"checkpoint", "org/bareown:mine.gguf"}, {"recipe", "llamacpp"}}},
        {"bareown-b", json{{"checkpoint", "org/bareown"}, {"recipe", "llamacpp"}}},
        {"mansh-a", json{{"checkpoint", "org/mansh:a.gguf"}, {"recipe", "llamacpp"}}},
        {"mansh-b", json{{"checkpoint", "org/mansh:b.gguf"}, {"recipe", "llamacpp"}}},
        {"mansh-c", json{{"checkpoint", "org/mansh:occupant.gguf"}, {"recipe", "llamacpp"}}},
        {"quant-a", json{{"checkpoint", "org/quant:Q4_K_M"}, {"recipe", "llamacpp"}}},
        {"whole-a", json{{"checkpoint", "org/whole"}, {"recipe", "llamacpp"}}},
        {"wholeshared-a", json{{"checkpoint", "org/wholeshared"}, {"recipe", "llamacpp"}}},
        {"wholeshared-b", json{{"checkpoint", "org/wholeshared:b.gguf"},
                               {"recipe", "llamacpp"}}},
        {"pathvar-a", json{{"checkpoint",
                            "org/pathvar:split_files/diffusion_models/"
                            "anima-base-v1.0.safetensors"},
                           {"recipe", "llamacpp"}}},
        {"pathvar-b", json{{"checkpoint", "org/pathvar:sibling.gguf"},
                           {"recipe", "llamacpp"}}},
        {"shard-a", json{{"checkpoint", "org/shard:Q4_K_M"}, {"recipe", "llamacpp"}}},
        {"shard-b", json{{"checkpoint", "org/shard:sibling.gguf"},
                         {"recipe", "llamacpp"}}},
        {"adopted-a", json{{"checkpoint", "org/adopted:a.gguf"}, {"recipe", "llamacpp"}}},
        {"shardsib-a", json{{"checkpoint", "org/shardsib"}, {"recipe", "thinksound"}}},
        {"shardsib-b", json{{"checkpoint", "org/shardsib:sib-00001-of-00002.gguf"},
                            {"recipe", "llamacpp"}}},
        {"selfshard-a", json{{"checkpoint", "org/selfshard:a-00001-of-00002.gguf"},
                             {"recipe", "llamacpp"}}},
        {"selfshard-b", json{{"checkpoint", "org/selfshard:other.gguf"},
                             {"recipe", "llamacpp"}}},
        {"midquant-a", json{{"checkpoint", "org/midquant:IQ4_XS"}, {"recipe", "llamacpp"}}},
        {"adoptleft-a", json{{"checkpoint", "org/adoptleft:Q4_K_M"}, {"recipe", "llamacpp"}}},
        {"adopt2-a", json{{"checkpoint", "org/adopt2"}, {"recipe", "thinksound"}}},
        {"adopt2-b", json{{"checkpoints", json{{"main", "org/adopt2:b.gguf"},
                                               {"vae", "org/adopt2:vae.bin"}}},
                          {"recipe", "llamacpp"}}},
        {"orph-a", json{{"checkpoint", "org/orph:a.gguf"}, {"recipe", "llamacpp"}}},
        {"orph-b", json{{"checkpoint", "org/orph:b.gguf"}, {"recipe", "llamacpp"}}},
        {"norefs-a", json{{"checkpoint", "org/norefs:a.gguf"}, {"recipe", "llamacpp"}}},
        {"norefs-b", json{{"checkpoint", "org/norefs:b.gguf"}, {"recipe", "llamacpp"}}},
        {"coex-a", json{{"checkpoint", "org/coex:a.gguf"}, {"recipe", "llamacpp"}}},
        {"coex-b", json{{"checkpoint", "org/coex:b.gguf"}, {"recipe", "llamacpp"}}},
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

        // Same pair mid-download. Matching this model's variant is not enough
        // to call the marker this model's alone: twinp-b is still waiting on
        // the very same file, and resuming is all it has left.
        manager.delete_model("user.twinp-a");
        check("a partial two models resolve to is not removed with one of them",
              fs::exists(twinp_repo / "snapshots" / "snap" / "x.gguf.partial"));
        check("the manifest that makes that partial resumable survives too",
              fs::exists(twinp_repo / "snapshots" / "snap" / ".download_manifest.json"));

        // The same pair once the manifest records a name. A catalog entry that
        // merely names the same file has never asked for it, and the name on
        // the download says so. The disk space has to come back.
        manager.delete_model("user.twinown-a");
        check("a named download is reclaimed despite an identical-variant twin",
              !fs::exists(twinown_repo));

        // The twin started this one, so none of it is this delete's to take.
        manager.delete_model("user.twinoth-a");
        check("a download named for another model survives the delete",
              fs::exists(twinoth_repo / "snapshots" / "snap" / "x.gguf.partial"));
        check("that model's manifest survives with it",
              fs::exists(twinoth_repo / "snapshots" / "snap" / ".download_manifest.json"));

        // A sibling that claims the whole repo claims every marker in it. The
        // name on the manifest is the only thing that can tell them apart.
        manager.delete_model("user.bareown-a");
        check("a named download is reclaimed despite a variantless sibling",
              !fs::exists(bareown_repo));

        // mansh-c keeps the repo alive, so only mansh-a's own partial goes. The
        // manifest beside it is mansh-b's resume marker, not mansh-a's.
        manager.delete_model("user.mansh-a");
        check("the deleted model's own partial is still removed",
              !fs::exists(mansh_repo / "snapshots" / "snap" / "a.gguf.partial"));
        check("a manifest another model resumes from is not removed with it",
              fs::exists(mansh_repo / "snapshots" / "snap" / ".download_manifest.json"));
        check("the occupying model's file survives that cleanup",
              fs::exists(mansh_repo / "snapshots" / "snap" / "occupant.gguf"));

        // The manifest names orph-b, so the repo looks like orph-b's alone. The
        // partial orph-a left behind is older than that name and is still the
        // only thing orph-a has to resume from.
        manager.delete_model("user.orph-b");
        check("a name on the manifest does not claim an older model's partial",
              fs::exists(orph_repo / "snapshots" / "snap" / "a.gguf.partial"));
        check("the named model's own partial is still removed",
              !fs::exists(orph_repo / "snapshots" / "snap" / "b.gguf.partial"));

        // Nothing resolves in this repo, so nothing reports it as occupied. The
        // files are all still here, and a complete file is somebody's.
        manager.delete_model("user.norefs-a");
        check("a repo with no refs/main does not lose a sibling's complete file",
              fs::exists(norefs_repo / "snapshots" / "snap" / "b.gguf"));

        // coex-a finished long ago. Its file is hidden only because coex-b's
        // download parked a manifest in the same snapshot folder. Cancelling and
        // deleting coex-b must not take coex-a's file with it.
        manager.delete_model("user.coex-b");
        check("a complete file hidden by another model's manifest survives",
              fs::exists(coex_repo / "snapshots" / "snap" / "a.gguf"));

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

        // The variant spells out a repo-relative path, but the marker also
        // carries the snapshot revision in front of it.
        manager.delete_model("user.pathvar-a");
        check("a marker under a snapshot revision matches a path variant",
              !fs::exists(path_repo / "snapshots" / "snap" / "split_files" /
                          "diffusion_models" / "anima-base-v1.0.safetensors.partial"));
        check("the occupying model's file survives a path-variant cleanup",
              fs::exists(path_repo / "snapshots" / "snap" / "sibling.gguf"));

        // Selective cleanup touches resume markers and nothing else. A folder
        // named after the variant is not one, and removing it as a file throws.
        manager.delete_model("user.shard-a");
        check("a directory named after the variant survives selective cleanup",
              fs::exists(shard_repo / "snapshots" / "snap" / "Q4_K_M" / "weights.bin"));
        check("a flat shard marker still matches its quantization variant",
              !fs::exists(shard_repo / "snapshots" / "snap" /
                          "Q4_K_M-00001-of-00002.gguf.partial"));
        check("the occupying model's file survives a shard cleanup",
              fs::exists(shard_repo / "snapshots" / "snap" / "sibling.gguf"));

        // shardsib-a's "main" is the snapshot folder. Clearing it must respect
        // the whole of shardsib-b's shard family, not just the one shard that
        // resolution happens to name.
        manager.delete_model("user.shardsib-a");
        check("an occupying sibling keeps the shard resolution names",
              fs::exists(shardsib_repo / "snapshots" / "snap" / "sib-00001-of-00002.gguf"));
        check("an occupying sibling keeps the rest of its shard family",
              fs::exists(shardsib_repo / "snapshots" / "snap" / "sib-00002-of-00002.gguf"));

        // selfshard-b occupies the repo, so this delete takes the variant-only
        // road. Every shard of selfshard-a still has to go.
        manager.delete_model("user.selfshard-a");
        check("a sharded model gives up the shard resolution names",
              !fs::exists(selfshard_repo / "snapshots" / "snap" / "a-00001-of-00002.gguf"));
        check("a sharded model gives up the rest of its own family",
              !fs::exists(selfshard_repo / "snapshots" / "snap" / "a-00002-of-00002.gguf"));
        check("the occupying model's file survives a sharded delete",
              fs::exists(selfshard_repo / "snapshots" / "snap" / "other.gguf"));

        // Nothing else names this repo, so the interrupted download can only be
        // this model's own, however the quant token sits in the file name.
        manager.delete_model("user.midquant-a");
        check("a quant token inside the file name still marks the model's own download",
              !fs::exists(midquant_repo));
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

    // An --extra-models-dir cache that a catalog model adopts only after the
    // model cache was built. Resolution then moves the model outside the managed
    // cache, so an ownership check reading the older answer would wave the
    // delete through and the removal would land on user-managed files.
    {
        fs::path extra_root = temp / "extra";
        fs::create_directories(extra_root);

        // The constructor sets the directory without starting a watcher, so
        // nothing invalidates the cache when the files below appear.
        ModelManager manager(path_to_utf8(extra_root));
        (void)manager.get_model_info("user.adopted-a");  // nothing on disk yet

        fs::path adopted_repo = extra_root / "models--org--adopted";
        write_file(adopted_repo / "snapshots" / "snap" / "a.gguf");
        write_file(adopted_repo / "refs" / "main", "snap");

        manager.delete_model("user.adopted-a");
        check("an adopted external file is not deleted through its catalog id",
              fs::exists(adopted_repo / "snapshots" / "snap" / "a.gguf"));
    }

    // The same adoption, but this model's own cancelled download also left
    // artifacts in the managed cache. Refusing to follow resolution out of that
    // cache must not become a refusal to clean the cache the model does own.
    // A registered model keeps its catalog entry in this situation (see
    // test_021yj_extra_cache_keeps_registered_identity); a user model has
    // nothing here but its registration, so the delete still takes that.
    {
        fs::path extra_root = temp / "extra_e";
        fs::path managed = hf_root / "models--org--adoptleft";
        write_file(managed / "snapshots" / "snap" / "Model-Q4_K_M.gguf.partial", "partial");
        write_file(managed / "snapshots" / "snap" / ".download_manifest.json",
                   R"({"files":[{"name":"Model-Q4_K_M.gguf"}]})");
        fs::path ext = extra_root / "models--org--adoptleft";
        write_file(ext / "snapshots" / "snap" / "Model-Q4_K_M.gguf");
        write_file(ext / "refs" / "main", "snap");

        ModelManager manager(path_to_utf8(extra_root));
        bool threw = false;
        try {
            manager.delete_model("user.adoptleft-a");
        } catch (const std::exception& e) {
            threw = true;
            std::printf("    [delete threw] %s\n", e.what());
        }
        check("a user model pointing outside the managed cache is still deleted",
              !threw);
        check("an adopted external copy survives a delete that has work to do",
              fs::exists(ext / "snapshots" / "snap" / "Model-Q4_K_M.gguf"));
        check("the managed leftovers of an adopted model are still reclaimed",
              !fs::exists(managed / "snapshots" / "snap" / "Model-Q4_K_M.gguf.partial"));
    }

    // The same adoption for a registered model, which keeps its catalog
    // identity and refuses the delete. An error must report a server that did
    // nothing, so the refusal has to come before the managed cache is touched.
    {
        const std::string registered = "Tiny-Test-Model-GGUF";
        const std::string repo_dir = "models--unsloth--gemma-3-270m-it-GGUF";
        const std::string gguf = "gemma-3-270m-it-UD-IQ2_M.gguf";

        fs::path extra_root = temp / "extra_r";
        fs::path managed = hf_root / repo_dir;
        write_file(managed / "snapshots" / "snap" / (gguf + ".partial"), "partial");
        write_file(managed / "snapshots" / "snap" / ".download_manifest.json",
                   R"({"model_name":")" + registered + R"(","files":[{"name":")" + gguf + R"("}]})");
        fs::path ext = extra_root / repo_dir;
        write_file(ext / "snapshots" / "snap" / gguf);
        write_file(ext / "refs" / "main", "snap");

        ModelManager manager(path_to_utf8(extra_root));
        if (!manager.model_exists(registered)) {
            std::printf("[SKIP] %s is no longer in the catalog\n", registered.c_str());
        } else {
            bool threw = false;
            try {
                manager.delete_model(registered);
            } catch (const std::exception&) {
                threw = true;
            }
            check("a registered model outside its managed cache is refused", threw);
            check("the refusal leaves the external files alone",
                  fs::exists(ext / "snapshots" / "snap" / gguf));
            check("the refusal removes nothing from the managed cache either",
                  fs::exists(managed / "snapshots" / "snap" / (gguf + ".partial")));
        }
    }

    // adopt2-b is served from the external cache, because its vae never landed
    // in the managed one. Where a model is served from says nothing about which
    // files it still holds here, and b.gguf in the managed cache is one of them.
    {
        fs::path extra_root = temp / "extra_b";
        fs::path managed = hf_root / "models--org--adopt2";
        write_file(managed / "snapshots" / "snap" / "model.safetensors");
        write_file(managed / "snapshots" / "snap" / "b.gguf");
        write_file(managed / "refs" / "main", "snap");
        fs::path ext = extra_root / "models--org--adopt2";
        write_file(ext / "snapshots" / "snap" / "b.gguf");
        write_file(ext / "snapshots" / "snap" / "vae.bin", "VAE");
        write_file(ext / "refs" / "main", "snap");

        ModelManager manager(path_to_utf8(extra_root));
        manager.delete_model("user.adopt2-a");
        check("a sibling served from extra_models_dir keeps its managed file",
              fs::exists(managed / "snapshots" / "snap" / "b.gguf"));
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
