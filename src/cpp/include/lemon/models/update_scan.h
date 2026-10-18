#pragma once

#include <lemon/models/registry_id.h>

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <filesystem>

namespace lemon {

struct ModelInfo;
class ModelRegistry;

namespace models {

struct StoreSnapshot;
class ModelStore;

struct UpdateScanResult {
    std::vector<std::string> updated_models;
    std::vector<std::string> up_to_date_models;
    std::map<std::string, std::string> failed_models;
};

// What one pass settled, in canonical ids. Reported names are the publisher's
// job, so the store write and the names returned for it cannot disagree.
struct ScanDetermination {
    std::set<std::string> verified;
    std::set<std::string> updated;
    std::vector<std::string> examined;
};

struct UpdateScanDeps {
    std::function<void()> ensure_store;
    std::function<std::shared_ptr<const StoreSnapshot>()> snapshot;
    std::function<std::string()> hf_cache_dir;
    std::function<std::string(const ModelInfo& info)> registry_source;
    // The snapshot id pull recorded for this model's selection. A fast path
    // only: the comparison baseline is the snapshot on disk.
    std::function<std::string(const std::filesystem::path& model_cache_dir,
                              const std::string& model_name,
                              const ModelInfo& info)> processed_snapshot_id;
    // Which registry answers for a source. Empty means the configured ones.
    std::function<const ModelRegistry&(RegistryId source)> registry_for;
    std::function<bool()> cancel_requested;
    std::function<void(const ScanDetermination&, UpdateScanResult&)> publish;
    std::function<void(const std::string& phase)> phase;
    std::function<UpdateScanResult(const std::vector<std::string>&)> scan_override;
};

UpdateScanResult run_update_scan(const UpdateScanDeps& deps,
                                 const std::vector<std::string>& targets = {});

// Publishing what a pass settled. A model whose check failed is reported as failed
// rather than as either state, and gets no flag.
void publish_scan_flags(ModelStore& store, const ScanDetermination& settled,
                        UpdateScanResult& result);

}  // namespace models
}  // namespace lemon
