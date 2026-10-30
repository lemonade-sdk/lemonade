#include "lemon/models/repository_provenance.h"

#include "lemon/backends/hf_cache_util.h"
#include "lemon/models/artifact_store.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/safe_fs.h"

#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;
using lemon::utils::safe_exists;

namespace lemon {
namespace models {

namespace {

constexpr const char* kLog = "RepositoryProvenance";
constexpr const char kProvenanceFile[] = ".lemonade_registry.json";

}  // namespace

std::string read_active_snapshot_ref(const fs::path& repo_cache_dir) {
    return backends::hf_cache::read_ref_main(repo_cache_dir);
}

fs::path read_active_snapshot_path(const fs::path& repo_cache_dir) {
    const std::string ref = read_active_snapshot_ref(repo_cache_dir);
    if (ref.empty()) {
        return fs::path();
    }
    const fs::path snapshot = repo_cache_dir / "snapshots" / utils::path_from_utf8(ref);
    return safe_exists(snapshot) ? snapshot : fs::path();
}

void write_active_snapshot_ref(const fs::path& repo_cache_dir, const std::string& commit_hash) {
    backends::hf_cache::write_ref_main(repo_cache_dir, commit_hash);
}

void remove_unused_snapshot(const fs::path& repo_cache_dir, const std::string& snapshot_dir,
                            const std::string& active_ref) {
    if (repo_cache_dir.empty() || snapshot_dir.empty() || active_ref.empty()) {
        return;
    }

    const fs::path unused = utils::path_from_utf8(snapshot_dir);
    const fs::path active = repo_cache_dir / "snapshots" / utils::path_from_utf8(active_ref);
    std::error_code ec;
    if (!safe_exists(unused) || fs::equivalent(unused, active, ec)) {
        return;
    }
    ec.clear();
    fs::remove_all(unused, ec);
    if (ec) {
        LOG(WARNING, kLog) << "Could not remove unused snapshot: " << utils::path_to_utf8(unused)
                           << " (" << ec.message() << ")" << std::endl;
    }
}

// What was processed: the recipe plus every checkpoint it was asked for, so a row
// that changed shape does not reuse a snapshot that no longer matches it.
std::string processed_selection(const ModelInfo& info) {
    std::ostringstream selection;
    selection << info.recipe;
    for (const auto& [type, checkpoint] : info.checkpoints) {
        selection << '\n' << type << '=' << checkpoint;
    }
    return selection.str();
}

// The fingerprint of one checkpoint's slot: a re-pull that changed the string
// changed the bytes it wants.
std::string processed_checkpoint_selection(const std::string& type,
                                           const std::string& checkpoint) {
    return type + '=' + checkpoint;
}

std::string processed_snapshot_id(const fs::path& repo_cache_dir, const ModelInfo& info) {
    const fs::path provenance_path = repo_cache_dir / kProvenanceFile;
    if (!safe_exists(provenance_path)) {
        return "";
    }

    try {
        const json provenance = utils::JsonUtils::load_from_file(utils::path_to_utf8(provenance_path));
        if (!provenance.contains("processed_models") || !provenance["processed_models"].is_object()) {
            return "";
        }
        const auto& processed = provenance["processed_models"];
        const auto entry = processed.find(info.model_name);
        if (entry == processed.end() || !entry->is_object()) {
            return "";
        }
        if (utils::JsonUtils::get_or_default<std::string>(*entry, "selection", "") != processed_selection(info)) {
            return "";
        }
        return utils::JsonUtils::get_or_default<std::string>(*entry, "snapshot_id", "");
    } catch (const std::exception&) {
        return "";
    }
}

// v1 said: "this model's whole selection came from this repository's active
// snapshot," and only the main repository was allowed to write. v2 keeps that
// top-level shape for v1 readers forever and adds what the sentence could not
// say: which checkpoint of the model landed here. An auxiliary repository
// records only its own checkpoint, so a shared side cache knows every model
// that filled it without any of them claiming the whole model.
void record_processed_model(const fs::path& repo_cache_dir, const ModelInfo& info,
                              const Provenance& provenance, const std::string& checkpoint_type) {
    const fs::path provenance_path = repo_cache_dir / kProvenanceFile;
    const bool is_main = checkpoint_type.empty() || checkpoint_type == "main";

    json previous;
    json processed = json::object();
    if (safe_exists(provenance_path)) {
        try {
            previous = utils::JsonUtils::load_from_file(utils::path_to_utf8(provenance_path));
            if (previous.contains("processed_models") && previous["processed_models"].is_object()) {
                processed = previous["processed_models"];
            }
        } catch (const std::exception&) {
            // Invalid provenance is replaced after a successful download.
        }
    }

    json entry = json::object();
    if (is_main) {
        // The v1 sentence, preserved for readers that stop here. When this pull
        // moved the model to a new snapshot, the one it leaves behind stays named.
        const auto last = processed.find(info.model_name);
        if (last != processed.end() && last->is_object() && last->contains("snapshot_id") &&
            last->at("snapshot_id").is_string() &&
            last->at("snapshot_id").get<std::string>() != provenance.active_snapshot_id) {
            entry["previous_snapshot_id"] = last->at("snapshot_id");
        }
        entry["selection"] = processed_selection(info);
        entry["snapshot_id"] = provenance.processed_snapshot_id;
    }

    // v2: what this model processed from this repository, checkpoint by checkpoint.
    json checkpoints = json::object();
    const auto own = processed.find(info.model_name);
    if (own != processed.end() && own->is_object() && own->contains("checkpoints") &&
        own->at("checkpoints").is_object()) {
        checkpoints = own->at("checkpoints");
    }
    const std::string checkpoint = info.checkpoint(is_main ? "main" : checkpoint_type);
    checkpoints[is_main ? "main" : checkpoint_type] = {
        {"selection", processed_checkpoint_selection(checkpoint_type.empty() ? "main" : checkpoint_type,
                                                     checkpoint)},
        {"snapshot_id", provenance.processed_snapshot_id},
    };
    entry["checkpoints"] = std::move(checkpoints);

    processed[info.model_name] = std::move(entry);

    json out{{"source", provenance.source},
             {"repo_id", provenance.repo_id},
             {"revision", provenance.revision},
             {"snapshot_id", provenance.active_snapshot_id},
             {"processed_models", std::move(processed)}};
    if (!is_main) out["version"] = 2;
    utils::JsonUtils::save_to_file(out, utils::path_to_utf8(provenance_path));
}

}  // namespace models
}  // namespace lemon
