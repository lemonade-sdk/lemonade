#pragma once

#include "lemon/directory_watcher.h"
#include "lemon/routing_policy.h"
#include "lemon/routing_policy_parser.h"

#include <map>
#include <memory>
#include <string>

namespace lemon {

// Directory-backed cache of compiled collection.router engines. A reload builds
// a complete immutable snapshot and atomically swaps the shared_ptr, so readers
// either see the old valid set or the new valid set.
//
// This is the module-level hot-reload implementation for #2383. It is not wired
// into Server request dispatch yet: #2385 owns attaching a store/registry to the
// live server lifecycle and using its engines to route OpenAI requests.
class RoutingPolicyStore {
public:
    struct Snapshot {
        std::map<std::string, std::shared_ptr<const RoutingPolicyEngine>> engines;
        std::map<std::string, std::string> errors;
    };

    RoutingPolicyStore(std::string directory,
                       ClassifierServices services,
                       RoutingPolicyParseOptions parse_options = {});
    ~RoutingPolicyStore();

    std::shared_ptr<const Snapshot> reload();
    void start_watching();
    void stop_watching();

    std::shared_ptr<const RoutingPolicyEngine> get_engine(const std::string& model_name) const;
    std::shared_ptr<const Snapshot> snapshot() const;

private:
    std::shared_ptr<const Snapshot> load_directory() const;

    std::string directory_;
    ClassifierServices services_;
    RoutingPolicyParseOptions parse_options_;
    std::unique_ptr<DirectoryWatcher> watcher_;
    // TODO(C++20): replace std::atomic_load/store free functions with a
    // std::atomic<std::shared_ptr<const Snapshot>> member (deprecated in C++20).
    std::shared_ptr<const Snapshot> snapshot_;
};

} // namespace lemon
