#include <lemon/models/update_scan.h>

#include <lemon/model_manager.h>
#include <lemon/models/artifact_source.h>
#include <lemon/models/artifact_store.h>
#include <lemon/models/store_snapshot.h>
#include <lemon/runtime_config.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using lemon::ModelInfo;
using lemon::models::ArtifactSource;
using lemon::models::ScanDetermination;
using lemon::models::StoreSnapshot;
using lemon::models::UpdateScanDeps;
using lemon::models::UpdateScanResult;

namespace {

int failures = 0;

void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

void set_env_var(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

ModelInfo registry_model(const std::string& name, const std::string& recipe) {
    ModelInfo info;
    info.model_name = name;
    info.recipe = recipe;
    info.downloaded = true;
    info.checkpoints["main"] = "lemonade-tests/no-such-repo";
    return info;
}

// Every eligibility rule the scan applies, one model each.
std::shared_ptr<StoreSnapshot> make_store(const std::string& public_name) {
    auto snapshot = std::make_shared<StoreSnapshot>();
    snapshot->valid = true;

    snapshot->models["eligible"] = registry_model("eligible", "llamacpp");
    snapshot->models["flm"] = registry_model("flm", "flm");
    snapshot->models["cloud"] = registry_model("cloud", "cloud");

    ModelInfo imported = registry_model("imported", "llamacpp");
    imported.source = "local_path";
    snapshot->models["imported"] = imported;

    ModelInfo uncheckpointed = registry_model("uncheckpointed", "llamacpp");
    uncheckpointed.checkpoints.clear();
    snapshot->models["uncheckpointed"] = uncheckpointed;

    ModelInfo absent = registry_model("absent", "llamacpp");
    absent.downloaded = false;
    snapshot->models["absent"] = absent;

    snapshot->canonical_public_names["eligible"] = public_name;
    snapshot->public_model_aliases[public_name] = "eligible";
    return snapshot;
}

struct Probe {
    std::shared_ptr<StoreSnapshot> snapshot;
    int ensure_store_calls = 0;
    bool published = false;
    ScanDetermination settled;
    std::vector<std::string> phases;
};

// An artifact store rooted at the test directory, empty as the day it was built.
struct Cache {
    explicit Cache(std::filesystem::path r) : root(std::move(r)) {}
    std::filesystem::path root;
    lemon::models::ModelArtifactStore artifacts{lemon::models::ModelArtifactStore::Hooks{
        [this] { return root.string(); },
        [](const ModelInfo&, const std::string&) -> std::unique_ptr<ArtifactSource> {
            return nullptr;
        },
        [] { return std::string(); },
    }};
};

UpdateScanDeps make_deps(Probe& probe, Cache& cache) {
    UpdateScanDeps deps;
    deps.artifacts = &cache.artifacts;
    deps.ensure_store = [&probe] { ++probe.ensure_store_calls; };
    deps.snapshot = [&probe] {
        return std::const_pointer_cast<const StoreSnapshot>(probe.snapshot);
    };
    deps.cancel_requested = [] { return false; };
    deps.publish = [&probe](const ScanDetermination& settled, UpdateScanResult&) {
        probe.published = true;
        probe.settled = settled;
    };
    deps.phase = [&probe](const std::string& phase) { probe.phases.push_back(phase); };
    return deps;
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

}  // namespace

int main() {
    // Nothing listens here, so every registry round trip fails at once. The scan
    // still has to finish, report the failure and settle nothing.
    set_env_var("HF_ENDPOINT", "http://127.0.0.1:9");

    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path base = fs::temp_directory_path() / ("lemonade_update_scan_test_" +
                                                        std::to_string(tick));
    std::error_code ec;
    fs::create_directories(base, ec);

    {
        lemon::RuntimeConfig offline(lemon::json{{"offline", true}});
        lemon::RuntimeConfig::set_global(&offline);

        Probe probe;
        probe.snapshot = make_store("eligible");
        Cache cache(base);
        UpdateScanDeps deps = make_deps(probe, cache);
        const UpdateScanResult result = lemon::models::run_update_scan(deps);

        check("offline scan reports nothing", result.updated_models.empty() &&
                                                  result.up_to_date_models.empty() &&
                                                  result.failed_models.empty());
        check("offline scan touches neither the store nor the publish",
              probe.ensure_store_calls == 0 && !probe.published);
        lemon::RuntimeConfig::set_global(nullptr);
    }

    {
        Probe probe;
        probe.snapshot = make_store("Shown-Name");
        Cache cache(base);
        UpdateScanDeps deps = make_deps(probe, cache);
        const UpdateScanResult result = lemon::models::run_update_scan(deps);

        check("only a downloaded registry checkpoint is examined",
              probe.settled.examined.size() == 1 &&
                  probe.settled.examined[0] == "eligible");
        check("an unreachable registry settles no staleness", probe.settled.verified.empty() &&
                                                                   probe.settled.updated.empty());
        check("a failed check is reported under the listed name",
              result.failed_models.count("Shown-Name") != 0);
        check("the phase hook names the model being checked",
              contains(probe.phases, "registry_check:eligible"));
        check("the scan publishes once per pass", probe.published);
    }

    {
        Probe probe;
        probe.snapshot = make_store("eligible");
        Cache cache(base);
        UpdateScanDeps deps = make_deps(probe, cache);
        const UpdateScanResult result = lemon::models::run_update_scan(deps, {"flm"});

        check("a target outside the eligible set examines nothing",
              probe.settled.examined.empty() && result.failed_models.empty());
    }

    {
        Probe probe;
        probe.snapshot = make_store("eligible");
        Cache cache(base);
        UpdateScanDeps deps = make_deps(probe, cache);
        deps.scan_override = [](const std::vector<std::string>&) {
            UpdateScanResult res;
            res.updated_models.push_back("overridden-updated");
            res.up_to_date_models.push_back("overridden-current");
            res.failed_models["overridden-failed"] = "forced";
            return res;
        };
        const UpdateScanResult result = lemon::models::run_update_scan(deps);

        check("the override result is returned verbatim",
              result.updated_models.size() == 1 && result.updated_models[0] == "overridden-updated" &&
                  result.up_to_date_models.size() == 1 &&
                  result.failed_models.count("overridden-failed") != 0);
        check("an overridden pass reads and publishes nothing",
              probe.ensure_store_calls == 0 && !probe.published);
        check("every reported model gets its phase, once",
              probe.phases.size() == 3);
    }

    fs::remove_all(base, ec);
    std::printf("%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
