#pragma once

// A test's fixtures must not depend on what the host happens to run: a machine
// without a GPU, or with no backend engines installed, hides recipes, and a
// published-set assertion then describes the runner instead of the fixture.
// Capability is decided by injected machine facts in test_filter_hardware.

#include <lemon/config_file.h>
#include <lemon/runtime_config.h>

#include <memory>

class ShowAllModels {
  public:
    ShowAllModels() {
        lemon::json config = lemon::ConfigFile::base_defaults();
        config["disable_model_filtering"] = true;
        config_ = std::make_unique<lemon::RuntimeConfig>(config);
        lemon::RuntimeConfig::set_global(config_.get());
    }

    ~ShowAllModels() { lemon::RuntimeConfig::set_global(nullptr); }

    ShowAllModels(const ShowAllModels&) = delete;
    ShowAllModels& operator=(const ShowAllModels&) = delete;

  private:
    std::unique_ptr<lemon::RuntimeConfig> config_;
};
