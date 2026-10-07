#include "lemon/server/config_effects.h"

#include <string>
#include <vector>

#include <lemon/utils/aixlog.hpp>

#include "lemon/backend_manager.h"
#include "lemon/logging_config.h"
#include "lemon/model_manager.h"
#include "lemon/router.h"
#include "lemon/runtime_config.h"
#include "lemon/server/http_listener.h"
#include "lemon/system_info.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"

namespace lemon {

using json = nlohmann::json;

void ConfigEffects::apply(const std::string& key, const json& value) {
    RuntimeConfig& config = *ctx_.config;

    if (key == "port" || key == "host" || key == "websocket_port" ||
        key == "broadcast" || key == "no_broadcast") {
        ctx_.listener->rebind();
    } else if (key == "log_level" || key == "log_file" || key == "log_max_file_size_mb" ||
               key == "log_max_files") {
        LogRotationConfig rot_cfg;
        rot_cfg.file_mode = config.log_file();
        rot_cfg.max_file_size_mb = config.log_max_file_size_mb();
        rot_cfg.max_files = config.log_max_files();
        LOG(INFO, "Server") << "Logging configuration updated (level=" << config.log_level()
                            << ", file=" << rot_cfg.file_mode
                            << ", max_size=" << rot_cfg.max_file_size_mb << "MB"
                            << ", max_files=" << rot_cfg.max_files << ")" << std::endl;
        reconfigure_application_logging(config.log_level(), rot_cfg);
    } else if (key == "global_timeout") {
        long timeout = config.global_timeout();
        LOG(INFO, "Server") << "Global timeout set to: " << timeout << "s" << std::endl;
        utils::HttpClient::set_default_timeout(timeout);
    } else if (key == "download_rate_limit") {
        const int64_t bps = config.download_rate_limit_bytes_per_second();
        if (bps > 0) {
            LOG(INFO, "Server") << "Download rate limit enabled at " << bps << " B/s" << std::endl;
        } else {
            LOG(INFO, "Server") << "Download rate limit disabled" << std::endl;
        }
        utils::HttpClient::set_download_rate_limit(bps);
    } else if (key == "allowed_origins") {
        LOG(INFO, "Server") << "Allowed origins updated: " << config.allowed_origins() << std::endl;
    } else if (key == "extra_models_dir") {
        std::string dir = config.extra_models_dir();
        LOG(INFO, "Server") << "Extra models dir changed to: " << dir << std::endl;
        ctx_.model_manager->set_extra_models_dir(dir);
    } else if (key == "models_dir") {
        std::string dir = config.models_dir();
        LOG(INFO, "Server") << "Models dir changed to: " << dir << std::endl;
        utils::set_models_dir(dir);
        ctx_.model_manager->invalidate_models_cache();
    } else if (key == "telemetry") {
        if (value.is_object()) {
            if (value.contains("enabled")) {
                bool enabled = config.telemetry_enabled();
                LOG(INFO, "Server") << "Telemetry " << (enabled ? "enabled" : "disabled") << std::endl;
            }
            if (value.contains("otlp") && value["otlp"].is_object() && value["otlp"].contains("endpoint")) {
                LOG(INFO, "Server") << "Telemetry endpoint changed to: " << config.telemetry_otlp_endpoint() << std::endl;
            }
        }
    } else if (value.is_object()) {
        // Nested backend section change (llamacpp / whispercpp / sdcpp / ryzenai / kokoro).
        // Recipe defaults (e.g. default_backend) are derived from these settings, so
        // drop the memoized recipes so the next /system-info recomputes them.
        SystemInfoCache::invalidate_recipes();
        for (auto& [sub_key, sub_value] : value.items()) {
            if (sub_key.size() >= 4
                && sub_key.compare(sub_key.size() - 4, 4, "_bin") == 0) {
                apply_bin_change(key, sub_key,
                                 sub_value.is_string() ? sub_value.get<std::string>() : "");
            }
        }
    }
}

void ConfigEffects::apply_bin_change(const std::string& section,
                                     const std::string& bin_key,
                                     const std::string& new_value) {
    Router& router = *ctx_.router;
    std::string recipe = RuntimeConfig::config_section_to_recipe(section);

    // bin_key is "<backend>_bin" — strip the suffix to get the backend name
    // expected by install_backend / find_external_backend_binary.
    std::string backend = bin_key.substr(0, bin_key.size() - 4);

    // The "server_bin" key (as in ryzenai.server_bin) is not consumed by the
    // current install flow, so skip the hot-swap rather than attempt an install
    // that won't help.
    if (backend == "server") {
        LOG(WARNING, "Server") << section << "." << bin_key
                               << " is not consumed by the install flow; "
                                  "no hot-swap performed." << std::endl;
        return;
    }

    LOG(INFO, "Server") << "*_bin config changed: " << section << "." << bin_key
                        << " = '" << new_value << "' — hot-swapping "
                        << recipe << ":" << backend << std::endl;

    // Snapshot loaded models on this (recipe, backend). A model whose options
    // do not pin a backend (mb empty) is treated as potentially affected since
    // it could resolve to the changed backend on next load.
    struct Saved {
        std::string name;
        RecipeOptions opts;
    };
    std::vector<Saved> previously_loaded;
    auto loaded = router.get_all_loaded_models();
    std::string backend_option_key = recipe + "_backend";
    for (const auto& m : loaded) {
        if (m.value("recipe", "") != recipe) continue;
        std::string mb;
        if (m.contains("recipe_options") && m["recipe_options"].contains(backend_option_key)) {
            mb = m["recipe_options"].value(backend_option_key, "");
        }
        if (!mb.empty() && mb != backend) continue;
        std::string name = m.value("model_name", "");
        if (name.empty()) continue;
        previously_loaded.push_back({name, router.get_model_recipe_options(name)});
    }

    for (const auto& s : previously_loaded) {
        LOG(INFO, "Server") << "Unloading " << s.name
                            << " before installing new " << recipe << ":" << backend
                            << " binary" << std::endl;
        try {
            router.unload_model(s.name);
        } catch (const std::exception& e) {
            LOG(WARNING, "Server") << "Failed to unload " << s.name << ": " << e.what() << std::endl;
        }
    }

    // Install the new binary. install_from_github bails early when the user's
    // value resolves to a path (find_external_backend_binary returns it). When
    // version.txt mismatches the resolved version, the install dir is wiped
    // and re-downloaded.
    try {
        ctx_.backend_manager->install_backend(recipe, backend);
    } catch (const std::exception& e) {
        LOG(WARNING, "Server") << "install_backend(" << recipe << ":" << backend
                               << ") failed after *_bin change: " << e.what() << std::endl;
    }

    // Best-effort reload of previously-loaded models on the new binary.
    for (const auto& s : previously_loaded) {
        try {
            auto info = ctx_.model_manager->get_model_info(s.name);
            router.load_model(s.name, info, s.opts, true);
            LOG(INFO, "Server") << "Reloaded " << s.name << " on new "
                                << recipe << ":" << backend << " binary" << std::endl;
        } catch (const std::exception& e) {
            LOG(WARNING, "Server") << "Failed to reload " << s.name
                                   << " after *_bin hot-swap: " << e.what() << std::endl;
        }
    }

    SystemInfoCache::invalidate_recipes();
    ctx_.model_manager->invalidate_models_cache();
}

} // namespace lemon
