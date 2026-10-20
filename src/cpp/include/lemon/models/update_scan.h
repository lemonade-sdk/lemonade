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

namespace models {

struct ArtifactRef;
class FetchableRepository;
class ModelArtifactStore;
struct StoreSnapshot;
class ModelStore;
class ArtifactSourceResolver;

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
    // The cache the snapshots on disk live in, as itself: baseline reads and the
    // processed-snapshot fast path both belong to it.
    const ModelArtifactStore* artifacts;

    std::function<void()> ensure_store;
    std::function<std::shared_ptr<const StoreSnapshot>()> snapshot;
    // The probe repository each target is checked against comes from the resolver.
    const ArtifactSourceResolver* resolver = nullptr;
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
