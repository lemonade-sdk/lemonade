#include <lemon/model_manager.h>
#include <lemon/models/catalog.h>
#include <lemon/models/registry_descriptors.h>
#include <lemon/utils/path_utils.h>

#include "support/test_state_isolation.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using lemon::ModelInfo;
using lemon::ModelManager;
using lemon::ModelManagerConfig;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

namespace {

ModelInfo row(const std::string& name) {
    ModelInfo info;
    info.model_name = name;
    info.recipe = "llamacpp";
    info.checkpoints["main"] = "org/" + name + ":a.gguf";
    info.suggested = true;
    info.labels = {"chat"};
    return info;
}

bool answers(ModelManager& manager, const std::string& name) {
    try {
        return manager.get_model_info(name).recipe == "llamacpp";
    } catch (const std::exception&) {
        return false;
    }
}

// Stands in for the second pass of a compose that yielded: the registries now report a
// row that the first pass could not know about.
struct Late {
    ModelManager* manager = nullptr;
    std::atomic<int> passes{0};
};

}  // namespace

int main() {
    const fs::path scratch = lemon::test::make_scratch_dir("compose_retry");
    lemon::utils::set_models_dir((scratch / "models").string());

    Late late;
    auto racy = std::make_shared<lemon::models::EnumeratedCatalog>(
        lemon::models::CatalogDescriptor{"racy"},
        [&late](const lemon::models::CatalogAccumulator&) {
            if (++late.passes == 2 && late.manager != nullptr) {
                // Something persisted while this set was being put together. A writer
                // persists and invalidates; it never patches the composed set.
                late.manager->invalidate_models_cache();
                return std::vector<ModelInfo>{row("Fixture-Model")};
            }
            if (late.passes == 1) return std::vector<ModelInfo>{row("Fixture-Model")};
            return std::vector<ModelInfo>{row("Fixture-Model"), row("Late-Model")};
        });

    ModelManagerConfig config;
    config.cache_dir = scratch.string();
    config.extra_catalogs = {racy};
    // The compose protocol is the question, not what this machine can run.
    config.availability = [](const std::map<std::string, ModelInfo>&) {
        lemon::models::BackendAvailability declared;
        declared.system_ram_gb = 64.0;
        declared.largest_mem_pool_gb = 48.0;
        return declared;
    };
    ModelManager manager(std::move(config));
    late.manager = &manager;

    manager.get_supported_models();
    {
        manager.invalidate_models_cache();
    }

    const int composed_before = late.passes;
    manager.get_supported_models();
    const int composed_by_now = late.passes;
    check("the refused compose is asked again", composed_by_now >= composed_before + 2);
    // A read after the call composing again is the failure this guards: the caller of
    // the composing call is told the change landed, so it must already be there.
    check("and the change it yielded to is answered", answers(manager, "Late-Model"));
    check("by that call, not by one it provoked", late.passes == composed_by_now);
    check("the row it already had survives the retry", answers(manager, "Fixture-Model"));

    if (g_failures > 0) {
        std::printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("\nAll compose-retry checks passed\n");
    return 0;
}
