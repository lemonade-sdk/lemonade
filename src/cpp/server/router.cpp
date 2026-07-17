#include "lemon/router.h"
#include "lemon/cloud_provider_registry.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/cloud/cloud_server.h"
#include "lemon/backends/llamacpp/llamacpp_server.h"
#include "lemon/backends/fastflowlm/fastflowlm_server.h"
#include "lemon/backends/ryzenai/ryzenai_server.h"
#include "lemon/backends/whispercpp/whispercpp_server.h"
#include "lemon/backends/moonshine/moonshine_server.h"
#include "lemon/backends/kokoro/kokoro_server.h"
#include "lemon/backends/sdcpp/sdcpp_server.h"
#include "lemon/backends/vllm/vllm_server.h"
#include "lemon/server_capabilities.h"
#include "lemon/streaming_proxy.h"
#include "lemon/error_types.h"
#include "lemon/recipe_options.h"
#include "lemon/auto_tune.h"
#include "telemetry.h"
#include <algorithm>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include "lemon/utils/aixlog.hpp"
#include "lemon/global_vram_monitor.h"
#include "lemon/eviction_engine.h"
#include "lemon/suspend_inhibitor.h"
#include "lemon/utils/http_client.h"

namespace lemon {

namespace {

// RAII: holds a suspend-inhibitor refcount for the duration of one inference,
// but only when the feature is enabled in config. Released on scope exit so all
// early-return/exception paths are covered.
class InhibitGuard {
public:
    InhibitGuard(SuspendInhibitor* inhibitor, bool enabled)
        : inhibitor_(enabled ? inhibitor : nullptr) {
        if (inhibitor_) inhibitor_->acquire();
    }
    ~InhibitGuard() {
        if (inhibitor_) inhibitor_->release();
    }
    InhibitGuard(const InhibitGuard&) = delete;
    InhibitGuard& operator=(const InhibitGuard&) = delete;

private:
    SuspendInhibitor* inhibitor_;
};

} // namespace

Router::Router(RuntimeConfig* config, ModelManager* model_manager, BackendManager* backend_manager)
    : config_(config), model_manager_(model_manager), backend_manager_(backend_manager) {

    int max = config_->max_loaded_models();
    if (max == -1) {
    LOG(DEBUG, "Router") << "Max loaded models per type: unlimited" << std::endl;
    } else {
    LOG(DEBUG, "Router") << "Max loaded models per type: " << max << std::endl;
    }

    vram_monitor_ = std::make_unique<GlobalVramMonitor>();
    eviction_engine_ = std::make_unique<EvictionEngine>(this, vram_monitor_.get());
    suspend_inhibitor_ = create_suspend_inhibitor();

    // Always start the monitor/engine threads; they are cheap no-ops until the
    // user opts in. The monitor skips the VRAM poll when auto_evict is disabled,
    // and the engine's per-server check skips models that haven't opted in.
    // (auto_evict can be toggled at runtime via /internal/set, so we cannot gate
    // thread creation on the construction-time config value.)
    vram_monitor_->start();
    eviction_engine_->start();
}

Router::~Router() {
    LOG(DEBUG, "Router") << "Destructor: stopping monitors and unloading all models" << std::endl;
    if (eviction_engine_) eviction_engine_->stop();
    if (vram_monitor_) vram_monitor_->stop();
    unload_model("");  // Unload all
}

void Router::set_cloud_registry(CloudProviderRegistry* registry) {
    cloud_registry_ = registry;
}

WrappedServer* Router::find_server_by_model_name(const std::string& model_name) const {
    WrappedServer* unavailable_match = nullptr;
    for (const auto& server : loaded_servers_) {
        if (server->get_model_name() != model_name) {
            continue;
        }
        if (server->is_backend_alive()) {
            return server.get();
        }
        if (!unavailable_match) {
            unavailable_match = server.get();
        }
    }
    return unavailable_match;
}

std::string Router::resolve_model_name(const std::string& model_name) const {
    return model_name.empty() ? model_name : model_manager_->resolve_model_name(model_name);
}

WrappedServer* Router::get_most_recent_server() const {
    WrappedServer* most_recent = nullptr;
    for (const auto& server : loaded_servers_) {
        if (!server->is_backend_alive()) {
            continue;
        }
        if (!most_recent || server->get_last_access_time() > most_recent->get_last_access_time()) {
            most_recent = server.get();
        }
    }
    return most_recent;
}

void Router::prune_unavailable_servers_locked() {
    std::vector<WrappedServer*> unavailable;
    for (const auto& server : loaded_servers_) {
        if (!server->is_backend_alive()) {
            unavailable.push_back(server.get());
        }
    }

    for (auto* server : unavailable) {
        LOG(WARNING, "Router") << "Pruning unavailable backend for model: "
                                << server->get_model_name()
                                << " (state=" << server->get_backend_health_state() << ")"
                                << std::endl;
        evict_server(server);
    }
}

bool Router::is_watchdog_reset_response(const json& response) const {
    if (!response.is_object() || !response.contains("error") || !response["error"].is_object()) {
        return false;
    }

    const auto& error = response["error"];
    if (error.contains("code") && error["code"] == "backend_watchdog_reset") {
        return true;
    }
    if (error.contains("details") && error["details"].is_object() &&
        error["details"].contains("code") &&
        error["details"]["code"] == "backend_watchdog_reset") {
        return true;
    }
    return false;
}

bool Router::reload_model_after_watchdog_reset(const std::string& requested_model, const RecipeOptions& options) {
    try {
        LOG(WARNING, "Router") << "Reloading model after backend watchdog reset: "
                                << requested_model << std::endl;
        auto info = model_manager_->get_model_info(requested_model);
        bool was_pinned = false;
        {
            std::lock_guard<std::mutex> lock(load_mutex_);
            auto* existing = find_server_by_model_name(requested_model);
            if (existing) {
                was_pinned = existing->is_pinned();
            }
        }
        load_model(requested_model, info, options, true, false, was_pinned);
        return true;
    } catch (const std::exception& e) {
        LOG(ERROR, "Router") << "Automatic reload after watchdog reset failed for "
                              << requested_model << ": " << e.what() << std::endl;
        return false;
    }
}

// Slot/eviction policy for a recipe, from its descriptor (default Standard).
// This is the recipe-static policy used for pre-load slot decisions.
static SlotPolicy slot_policy_for_recipe(const std::string& recipe) {
    if (const auto* desc = backends::descriptor_for(recipe)) {
        return desc->slot_policy;
    }
    return SlotPolicy::Standard;
}

static bool is_unmetered_recipe(const std::string& recipe) {
    return slot_policy_for_recipe(recipe) == SlotPolicy::Unmetered;
}

int Router::count_servers_by_type(ModelType type) const {
    int count = 0;
    for (const auto& server : loaded_servers_) {
        // Unmetered backends (cloud) consume no local memory and stay loaded for
        // free, so they are excluded from the slot accounting that drives LRU eviction.
        if (is_unmetered_recipe(server->get_recipe_options().get_recipe())) {
            continue;
        }
        if (server->is_backend_alive() && server->get_model_type() == type) {
            count++;
        }
    }
    return count;
}

WrappedServer* Router::find_lru_server_by_type(ModelType type) const {
    WrappedServer* lru = nullptr;

    for (const auto& server : loaded_servers_) {
        // Unmetered backends (cloud) are not eviction candidates; they have no
        // memory cost and reloading them is essentially free, but evicting them
        // throws away the cached api key/upstream-id binding for no benefit.
        if (is_unmetered_recipe(server->get_recipe_options().get_recipe())) {
            continue;
        }
        if (server->is_backend_alive() && server->get_model_type() == type) {
            if (server->is_pinned()) {
                continue;
            }
            if (!lru || server->get_last_access_time() < lru->get_last_access_time()) {
                lru = server.get();
            }
        }
    }

    return lru;
}

bool Router::has_npu_server() const {
    for (const auto& server : loaded_servers_) {
        if (server->is_backend_alive() && (server->get_device_type() & DEVICE_NPU)) {
            return true;
        }
    }
    return false;
}

WrappedServer* Router::find_npu_server() const {
    for (const auto& server : loaded_servers_) {
        if (server->is_backend_alive() && (server->get_device_type() & DEVICE_NPU)) {
            return server.get();
        }
    }
    return nullptr;
}

WrappedServer* Router::find_npu_server_by_recipe(const std::string& recipe) const {
    for (const auto& server : loaded_servers_) {
        if (server->is_backend_alive() &&
            (server->get_device_type() & DEVICE_NPU) &&
            server->get_recipe_options().get_recipe() == recipe) {
            return server.get();
        }
    }
    return nullptr;
}

WrappedServer* Router::find_coexisting_server_by_type(ModelType type) const {
    for (const auto& server : loaded_servers_) {
        if (server->is_backend_alive() &&
            slot_policy_for_recipe(server->get_recipe_options().get_recipe()) ==
                SlotPolicy::CoexistByType &&
            server->get_model_type() == type) {
            return server.get();
        }
    }
    return nullptr;
}

// Helper: Evict all NPU servers
void Router::evict_all_npu_servers() {
    std::vector<WrappedServer*> npu_servers;
    for (const auto& server : loaded_servers_) {
        if (server->is_backend_alive() && (server->get_device_type() & DEVICE_NPU)) {
            npu_servers.push_back(server.get());
        }
    }
    for (auto* server : npu_servers) {
        LOG(INFO, "Router") << "Evicting NPU server: " << server->get_model_name() << std::endl;
        evict_server(server);
    }
}

void Router::evict_server(WrappedServer* server, int timeout_seconds) {
    if (!server) return;

    std::string model_name = server->get_model_name();
    LOG(INFO, "Router") << "Evicting model: " << model_name << std::endl;

    // Wait for any ongoing inference to complete. For watchdog-reset/dead
    // backends the wait is bounded so recovery can continue, but the object must
    // not be destroyed while a request thread may still be using this raw
    // pointer. In that case we leave a tombstoned server in loaded_servers_;
    // future prune/load calls will remove it once the request unwinds.
    const int wait_timeout = server->is_backend_alive() ? timeout_seconds : EVICTION_TIMEOUT;
    const bool idle = server->wait_until_not_busy(wait_timeout);
    if (!idle) {
        LOG(WARNING, "Router") << "Deferring eviction for model " << model_name
                                << " because requests are still unwinding after "
                                << EVICTION_TIMEOUT << "s (state="
                                << server->get_backend_health_state() << ")"
                                << std::endl;
        return;
    }

    server->unload();

    // Remove from vector
    loaded_servers_.erase(
        std::remove_if(loaded_servers_.begin(), loaded_servers_.end(),
                      [server](const std::unique_ptr<WrappedServer>& s) {
                          return s.get() == server;
                      }),
        loaded_servers_.end()
    );

    LOG(INFO, "Router") << "Evicted model: " << model_name << std::endl;
}

void Router::evict_all_servers() {
    LOG(INFO, "Router") << "Evicting all models (" << loaded_servers_.size() << " total)" << std::endl;

    // Copy raw pointers first; evict_server may erase entries and move
    // unique_ptrs inside the vector, but the pointed-to WrappedServer objects
    // remain stable until their individual eviction completes. Busy/dead
    // servers are safely left as tombstones for a later prune pass.
    std::vector<WrappedServer*> servers;
    servers.reserve(loaded_servers_.size());
    for (const auto& server : loaded_servers_) {
        servers.push_back(server.get());
    }

    for (auto* server : servers) {
        evict_server(server, EVICTION_TIMEOUT);
    }

    LOG(INFO, "Router") << "Evict all completed. Remaining tombstoned models: "
                         << loaded_servers_.size() << std::endl;
}

void Router::simulate_vram_pressure(double pct) {
    if (vram_monitor_) {
        vram_monitor_->simulate_pressure(pct);
    }
}

std::unique_ptr<WrappedServer> Router::create_backend_server(const ModelInfo& model_info) {
    std::string log_level = config_->log_level();

    backends::BackendContext ctx;
    ctx.log_level = log_level;
    ctx.model_manager = model_manager_;
    ctx.backend_manager = backend_manager_;
    ctx.cloud_registry = cloud_registry_;
    ctx.model_info = &model_info;

    // The backend registry binds each recipe to its create() (see LEMON_BACKENDS).
    std::unique_ptr<WrappedServer> new_server = backends::create_server(model_info.recipe, ctx);
    if (new_server) {
        LOG(DEBUG, "Router") << "Created backend for recipe '" << model_info.recipe
                             << "' via registry" << std::endl;
        return new_server;
    }

    // Unknown recipe: fall back to llamacpp, preserving the historical default.
    LOG(DEBUG, "Router") << "No registered backend for recipe '" << model_info.recipe
                         << "', defaulting to LlamaCpp" << std::endl;
    return std::make_unique<backends::LlamaCppServer>(log_level, model_manager_, backend_manager_);
}

bool Router::begin_exclusive(std::atomic<bool>* cancel) {
    std::unique_lock<std::mutex> lock(load_mutex_);
    exclusive_active_ = true;
    exclusive_owner_ = std::this_thread::get_id();
    while (true) {
        if (cancel && cancel->load()) {
            exclusive_active_ = false;
            exclusive_owner_ = std::thread::id{};
            exclusive_cv_.notify_all();
            load_cv_.notify_all();
            return false;
        }
        if (is_loading_) {
            load_cv_.wait_for(lock, std::chrono::milliseconds(25));
            continue;
        }
        bool busy = false;
        for (const auto& server : loaded_servers_) {
            if (server->is_busy()) {
                busy = true;
                break;
            }
        }
        if (!busy) break;
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        lock.lock();
    }
    exclusive_cv_.notify_all();
    load_cv_.notify_all();
    return true;
}

void Router::end_exclusive() {
    std::lock_guard<std::mutex> lock(load_mutex_);
    exclusive_active_ = false;
    exclusive_owner_ = std::thread::id{};
    exclusive_cv_.notify_all();
    load_cv_.notify_all();
}

void Router::wait_for_slot_clearance(std::unique_lock<std::mutex>& lock) {
    exclusive_cv_.wait(lock, [&] {
        return !exclusive_active_ || exclusive_owner_ == std::this_thread::get_id();
    });
}

std::map<std::string, bool> Router::snapshot_loaded_models() const {
    std::lock_guard<std::mutex> lock(load_mutex_);
    std::map<std::string, bool> models;
    for (const auto& server : loaded_servers_)
        if (server->is_backend_alive()) models[server->get_model_name()] = server->is_pinned();
    return models;
}

std::map<std::string, json> Router::unload_job_models(const std::map<std::string, int>& owned_live,
                                                      const std::map<std::string, bool>& snapshot_pins) {
    std::unique_lock<std::mutex> lock(load_mutex_);
    wait_for_slot_clearance(lock);
    std::map<std::string, int> resolved_owned;
    for (const auto& kv : owned_live) resolved_owned[resolve_model_name(kv.first)] = kv.second;
    std::map<std::string, json> captured;
    std::vector<WrappedServer*> victims;
    for (const auto& server : loaded_servers_) {
        if (!server->is_backend_alive()) continue;
        const std::string name = server->get_model_name();
        auto pin_it = snapshot_pins.find(name);
        if (pin_it != snapshot_pins.end()) {
            if (server->is_pinned() != pin_it->second) server->set_pinned(pin_it->second);
            continue;
        }
        auto own_it = resolved_owned.find(name);
        if (own_it == resolved_owned.end()) continue;
        if (server->get_process_id() != own_it->second) continue;
        captured[name] = {{"options", server->get_recipe_options().to_json()},
                          {"pinned", server->is_pinned()}};
        victims.push_back(server.get());
    }
    for (auto* victim : victims) {
        if (victim->is_pinned()) victim->set_pinned(false);
        LOG(INFO, "Router") << "Reconcile-unload of job-loaded model: "
                            << victim->get_model_name() << std::endl;
        evict_server(victim);
    }
    return captured;
}

int Router::loaded_model_pid(const std::string& model_name) const {
    std::lock_guard<std::mutex> lock(load_mutex_);
    WrappedServer* server = find_server_by_model_name(resolve_model_name(model_name));
    return server && server->is_backend_alive() ? server->get_process_id() : -1;
}

std::string Router::canonical_model_name(const std::string& model_name) const {
    std::lock_guard<std::mutex> lock(load_mutex_);
    return resolve_model_name(model_name);
}

void Router::load_model(const std::string& model_name,
                       const ModelInfo& model_info,
                       RecipeOptions options,
                       bool do_not_upgrade,
                       bool allow_reload_on_option_change,
                       std::optional<bool> pinned,
                       std::atomic<bool>* cancel_flag) {
    const std::string canonical_model_name = resolve_model_name(model_name);
    const std::string backend_option = model_info.recipe + "_backend";

    RecipeOptions tentative = options.inherit(model_info.recipe_options.inherit(
    RecipeOptions(model_info.recipe, config_->recipe_options(""))));
    json backend_json = tentative.get_option(backend_option);
    const std::string backend = backend_json.is_string() ? backend_json.get<std::string>() : "";

    // Second pass: rebuild defaults using the resolved backend.
    // Per-architecture defaults sit between global config and model-level recipe_options.
    RecipeOptions default_opt = RecipeOptions(model_info.recipe, config_->recipe_options(backend));
    RecipeOptions arch_opts(model_info.recipe,
                            model_manager_->get_architecture_defaults(model_info.gguf.architecture));
    RecipeOptions effective_options = options.inherit(model_info.recipe_options.inherit(arch_opts.inherit(default_opt)));

    // LOAD SERIALIZATION STRATEGY (from spec: point #2 in Additional Considerations)
    std::unique_lock<std::mutex> lock(load_mutex_);

    load_cv_.wait(lock, [&] {
        return !is_loading_
               && (!exclusive_active_ || exclusive_owner_ == std::this_thread::get_id());
    });

    is_loading_ = true;

    LOG(DEBUG, "Router") << "Loading model: " << canonical_model_name
            << " (checkpoint: " << model_info.checkpoint()
            << ", recipe: " << model_info.recipe
            << ", type: " << model_type_to_string(model_info.type)
            << ", device: " << device_type_to_string(model_info.device) << ")" << std::endl;

    try {
        WrappedServer* existing_pre = find_server_by_model_name(canonical_model_name);
        bool final_pinned = false;
        if (pinned.has_value()) {
            final_pinned = pinned.value();
        } else if (existing_pre) {
            final_pinned = existing_pre->is_pinned();
        } else {
            final_pinned = (effective_options.get_option("pinned").is_boolean() && effective_options.get_option("pinned").get<bool>());
        }

        prune_unavailable_servers_locked();

        // Check if model is already loaded. Watchdog-reset or otherwise dead
        // entries are evicted first so auto-load performs a real lazy restart.
        WrappedServer* existing = find_server_by_model_name(canonical_model_name);
        if (existing && !existing->is_backend_alive()) {
            LOG(WARNING, "Router") << "Existing backend for " << canonical_model_name
                                    << " is unavailable (state="
                                    << existing->get_backend_health_state()
                                    << "), evicting before reload" << std::endl;
            evict_server(existing);
            existing = nullptr;
        }
        if (existing) {
            json existing_opts = existing->get_recipe_options().to_json();
            json requested_opts = effective_options.to_json();
            existing_opts.erase("pinned");
            requested_opts.erase("pinned");
            if (allow_reload_on_option_change && existing_opts != requested_opts) {
                LOG(INFO, "Router") << "Options changed, reloading model: " << canonical_model_name << std::endl;
                evict_server(existing);
                // Fall through to create and load with new options
            } else {
                LOG(DEBUG, "Router") << "Model already loaded, updating access time and pinned status" << std::endl;
                existing->set_pinned(final_pinned);
                existing->update_access_time();
                is_loading_ = false;
                load_cv_.notify_all();
                return;
            }
        }

        // Determine model type and device
        ModelType model_type = model_info.type;
        DeviceType device_type = model_info.device;

        // Get max models for this type (same limit for all types)
        int max_models = config_->max_loaded_models();

        // NPU EXCLUSIVITY CHECK — driven by the backend's slot policy (descriptor).
        //   ExclusiveNpu (ryzenai-llm, whisper-on-npu): lock the entire NPU,
        //                evicting ALL NPU servers first.
        //   CoexistByType (flm): coexist with other FLM types (max 1 per type),
        //                but evict exclusive-NPU peers.
        // Standard/Unmetered backends share no device exclusivity.
        switch (slot_policy_for_recipe(model_info.recipe)) {
            case SlotPolicy::ExclusiveNpu: {
                if (has_npu_server()) {
                    LOG(INFO, "Router") << model_info.recipe
                              << " requires exclusive NPU access, evicting all NPU servers..." << std::endl;
                    evict_all_npu_servers();
                }
                break;
            }
            case SlotPolicy::CoexistByType: {
                // 1. Evict every NPU holder that is not itself a coexisting (FLM)
                //    backend — i.e. exclusive-NPU peers like ryzenai-llm and
                //    whisper-on-npu. Collect first; evict_server mutates loaded_servers_.
                std::vector<WrappedServer*> exclusive_peers;
                for (const auto& server : loaded_servers_) {
                    if (server->is_backend_alive() && (server->get_device_type() & DEVICE_NPU) &&
                        slot_policy_for_recipe(server->get_recipe_options().get_recipe()) !=
                            SlotPolicy::CoexistByType) {
                        exclusive_peers.push_back(server.get());
                    }
                }
                for (auto* peer : exclusive_peers) {
                    LOG(INFO, "Router") << "FLM cannot coexist with "
                              << peer->get_recipe_options().get_recipe()
                              << ", evicting: " << peer->get_model_name() << std::endl;
                    evict_server(peer);
                }
                // 2. Evict FLM of the SAME model type (max 1 per type: 1 LLM, 1 transcription, 1 embed)
                WrappedServer* same_type_flm = find_coexisting_server_by_type(model_type);
                if (same_type_flm) {
                    LOG(INFO, "Router") << "FLM " << model_type_to_string(model_type)
                              << " slot occupied by: " << same_type_flm->get_model_name()
                              << ", evicting..." << std::endl;
                    evict_server(same_type_flm);
                }
                break;
            }
            case SlotPolicy::Standard:
            case SlotPolicy::Unmetered:
                break;
        }

        // LRU EVICTION CHECK (from spec: Least Recently Used Cache)
        // Skip eviction if unlimited (-1). Unmetered (cloud) loads also skip the
        // check entirely: they consume no local resources, so they have no
        // business kicking a warm local model out of memory.
        bool is_unmetered_load = is_unmetered_recipe(model_info.recipe);
        int current_count = count_servers_by_type(model_type);
        if (!is_unmetered_load && max_models != -1 && current_count >= max_models) {
            WrappedServer* lru = find_lru_server_by_type(model_type);
            if (lru) {
                LOG(INFO, "Router") << "Slot limit reached for type "
                          << model_type_to_string(model_type)
                          << ", evicting LRU: " << lru->get_model_name() << std::endl;
                evict_server(lru);
            } else {
                is_loading_ = false;
                load_cv_.notify_all();
                throw SlotsPinnedException(model_type_to_string(model_type));
            }
        }

        // Auto-tune: resolve ctx_size = -1 → computed from memory + arch metadata
        // Done AFTER eviction so that freed VRAM/RAM is visible to the memory query.
        int64_t auto_ctx = resolve_auto_ctx_size(effective_options, model_info);
        if (auto_ctx > 0) {
            LOG(INFO, "Router") << "Auto-tune ctx_size resolved to " << auto_ctx << std::endl;
            effective_options.set_option("ctx_size", auto_ctx);
        }

        LOG(DEBUG, "Router") << "Effective settings: " << effective_options.to_log_string() << std::endl;

        // Create new backend server
        std::unique_ptr<WrappedServer> new_server = create_backend_server(model_info);

        // Set model metadata
        new_server->set_model_metadata(canonical_model_name, model_info.checkpoint(), model_type, device_type, effective_options);
        new_server->set_pinned(final_pinned);
        new_server->update_access_time();

        // CRITICAL: Release lock before slow backend startup
        lock.unlock();

        // Load the backend (this can take 30-60 seconds)
        LOG(DEBUG, "Router") << "Starting backend (this may take a moment)..." << std::endl;
        bool load_success = false;
        std::string error_message;
        auto load_start = std::chrono::steady_clock::now();

        new_server->set_load_cancel_flag(cancel_flag);

        try {
            new_server->load(canonical_model_name, model_info, effective_options, do_not_upgrade);
            load_success = true;
            auto load_end = std::chrono::steady_clock::now();
            new_server->set_load_duration_ms(std::chrono::duration_cast<std::chrono::milliseconds>(load_end - load_start).count());
            LOG(DEBUG, "Router") << "Backend started successfully in " << new_server->get_load_duration_ms() << "ms" << std::endl;
        } catch (const std::exception& e) {
            error_message = e.what();
            load_success = false;
            LOG(ERROR, "Router") << "Backend load failed: " << error_message << std::endl;
        }

        new_server->set_load_cancel_flag(nullptr);

        lock.lock();

        if (load_success) {
            // Success: Refresh access time so this model is returned by
            // get_most_recent_server() (the pre-load timestamp from line 316
            // may have been overtaken by other models serving requests while
            // the lock was released during the slow backend load).
            new_server->update_access_time();
            new_server->set_state(ModelState::READY);

            // Add to loaded servers
            loaded_servers_.push_back(std::move(new_server));

            is_loading_ = false;
            load_cv_.notify_all();

            LOG(INFO, "Router") << "Model loaded successfully. Total loaded: "
                      << loaded_servers_.size() << std::endl;
        } else {
            // ERROR HANDLING (from spec: Error Handling section)
            // Check if error is "file not found" (exception to nuclear policy)
            bool is_file_not_found = (error_message.find("not found") != std::string::npos ||
                                     error_message.find("does not exist") != std::string::npos ||
                                     error_message.find("No such file") != std::string::npos);

            is_loading_ = false;
            load_cv_.notify_all();

            if (cancel_flag && cancel_flag->load()) {
                LOG(INFO, "Router") << "Load cancelled, skipping nuclear retry" << std::endl;
                throw std::runtime_error("load cancelled");
            }

            if (is_file_not_found) {
                LOG(ERROR, "Router") << "File not found error, NOT evicting other models" << std::endl;
                throw std::runtime_error(error_message);
            }

            // Nuclear option: evict all models and retry
            LOG(WARNING, "Router") << "Load failed with non-file-not-found error, "
                      << "evicting all models and retrying..." << std::endl;

            evict_all_servers();

            // Mark loading again for retry
            is_loading_ = true;

            // Create new server for retry
            std::unique_ptr<WrappedServer> retry_server = create_backend_server(model_info);
            retry_server->set_model_metadata(canonical_model_name, model_info.checkpoint(), model_type, device_type, effective_options);
            retry_server->set_pinned(final_pinned);
            retry_server->update_access_time();
            retry_server->set_load_cancel_flag(cancel_flag);

            lock.unlock();

            LOG(DEBUG, "Router") << "Retrying backend load..." << std::endl;
            try {
                auto retry_start = std::chrono::steady_clock::now();
                retry_server->load(canonical_model_name, model_info, effective_options, do_not_upgrade);
                auto retry_end = std::chrono::steady_clock::now();
                retry_server->set_load_duration_ms(std::chrono::duration_cast<std::chrono::milliseconds>(retry_end - retry_start).count());

                lock.lock();

                retry_server->set_state(ModelState::READY);
                const auto retry_duration_ms = retry_server->get_load_duration_ms();
                retry_server->set_load_cancel_flag(nullptr);
                loaded_servers_.push_back(std::move(retry_server));
                is_loading_ = false;
                load_cv_.notify_all();

                LOG(DEBUG, "Router") << "Retry successful in " << retry_duration_ms << "ms!" << std::endl;
            } catch (const std::exception& retry_error) {
                retry_server->set_load_cancel_flag(nullptr);
                lock.lock();
                is_loading_ = false;
                load_cv_.notify_all();

                LOG(ERROR, "Router") << "Retry also failed: " << retry_error.what() << std::endl;
                throw;
            }
        }

    } catch (const std::exception& e) {
        LOG(ERROR, "Router") << "Failed to load model: " << e.what() << std::endl;

        if (!lock.owns_lock()) {
            lock.lock();
        }
        is_loading_ = false;
        load_cv_.notify_all();

        throw;
    }
}

void Router::unload_model(const std::string& model_name) {
    std::unique_lock<std::mutex> lock(load_mutex_);
    wait_for_slot_clearance(lock);

    if (model_name.empty()) {
        // Unload all models
        LOG(INFO, "Router") << "Unload all models called" << std::endl;
        evict_all_servers();
    } else {
        // Unload specific model
        LOG(INFO, "Router") << "Unload model called: " << model_name << std::endl;
        std::string canonical_model_name = resolve_model_name(model_name);
        WrappedServer* server = find_server_by_model_name(canonical_model_name);
        if (!server) {
            throw std::runtime_error("Model not loaded: " + model_name);
        }
        evict_server(server);
    }
}

void Router::evict_if_committed(const std::string& model_name) {
    std::lock_guard<std::mutex> lock(load_mutex_);

    WrappedServer* server = find_server_by_model_name(model_name);
    if (!server) {
        return;  // Already gone
    }

    // An exclusive session may have started (and re-pinned this model via the
    // job snapshot reconcile) since the EVICTING mark was set. Neither state
    // was known when the eviction was decided, so abandon it.
    if (exclusive_active_ || server->is_pinned()) {
        server->rescue_from_eviction();
        LOG(INFO, "Router") << "Eviction of " << model_name << " cancelled ("
                            << (server->is_pinned() ? "pinned" : "exclusive session active")
                            << ")" << std::endl;
        return;
    }

    // Atomically confirm the model is still idle and EVICTING. If a request
    // rescued it (now IN_USE) this returns false and reverts it to READY, so we
    // leave it loaded — no crashed generation, no talking to a dead subprocess.
    if (!server->try_commit_eviction()) {
        LOG(INFO, "Router") << "Eviction of " << model_name
                            << " cancelled (rescued by in-flight request)" << std::endl;
        return;
    }

    evict_server(server);
}

std::string Router::get_loaded_model() const {
    std::lock_guard<std::mutex> lock(load_mutex_);
    WrappedServer* server = get_most_recent_server();
    return server ? model_manager_->get_public_model_name(server->get_model_name()) : "";
}

std::string Router::get_loaded_recipe() const {
    std::lock_guard<std::mutex> lock(load_mutex_);
    WrappedServer* server = get_most_recent_server();
    if (!server) return "";

    // Get the actual recipe from the server's recipe options
    return server->get_recipe_options().get_recipe();
}

std::string Router::get_sole_loaded_model_of_type(ModelType type) const {
    std::lock_guard<std::mutex> lock(load_mutex_);

    WrappedServer* match = nullptr;
    for (const auto& server : loaded_servers_) {
        if (!server->is_backend_alive() || server->get_model_type() != type) {
            continue;
        }
        if (match) return "";  // ambiguous: caller must name the model
        match = server.get();
    }
    return match ? model_manager_->get_public_model_name(match->get_model_name()) : "";
}

json Router::get_all_loaded_models() const {
    std::lock_guard<std::mutex> lock(load_mutex_);

    json result = json::array();

    for (const auto& server : loaded_servers_) {
        const bool backend_alive = server->is_backend_alive();
        if (!backend_alive) {
            continue;
        }

        json model_info;
        model_info["model_name"] = model_manager_->get_public_model_name(server->get_model_name());
        model_info["checkpoint"] = server->get_checkpoint();
        model_info["type"] = model_type_to_string(server->get_model_type());
        model_info["device"] = device_type_to_string(server->get_device_type());
        model_info["backend_url"] = server->get_address();  // For debugging port issues
        model_info["pid"] = server->get_process_id();
        model_info["status"] = model_state_to_string(server->get_state());
        model_info["backend_alive"] = true;
        model_info["backend_health"] = server->get_backend_health_state();
        model_info["loaded"] = true;
        model_info["watchdog_reset"] = server->was_watchdog_triggered();
        std::string watchdog_reason = server->get_watchdog_reset_reason();
        if (!watchdog_reason.empty()) {
            model_info["watchdog_reset_reason"] = watchdog_reason;
        }
        model_info["pinned"] = server->is_pinned();
        RecipeOptions recipe_options =  server->get_recipe_options();
        model_info["recipe"] = recipe_options.get_recipe();
        model_info["recipe_options"] = recipe_options.to_json();

        // Static metadata from the registry entry. Cloud models carry the
        // provider-reported context window + per-million-token cost (recorded
        // at discovery by ModelManager::refresh_cloud_models); local models
        // surface their runtime context via recipe_options instead.
        try {
            const ModelInfo reg_info = model_manager_->get_model_info(server->get_model_name());
            if (reg_info.max_context_window > 0) {
                model_info["max_context_window"] = reg_info.max_context_window;
            }
            if (reg_info.cost_input_per_million >= 0) {
                model_info["cost_input_per_million"] = reg_info.cost_input_per_million;
            }
            if (reg_info.cost_output_per_million >= 0) {
                model_info["cost_output_per_million"] = reg_info.cost_output_per_million;
            }
        } catch (...) {
            // Registry entry not found (raced with a delete) — skip static metadata.
        }

        // Convert timestamp to milliseconds since epoch
        auto time_point = server->get_last_access_time();
        auto duration = time_point.time_since_epoch();
        auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        model_info["last_use"] = millis;

        result.push_back(model_info);
    }

    return result;
}

json Router::get_max_model_limits() const {
    int max = config_->max_loaded_models();
    return {
        {"llm", max},
        {"embedding", max},
        {"reranking", max},
        {"transcription", max},
        {"image", max},
        {"tts", max},
        {"classification", max}
    };
}

bool Router::is_model_loaded() const {
    std::lock_guard<std::mutex> lock(load_mutex_);
    for (const auto& server : loaded_servers_) {
        if (server->is_backend_alive()) {
            return true;
        }
    }
    return false;
}

bool Router::is_model_loaded(const std::string& model_name) const {
    std::lock_guard<std::mutex> lock(load_mutex_);
    auto* server = find_server_by_model_name(resolve_model_name(model_name));
    return server != nullptr && server->is_backend_alive();
}

RecipeOptions Router::get_model_recipe_options(const std::string& model_name) const {
    std::lock_guard<std::mutex> lock(load_mutex_);
    auto* server = find_server_by_model_name(resolve_model_name(model_name));
    if (server && server->is_backend_alive()) return server->get_recipe_options();
    return RecipeOptions();
}

ModelType Router::get_model_type(const std::string& model_name) const {
    std::lock_guard<std::mutex> lock(load_mutex_);
    WrappedServer* server = model_name.empty()
        ? get_most_recent_server()
        : find_server_by_model_name(resolve_model_name(model_name));
    return (server && server->is_backend_alive()) ? server->get_model_type() : ModelType::LLM;
}

std::string Router::get_backend_address() const {
    std::lock_guard<std::mutex> lock(load_mutex_);
    WrappedServer* server = get_most_recent_server();
    return (server && server->is_backend_alive()) ? server->get_address() : "";
}

std::string Router::get_streaming_transcription_address(const std::string& model_name) const {
    std::lock_guard<std::mutex> lock(load_mutex_);
    WrappedServer* server = nullptr;
    if (!model_name.empty()) {
        // Route by the session's requested model, like the normal inference
        // path — most-recent would misroute multi-model setups (e.g. a
        // Whisper session connecting to Moonshine's stream)
        server = find_server_by_model_name(resolve_model_name(model_name));
    } else {
        server = get_most_recent_server();
    }
    if (!server) {
        return "";
    }
    auto* streaming = dynamic_cast<IStreamingTranscriptionServer*>(server);
    return streaming ? streaming->get_streaming_address() : "";
}

template<typename Func>
auto Router::execute_inference(const json& request, Func&& inference_func) -> decltype(inference_func(nullptr)) {
    std::string requested_model;
    if (request.contains("model") && request["model"].is_string()) {
        requested_model = request["model"].get<std::string>();
    }

    if (requested_model.empty()) {
        return ErrorResponse::from_exception(InvalidRequestException("No model specified in request"));
    }

    // A watchdog reset should be transparent for non-streaming calls when the
    // backend died before any response was returned. Retry exactly once after a
    // lazy reload; streaming paths deliberately do not retry after partial data.
    for (int attempt = 0; attempt < 2; ++attempt) {
        WrappedServer* server = nullptr;
        RecipeOptions restart_options;
        std::string restart_model_name;
        bool should_reload_before_request = false;

        {
            std::unique_lock<std::mutex> lock(load_mutex_);
            wait_for_slot_clearance(lock);
            server = find_server_by_model_name(resolve_model_name(requested_model));
            if (!server) {
                return ErrorResponse::from_exception(ModelNotLoadedException(requested_model));
            }

            if (!server->is_backend_alive()) {
                restart_options = server->get_recipe_options();
                restart_model_name = server->get_model_name();
                should_reload_before_request = true;
            } else {
                if (!server->acquire_for_inference()) {
                    return ErrorResponse::from_exception(ModelNotLoadedException(requested_model));
                }
                server->update_access_time();
            }
        } // Lock released here

        if (should_reload_before_request) {
            if (restart_model_name.empty()) {
                restart_model_name = requested_model;
            }
            if (attempt == 0 && reload_model_after_watchdog_reset(restart_model_name, restart_options)) {
                continue;
            }
            return ErrorResponse::create(
                "Backend for model '" + requested_model + "' is unavailable",
                ErrorType::BACKEND_ERROR,
                {{"code", "backend_unavailable"}, {"retryable", true}}
            );
        }

        InhibitGuard inhibit_guard(suspend_inhibitor_.get(), config_->inhibit_suspend());

        try {
            auto response = inference_func(server);
            const bool watchdog_reset =
                server->was_watchdog_triggered() || is_watchdog_reset_response(response);

            if (attempt == 0 && watchdog_reset) {
                restart_options = server->get_recipe_options();
                restart_model_name = server->get_model_name();
            }

            server->release_inference();

            if (attempt == 0 && watchdog_reset) {
                if (restart_model_name.empty()) {
                    restart_model_name = requested_model;
                }
                if (reload_model_after_watchdog_reset(restart_model_name, restart_options)) {
                    continue;
                }
            }

            return response;
        } catch (...) {
            server->release_inference();
            throw;
        }
    }

    return ErrorResponse::create(
        "Backend watchdog reset recovery failed for model '" + requested_model + "'",
        ErrorType::BACKEND_ERROR,
        {{"code", "backend_watchdog_reset"}, {"retryable", true}}
    );
}

// Template method for streaming execution
template<typename Func>
void Router::execute_streaming(const std::string& request_body, httplib::DataSink& sink, Func&& streaming_func, std::shared_ptr<telemetry::InferenceSpan> span) {
    WrappedServer* server = nullptr;
    std::string requested_model;

    try {
        json request = json::parse(request_body);
        if (request.contains("model") && request["model"].is_string()) {
            requested_model = request["model"].get<std::string>();
        }
    } catch (...) {
        LOG(DEBUG, "Router") << "Failed to parse request body for model extraction" << std::endl;
    }

    if (requested_model.empty()) {
        LOG(ERROR, "Router") << "No model specified in streaming request" << std::endl;
        json error = ErrorResponse::from_exception(InvalidRequestException("No model specified in request"));
        std::string error_msg = "data: " + error.dump() + "\n\n";
        sink.write(error_msg.c_str(), error_msg.size());
        sink.done();
        return;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        RecipeOptions restart_options;
        std::string restart_model_name;
        bool should_reload_before_request = false;

        {
            std::unique_lock<std::mutex> lock(load_mutex_);
            wait_for_slot_clearance(lock);
            server = find_server_by_model_name(resolve_model_name(requested_model));
            if (!server) {
                json error = ErrorResponse::from_exception(ModelNotLoadedException(requested_model));
                std::string error_msg = "data: " + error.dump() + "\n\n";
                sink.write(error_msg.c_str(), error_msg.size());
                sink.done();
                return;
            }

            if (!server->is_backend_alive()) {
                restart_options = server->get_recipe_options();
                restart_model_name = server->get_model_name();
                should_reload_before_request = true;
            } else {
                if (!server->acquire_for_inference()) {
                    std::string error_msg =
                        "data: {\"error\":{\"message\":\"Model evicted: " + requested_model +
                        "\",\"type\":\"model_not_loaded\"}}\n\n";
                    sink.write(error_msg.c_str(), error_msg.size());
                    sink.done();
                    return;
                }
                server->update_access_time();
            }
        } // Lock released here

        if (should_reload_before_request) {
            if (restart_model_name.empty()) {
                restart_model_name = requested_model;
            }
            if (attempt == 0 && reload_model_after_watchdog_reset(restart_model_name, restart_options)) {
                continue;
            }

            json error = ErrorResponse::create(
                "Backend for model '" + requested_model + "' is unavailable",
                ErrorType::BACKEND_ERROR,
                {{"code", "backend_unavailable"}, {"retryable", true}}
            );
            std::string error_msg = "data: " + error.dump() + "\n\n";
            sink.write(error_msg.c_str(), error_msg.size());
            sink.done();
            return;
        }

        InhibitGuard inhibit_guard(suspend_inhibitor_.get(), config_->inhibit_suspend());

        try {
            streaming_func(server);
            const bool watchdog_reset = server->was_watchdog_triggered();

            if (watchdog_reset) {
                restart_options = server->get_recipe_options();
                restart_model_name = server->get_model_name();
            }

            server->release_inference();

            // Do not replay a streaming response after bytes may have reached the
            // client. Reload immediately so the next request does not see a
            // stale tombstone, then return the stream outcome as-is.
            if (watchdog_reset) {
                if (restart_model_name.empty()) {
                    restart_model_name = requested_model;
                }
                reload_model_after_watchdog_reset(restart_model_name, restart_options);
            }
            return;
        } catch (const BackendStreamRetryableReset& e) {
            restart_options = server->get_recipe_options();
            restart_model_name = server->get_model_name();
            server->release_inference();

            if (restart_model_name.empty()) {
                restart_model_name = requested_model;
            }
            if (attempt == 0 && reload_model_after_watchdog_reset(restart_model_name, restart_options)) {
                continue;
            }

            if (span) {
                span->end_with_error(e.what());
            }

            json error = ErrorResponse::create(
                std::string("Backend for model '") + requested_model +
                    "' crashed before streaming started and could not be reloaded: " + e.what(),
                ErrorType::BACKEND_ERROR,
                {{"code", "backend_watchdog_reset"}, {"retryable", true}}
            );
            std::string error_msg = "data: " + error.dump() + "\n\n";
            sink.write(error_msg.c_str(), error_msg.size());
            sink.done();
            return;
        } catch (...) {
            server->release_inference();
            throw;
        }
    }
}

json Router::chat_completion(const json& request, std::atomic<bool>* cancel) {
    std::string requested_model = request.value("model", "");
    std::shared_ptr<telemetry::InferenceSpan> span = telemetry::TelemetryTracker::start_span("LLM", "chat.completions", requested_model, request);

    struct RequestCancelScope {
        WrappedServer* server = nullptr;
        ~RequestCancelScope() {
            if (server) server->set_request_cancel_flag(nullptr);
        }
    } cancel_scope;

    try {
        WrappedServer* active_server = nullptr;
        json response = execute_inference(request, [&](WrappedServer* server) {
            active_server = server;
            if (cancel) {
                server->set_request_cancel_flag(cancel);
                cancel_scope.server = server;
            }
            ModelTelemetryIdentity identity = get_telemetry_identity(server);
            if (span) {
                span->set_attribute("llm.backend", identity.recipe);
                span->set_attribute("llm.device_type", identity.device);
                span->set_attribute("llm.checkpoint", identity.checkpoint);
                span->set_attribute("llm.recipe", identity.recipe);
                if (request.contains("temperature")) span->set_attribute("llm.config.temperature", request["temperature"]);
                if (request.contains("top_p")) span->set_attribute("llm.config.top_p", request["top_p"]);
                if (request.contains("max_tokens")) span->set_attribute("llm.config.max_tokens", request["max_tokens"]);
                if (request.contains("max_completion_tokens")) span->set_attribute("llm.config.max_completion_tokens", request["max_completion_tokens"]);
            }
            return server->chat_completion(request);
        });

        if (span) {
            if (response.contains("error")) {
                std::string error_msg = "Request failed";
                if (response["error"].contains("message") && response["error"]["message"].is_string()) {
                    error_msg = response["error"]["message"].get<std::string>();
                }
                span->end_with_error(error_msg);
            } else {
                nlohmann::json usage_payload = nlohmann::json::object();
                std::string text_output = "";
                if (response.contains("usage") && response["usage"].is_object()) {
                    auto usage = response["usage"];
                    if (usage.contains("prompt_tokens")) {
                        usage_payload["prompt_tokens"] = usage["prompt_tokens"].get<int>();
                    } else if (usage.contains("input_tokens")) {
                        usage_payload["prompt_tokens"] = usage["input_tokens"].get<int>();
                    }
                    if (usage.contains("completion_tokens")) {
                        usage_payload["completion_tokens"] = usage["completion_tokens"].get<int>();
                    } else if (usage.contains("output_tokens")) {
                        usage_payload["completion_tokens"] = usage["output_tokens"].get<int>();
                    }
                }
                if (response.contains("timings")) {
                    auto timings = response["timings"];
                    if (timings.contains("prompt_n")) usage_payload["prompt_tokens"] = timings["prompt_n"].get<int>();
                    if (timings.contains("predicted_n")) usage_payload["completion_tokens"] = timings["predicted_n"].get<int>();

                    if (timings.contains("prompt_ms") && timings.contains("prompt_n")) {
                        double prompt_ms = timings["prompt_ms"].get<double>();
                        if (prompt_ms > 0) {
                            span->set_attribute("llm.performance.time_to_first_token", prompt_ms / 1000.0);
                        }
                    }
                    if (timings.contains("predicted_ms") && timings.contains("predicted_n")) {
                        double predicted_ms = timings["predicted_ms"].get<double>();
                        int predicted_n = timings["predicted_n"].get<int>();
                        if (predicted_ms > 0 && predicted_n > 0) {
                            span->set_attribute("llm.performance.tokens_per_second", (predicted_n / (predicted_ms / 1000.0)));
                        }
                    }
                }

                if (response.contains("choices") && response["choices"].is_array() && !response["choices"].empty()) {
                    auto choice = response["choices"][0];
                    std::string reasoning_output = "";
                    if (choice.contains("message")) {
                        auto msg = choice["message"];
                        if (msg.contains("reasoning_content") && msg["reasoning_content"].is_string()) {
                            reasoning_output = msg["reasoning_content"].get<std::string>();
                        } else if (msg.contains("thinking") && msg["thinking"].is_string()) {
                            reasoning_output = msg["thinking"].get<std::string>();
                        }
                        if (msg.contains("content") && msg["content"].is_string()) {
                            text_output = msg["content"].get<std::string>();
                        }
                    }
                    if (!reasoning_output.empty()) {
                        text_output = "<think>\n" + reasoning_output + "\n</think>\n" + text_output;
                    }
                }

                std::string url;
                std::function<std::map<std::string, nlohmann::json>(const std::string&)> parser;
                if (active_server) {
                    url = active_server->get_additional_telemetry_url();
                    parser = active_server->get_additional_telemetry_parser();
                }
                telemetry::end_llm_span_async(span, url, parser, usage_payload, text_output);
            }
        }
        return response;
    } catch (const std::exception& e) {
        if (span) span->end_with_error(e.what());
        throw;
    }
}

json Router::completion(const json& request) {
    std::string requested_model = request.value("model", "");
    std::shared_ptr<telemetry::InferenceSpan> span = telemetry::TelemetryTracker::start_span("LLM", "completions", requested_model, request);

    try {
        WrappedServer* active_server = nullptr;
        json response = execute_inference(request, [&](WrappedServer* server) {
            active_server = server;
            ModelTelemetryIdentity identity = get_telemetry_identity(server);
            if (span) {
                span->set_attribute("llm.backend", identity.recipe);
                span->set_attribute("llm.device_type", identity.device);
                span->set_attribute("llm.checkpoint", identity.checkpoint);
                span->set_attribute("llm.recipe", identity.recipe);
                if (request.contains("temperature")) span->set_attribute("llm.config.temperature", request["temperature"]);
                if (request.contains("top_p")) span->set_attribute("llm.config.top_p", request["top_p"]);
                if (request.contains("max_tokens")) span->set_attribute("llm.config.max_tokens", request["max_tokens"]);
            }
            return server->completion(request);
        });

        if (span) {
            if (response.contains("error")) {
                std::string error_msg = "Request failed";
                if (response["error"].contains("message") && response["error"]["message"].is_string()) {
                    error_msg = response["error"]["message"].get<std::string>();
                }
                span->end_with_error(error_msg);
            } else {
                nlohmann::json usage_payload = nlohmann::json::object();
                std::string text_output = "";
                if (response.contains("usage") && response["usage"].is_object()) {
                    auto usage = response["usage"];
                    if (usage.contains("prompt_tokens")) {
                        usage_payload["prompt_tokens"] = usage["prompt_tokens"].get<int>();
                    } else if (usage.contains("input_tokens")) {
                        usage_payload["prompt_tokens"] = usage["input_tokens"].get<int>();
                    }
                    if (usage.contains("completion_tokens")) {
                        usage_payload["completion_tokens"] = usage["completion_tokens"].get<int>();
                    } else if (usage.contains("output_tokens")) {
                        usage_payload["completion_tokens"] = usage["output_tokens"].get<int>();
                    }
                }

                if (response.contains("choices") && response["choices"].is_array() && !response["choices"].empty()) {
                    auto choice = response["choices"][0];
                    if (choice.contains("text") && choice["text"].is_string()) {
                        text_output = choice["text"].get<std::string>();
                    }
                }

                std::string url;
                std::function<std::map<std::string, nlohmann::json>(const std::string&)> parser;
                if (active_server) {
                    url = active_server->get_additional_telemetry_url();
                    parser = active_server->get_additional_telemetry_parser();
                }
                telemetry::end_llm_span_async(span, url, parser, usage_payload, text_output);
            }
        }
        return response;
    } catch (const std::exception& e) {
        if (span) span->end_with_error(e.what());
        throw;
    }
}

json Router::embeddings(const json& request) {
    std::string requested_model = request.value("model", "");
    std::shared_ptr<telemetry::InferenceSpan> span = telemetry::TelemetryTracker::start_span("EMBEDDING", "embeddings", requested_model, request);

    try {
        json response = execute_inference(request, [&](WrappedServer* server) {
            ModelTelemetryIdentity identity = get_telemetry_identity(server);
            if (span) {
                span->set_attribute("embedding.backend", identity.recipe);
                span->set_attribute("embedding.device_type", identity.device);
                span->set_attribute("embedding.checkpoint", identity.checkpoint);
                span->set_attribute("embedding.recipe", identity.recipe);
            }
            auto embeddings_server = dynamic_cast<IEmbeddingsServer*>(server);
            if (!embeddings_server) {
                return ErrorResponse::from_exception(
                    UnsupportedOperationException("Embeddings", device_type_to_string(server->get_device_type()))
                );
            }
            return embeddings_server->embeddings(request);
        });

        if (span) {
            if (response.contains("error")) {
                std::string error_msg = "Request failed";
                if (response["error"].contains("message") && response["error"]["message"].is_string()) {
                    error_msg = response["error"]["message"].get<std::string>();
                }
                span->end_with_error(error_msg);
            } else {
                nlohmann::json usage_payload = nlohmann::json::object();
                if (response.contains("usage") && response["usage"].is_object()) {
                    auto usage = response["usage"];
                    if (usage.contains("prompt_tokens")) {
                        usage_payload["prompt_tokens"] = usage["prompt_tokens"].get<int>();
                    } else if (usage.contains("input_tokens")) {
                        usage_payload["prompt_tokens"] = usage["input_tokens"].get<int>();
                    }
                    if (usage.contains("total_tokens")) usage_payload["total_tokens"] = usage["total_tokens"].get<int>();
                }
                std::string output_dump = "";
                if (response.contains("data") && response["data"].is_array()) {
                    output_dump = "Embeddings data with " + std::to_string(response["data"].size()) + " vectors.";
                } else {
                    output_dump = response.dump();
                }
                span->end_with_success(usage_payload, output_dump);
            }
        }
        return response;
    } catch (const std::exception& e) {
        if (span) span->end_with_error(e.what());
        throw;
    }
}

json Router::reranking(const json& request) {
    std::string requested_model = request.value("model", "");
    std::shared_ptr<telemetry::InferenceSpan> span = telemetry::TelemetryTracker::start_span("RERANKER", "reranking", requested_model, request);

    try {
        json response = execute_inference(request, [&](WrappedServer* server) {
            ModelTelemetryIdentity identity = get_telemetry_identity(server);
            if (span) {
                span->set_attribute("reranker.backend", identity.recipe);
                span->set_attribute("reranker.device_type", identity.device);
                span->set_attribute("reranker.checkpoint", identity.checkpoint);
                span->set_attribute("reranker.recipe", identity.recipe);
            }
            auto reranking_server = dynamic_cast<IRerankingServer*>(server);
            if (!reranking_server) {
                return ErrorResponse::from_exception(
                    UnsupportedOperationException("Reranking", device_type_to_string(server->get_device_type()))
                );
            }
            return reranking_server->reranking(request);
        });

        if (span) {
            if (response.contains("error")) {
                std::string error_msg = "Request failed";
                if (response["error"].contains("message") && response["error"]["message"].is_string()) {
                    error_msg = response["error"]["message"].get<std::string>();
                }
                span->end_with_error(error_msg);
            } else {
                nlohmann::json usage_payload = nlohmann::json::object();
                if (response.contains("usage") && response["usage"].is_object()) {
                    auto usage = response["usage"];
                    if (usage.contains("prompt_tokens")) {
                        usage_payload["prompt_tokens"] = usage["prompt_tokens"].get<int>();
                    } else if (usage.contains("input_tokens")) {
                        usage_payload["prompt_tokens"] = usage["input_tokens"].get<int>();
                    }
                    if (usage.contains("total_tokens")) usage_payload["total_tokens"] = usage["total_tokens"].get<int>();
                }
                span->end_with_success(usage_payload, response.dump());
            }
        }
        return response;
    } catch (const std::exception& e) {
        if (span) span->end_with_error(e.what());
        throw;
    }
}

json Router::classify(const json& request) {
    std::string requested_model = request.value("model", "");
    std::shared_ptr<telemetry::InferenceSpan> span = telemetry::TelemetryTracker::start_span("CLASSIFIER", "classify", requested_model, request);

    try {
        json response = execute_inference(request, [&](WrappedServer* server) {
            ModelTelemetryIdentity identity = get_telemetry_identity(server);
            if (span) {
                span->set_attribute("classifier.backend", identity.recipe);
                span->set_attribute("classifier.device_type", identity.device);
                span->set_attribute("classifier.checkpoint", identity.checkpoint);
                span->set_attribute("classifier.recipe", identity.recipe);
            }
            auto classification_server = dynamic_cast<IClassificationServer*>(server);
            if (!classification_server) {
                return ErrorResponse::from_exception(
                    UnsupportedOperationException("Classification", device_type_to_string(server->get_device_type()))
                );
            }
            return classification_server->classify(request);
        });

        if (span) {
            if (response.contains("error")) {
                std::string error_msg = "Request failed";
                if (response["error"].contains("message") && response["error"]["message"].is_string()) {
                    error_msg = response["error"]["message"].get<std::string>();
                }
                span->end_with_error(error_msg);
            } else {
                // Label scores classify user content; keep them out of telemetry.
                span->end_with_success(nlohmann::json::object(), "");
            }
        }
        return response;
    } catch (const std::exception& e) {
        if (span) span->end_with_error(e.what());
        throw;
    }
}

json Router::get_slots() {
    WrappedServer* server = nullptr;
    ISlotsServer* slots_server = nullptr;

    {
        std::unique_lock<std::mutex> lock(load_mutex_);
        wait_for_slot_clearance(lock);
        server = get_most_recent_server();
        if (!server) {
            return ErrorResponse::from_exception(
                ModelNotLoadedException("No models loaded")
            );
        }

        // Check if server supports slots capability
        slots_server = dynamic_cast<ISlotsServer*>(server);
        if (!slots_server) {
            return ErrorResponse::from_exception(
                UnsupportedOperationException("Slots", device_type_to_string(server->get_device_type()))
            );
        }

        // Mark as busy and update access time
        if (!server->acquire_for_inference()) {
            return ErrorResponse::from_exception(ModelNotLoadedException("No models loaded"));
        }
        server->update_access_time();
    } // Lock released here

    // Execute without holding lock (but busy flag prevents eviction)
    try {
        auto response = slots_server->get_slots();
        server->release_inference();
        return response;
    } catch (...) {
        server->release_inference();
        throw;
    }
}

json Router::slots_action(int slot_id, const std::string& action, const json& request_body) {
    WrappedServer* server = nullptr;
    ISlotsServer* slots_server = nullptr;

    {
        std::unique_lock<std::mutex> lock(load_mutex_);
        wait_for_slot_clearance(lock);
        server = get_most_recent_server();
        if (!server) {
            return ErrorResponse::from_exception(
                ModelNotLoadedException("No models loaded")
            );
        }

        // Check if server supports slots capability
        slots_server = dynamic_cast<ISlotsServer*>(server);
        if (!slots_server) {
            return ErrorResponse::from_exception(
                UnsupportedOperationException("Slots", device_type_to_string(server->get_device_type()))
            );
        }

        // Mark as busy and update access time
        if (!server->acquire_for_inference()) {
            return ErrorResponse::from_exception(ModelNotLoadedException("No models loaded"));
        }
        server->update_access_time();
    } // Lock released here

    // Execute without holding lock (but busy flag prevents eviction)
    try {
        auto response = slots_server->slots_action(slot_id, action, request_body);
        server->release_inference();
        return response;
    } catch (...) {
        server->release_inference();
        throw;
    }
}

json Router::tokenize(const json& request_body) {
    WrappedServer* server = nullptr;
    ITokenizerServer* tokenizer_server = nullptr;

    {
        std::unique_lock<std::mutex> lock(load_mutex_);
        wait_for_slot_clearance(lock);
        server = get_most_recent_server();
        if (!server) {
            return ErrorResponse::from_exception(
                ModelNotLoadedException("No models loaded")
            );
        }

        // Check if server supports tokenize capability
        tokenizer_server = dynamic_cast<ITokenizerServer*>(server);
        if (!tokenizer_server) {
            return ErrorResponse::from_exception(
                UnsupportedOperationException("Tokenization", device_type_to_string(server->get_device_type()))
            );
        }

        // Mark as busy and update access time
        if (!server->acquire_for_inference()) {
            return ErrorResponse::from_exception(ModelNotLoadedException("No models loaded"));
        }
        server->update_access_time();
    } // Lock released here

    // Execute without holding lock (but busy flag prevents eviction)
    try {
        auto response = tokenizer_server->tokenize(request_body);
        server->release_inference();
        return response;
    } catch (...) {
        server->release_inference();
        throw;
    }
}

json Router::responses(const json& request) {
    return execute_inference(request, [&](WrappedServer* server) {
        return server->responses(request);
    });
}

json Router::audio_transcriptions(const json& request) {
    return execute_inference(request, [&](WrappedServer* server) {
        auto transcription_server = dynamic_cast<ITranscriptionServer*>(server);
        if (!transcription_server) {
            return ErrorResponse::from_exception(
                UnsupportedOperationException("Audio transcription", device_type_to_string(server->get_device_type()))
            );
        }
        return transcription_server->audio_transcriptions(request);
    });
}

void Router::audio_speech(const json& request, httplib::DataSink& sink) {
    execute_streaming(request.dump(), sink, [&](WrappedServer* server) {
        auto tts_server = dynamic_cast<ITextToSpeechServer*>(server);
        if (!tts_server) {
            throw UnsupportedOperationException("Text to speech", device_type_to_string(server->get_device_type()));
        }
        tts_server->audio_speech(request, sink);
    });
}

std::vector<std::string> Router::audio_speech_supported_formats(const std::string& model_name) {
    std::lock_guard<std::mutex> lock(load_mutex_);
    auto tts_server = dynamic_cast<ITextToSpeechServer*>(
        find_server_by_model_name(resolve_model_name(model_name)));
    return tts_server ? tts_server->supported_audio_formats() : std::vector<std::string>{};
}

json Router::image_generations(const json& request) {
    return execute_inference(request, [&](WrappedServer* server) {
        auto image_server = dynamic_cast<IImageServer*>(server);
        if (!image_server) {
            return ErrorResponse::from_exception(
                UnsupportedOperationException("Image generation", device_type_to_string(server->get_device_type()))
            );
        }
        return image_server->image_generations(request);
    });
}

json Router::image_edits(const json& request) {
    return execute_inference(request, [&](WrappedServer* server) {
        auto image_server = dynamic_cast<IImageServer*>(server);
        if (!image_server) {
            return ErrorResponse::from_exception(
                UnsupportedOperationException("Image editing", device_type_to_string(server->get_device_type()))
            );
        }
        return image_server->image_edits(request);
    });
}

json Router::image_variations(const json& request) {
    return execute_inference(request, [&](WrappedServer* server) {
        auto image_server = dynamic_cast<IImageServer*>(server);
        if (!image_server) {
            return ErrorResponse::from_exception(
                UnsupportedOperationException("Image variations", device_type_to_string(server->get_device_type()))
            );
        }
        return image_server->image_variations(request);
    });
}

void Router::audio_generations(const json& request, httplib::DataSink& sink) {
    execute_streaming(request.dump(), sink, [&](WrappedServer* server) {
        auto audio_server = dynamic_cast<IAudioGenerationServer*>(server);
        if (!audio_server) {
            throw UnsupportedOperationException("Audio generation", device_type_to_string(server->get_device_type()));
        }
        audio_server->audio_generations(request, sink);
    });
}

std::vector<std::string> Router::audio_generation_supported_formats(const std::string& model_name) {
    std::lock_guard<std::mutex> lock(load_mutex_);
    auto audio_server = dynamic_cast<IAudioGenerationServer*>(
        find_server_by_model_name(resolve_model_name(model_name)));
    return audio_server ? audio_server->supported_audio_formats() : std::vector<std::string>{};
}

void Router::model_3d_generations(const json& request, httplib::DataSink& sink) {
    execute_streaming(request.dump(), sink, [&](WrappedServer* server) {
        auto model_server = dynamic_cast<IModel3DServer*>(server);
        if (!model_server) {
            throw UnsupportedOperationException("3D generation", device_type_to_string(server->get_device_type()));
        }
        model_server->model_3d_generations(request, sink);
    });
}

json Router::get_stats() const {
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    return aggregate_telemetry_.to_json();
}

json Router::get_metrics_snapshot() const {
    json result;
    result["loaded_models"] = json::array();
    result["model_metrics"] = json::array();
    result["totals"] = {
        {"requests", 0},
        {"input_tokens", 0},
        {"output_tokens", 0},
        {"prompt_tokens", 0}
    };

    std::map<std::string, ModelTelemetryIdentity> loaded_identities;

    {
        std::lock_guard<std::mutex> lock(load_mutex_);
        for (const auto& server : loaded_servers_) {
            ModelTelemetryIdentity identity = get_telemetry_identity(server.get());
            loaded_identities[identity.key()] = identity;

            json model_info;
            model_info["model_name"] = model_manager_->get_public_model_name(identity.model_name);
            model_info["checkpoint"] = identity.checkpoint;
            model_info["type"] = identity.type;
            model_info["device"] = identity.device;
            model_info["backend_url"] = server->get_address();
            model_info["pid"] = server->get_process_id();
            model_info["recipe"] = identity.recipe;
            result["loaded_models"].push_back(model_info);
        }
    }

    {
        std::lock_guard<std::mutex> lock(telemetry_mutex_);
        for (const auto& item : telemetry_by_model_) {
            const auto& record = item.second;
            json model_info;
            model_info["model_name"] = model_manager_->get_public_model_name(record.identity.model_name);
            model_info["checkpoint"] = record.identity.checkpoint;
            model_info["type"] = record.identity.type;
            model_info["device"] = record.identity.device;
            model_info["recipe"] = record.identity.recipe;
            model_info["loaded"] = loaded_identities.find(item.first) != loaded_identities.end();
            model_info["telemetry"] = record.telemetry.to_json();
            result["model_metrics"].push_back(model_info);
        }

        for (const auto& item : loaded_identities) {
            if (telemetry_by_model_.find(item.first) != telemetry_by_model_.end()) {
                continue;
            }
            const auto& identity = item.second;
            json model_info;
            model_info["model_name"] = model_manager_->get_public_model_name(identity.model_name);
            model_info["checkpoint"] = identity.checkpoint;
            model_info["type"] = identity.type;
            model_info["device"] = identity.device;
            model_info["recipe"] = identity.recipe;
            model_info["loaded"] = true;
            model_info["telemetry"] = Telemetry().to_json();
            result["model_metrics"].push_back(model_info);
        }

        result["totals"]["requests"] = aggregate_telemetry_.request_count_total;
        result["totals"]["input_tokens"] = aggregate_telemetry_.input_tokens_total;
        result["totals"]["output_tokens"] = aggregate_telemetry_.output_tokens_total;
        result["totals"]["prompt_tokens"] = aggregate_telemetry_.prompt_tokens_total;
    }

    return result;
}

ModelTelemetryIdentity Router::get_telemetry_identity(WrappedServer* server) const {
    if (!server) {
        return {};
    }

    RecipeOptions recipe_options = server->get_recipe_options();
    return {
        server->get_model_name(),
        server->get_checkpoint(),
        model_type_to_string(server->get_model_type()),
        device_type_to_string(server->get_device_type()),
        recipe_options.get_recipe()
    };
}

void Router::record_telemetry_for_model(const ModelTelemetryIdentity& identity,
                                        int input_tokens,
                                        int output_tokens,
                                        double time_to_first_token,
                                        double tokens_per_second) {
    if (identity.model_name.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    ModelTelemetryRecord& record = telemetry_by_model_[identity.key()];
    record.identity = identity;
    Telemetry& model_telemetry = record.telemetry;
    model_telemetry.input_tokens = input_tokens;
    model_telemetry.output_tokens = output_tokens;
    model_telemetry.time_to_first_token = time_to_first_token;
    model_telemetry.tokens_per_second = tokens_per_second;
    model_telemetry.request_count_total++;

    aggregate_telemetry_.input_tokens = input_tokens;
    aggregate_telemetry_.output_tokens = output_tokens;
    aggregate_telemetry_.time_to_first_token = time_to_first_token;
    aggregate_telemetry_.tokens_per_second = tokens_per_second;
    aggregate_telemetry_.request_count_total++;

    if (input_tokens > 0) {
        model_telemetry.input_tokens_total += static_cast<uint64_t>(input_tokens);
        aggregate_telemetry_.input_tokens_total += static_cast<uint64_t>(input_tokens);
    }
    if (output_tokens > 0) {
        model_telemetry.output_tokens_total += static_cast<uint64_t>(output_tokens);
        aggregate_telemetry_.output_tokens_total += static_cast<uint64_t>(output_tokens);
    }
}

void Router::record_prompt_tokens_for_model(const ModelTelemetryIdentity& identity, int prompt_tokens) {
    if (identity.model_name.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    ModelTelemetryRecord& record = telemetry_by_model_[identity.key()];
    record.identity = identity;
    Telemetry& model_telemetry = record.telemetry;
    model_telemetry.prompt_tokens = prompt_tokens;
    aggregate_telemetry_.prompt_tokens = prompt_tokens;
    if (prompt_tokens > 0) {
        model_telemetry.prompt_tokens_total += static_cast<uint64_t>(prompt_tokens);
        aggregate_telemetry_.prompt_tokens_total += static_cast<uint64_t>(prompt_tokens);
    }
}

void Router::update_telemetry(const std::string& model_name,
                              int input_tokens, int output_tokens,
                              double time_to_first_token, double tokens_per_second) {
    ModelTelemetryIdentity identity;
    {
        std::lock_guard<std::mutex> lock(load_mutex_);
        WrappedServer* server = model_name.empty()
            ? get_most_recent_server()
            : find_server_by_model_name(resolve_model_name(model_name));
        identity = get_telemetry_identity(server);
    }
    record_telemetry_for_model(identity, input_tokens, output_tokens,
                               time_to_first_token, tokens_per_second);
}

void Router::update_prompt_tokens(const std::string& model_name, int prompt_tokens) {
    ModelTelemetryIdentity identity;
    {
        std::lock_guard<std::mutex> lock(load_mutex_);
        WrappedServer* server = model_name.empty()
            ? get_most_recent_server()
            : find_server_by_model_name(resolve_model_name(model_name));
        identity = get_telemetry_identity(server);
    }
    record_prompt_tokens_for_model(identity, prompt_tokens);
}

void Router::chat_completion_stream(const std::string& request_body, httplib::DataSink& sink) {
    json request_json;
    try {
        request_json = json::parse(request_body);
    } catch (...) {}
    std::string requested_model = request_json.value("model", "");

    std::shared_ptr<telemetry::InferenceSpan> span = telemetry::TelemetryTracker::start_span("LLM", "chat.completions", requested_model, request_json);

    bool hide_outputs = false;
    bool hide_thinking = false;
    if (auto* config = RuntimeConfig::global()) {
        hide_outputs = config->telemetry_hide_outputs();
        hide_thinking = config->telemetry_hide_thinking();
    }

    auto accumulated_text = std::make_shared<std::string>();
    auto accumulated_reasoning = std::make_shared<std::string>();
    auto line_buffer = std::make_shared<std::string>();

    httplib::DataSink telemetry_sink;
    telemetry_sink.write = [accumulated_text, accumulated_reasoning, line_buffer, &sink, hide_outputs, hide_thinking](const char* data, size_t len) -> bool {
        bool success = false;
        if (sink.write) {
            success = sink.write(data, len);
        }
        line_buffer->append(data, len);
        StreamingProxy::process_sse_lines(*line_buffer, [accumulated_text, accumulated_reasoning, hide_outputs, hide_thinking](const std::string& line) {
            if (line.rfind("data: ", 0) == 0) {
                std::string json_str = line.substr(6);
                if (json_str.find("[DONE]") == std::string::npos) {
                    try {
                        auto parsed = json::parse(json_str);
                        if (parsed.contains("choices") && parsed["choices"].is_array() && !parsed["choices"].empty()) {
                            auto delta = parsed["choices"][0]["delta"];
                            if (!hide_thinking) {
                                if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
                                    *accumulated_reasoning += delta["reasoning_content"].get<std::string>();
                                } else if (delta.contains("thinking") && delta["thinking"].is_string()) {
                                    *accumulated_reasoning += delta["thinking"].get<std::string>();
                                }
                            }
                            if (!hide_outputs) {
                                if (delta.contains("content") && delta["content"].is_string()) {
                                    *accumulated_text += delta["content"].get<std::string>();
                                }
                            }
                        }
                    } catch (...) {}
                }
            }
        });
        return success;
    };

    telemetry_sink.is_writable = [&sink]() -> bool {
        return sink.is_writable ? sink.is_writable() : true;
    };

    telemetry_sink.done = [&sink]() {
        if (sink.done) {
            sink.done();
        }
    };

    telemetry_sink.done_with_trailer = [&sink](const httplib::Headers& trailer) {
        if (sink.done_with_trailer) {
            sink.done_with_trailer(trailer);
        } else if (sink.done) {
            sink.done();
        }
    };

    try {
        execute_streaming(request_body, telemetry_sink, [&](WrappedServer* server) {
            ModelTelemetryIdentity identity = get_telemetry_identity(server);

            if (span) {
                span->set_attribute("llm.backend", identity.recipe);
                span->set_attribute("llm.device_type", identity.device);
                span->set_attribute("llm.checkpoint", identity.checkpoint);
                span->set_attribute("llm.recipe", identity.recipe);
                if (request_json.contains("temperature")) span->set_attribute("llm.config.temperature", request_json["temperature"]);
                if (request_json.contains("top_p")) span->set_attribute("llm.config.top_p", request_json["top_p"]);
                if (request_json.contains("max_tokens")) span->set_attribute("llm.config.max_tokens", request_json["max_tokens"]);
                if (request_json.contains("max_completion_tokens")) span->set_attribute("llm.config.max_completion_tokens", request_json["max_completion_tokens"]);
            }

            server->forward_streaming_request("/v1/chat/completions", request_body, telemetry_sink, true, 0,
                [this, identity, span, accumulated_text, accumulated_reasoning, server](int input_tokens,
                                 int output_tokens,
                                 double time_to_first_token,
                                 double tokens_per_second,
                                 const std::string& error_message) {
                    if (!error_message.empty()) {
                        if (span) {
                            span->end_with_error(error_message);
                        }
                        return;
                    }
                    record_telemetry_for_model(identity, input_tokens, output_tokens,
                                               time_to_first_token, tokens_per_second);
                    record_prompt_tokens_for_model(identity, input_tokens);

                    if (span) {
                        nlohmann::json usage_payload = {
                            {"prompt_tokens", input_tokens},
                            {"completion_tokens", output_tokens}
                        };
                        span->set_attribute("llm.performance.time_to_first_token", time_to_first_token);
                        span->set_attribute("llm.performance.tokens_per_second", tokens_per_second);
                        std::string final_output = *accumulated_text;
                        if (!accumulated_reasoning->empty()) {
                            final_output = "<think>\n" + *accumulated_reasoning + "\n</think>\n" + final_output;
                        }

                        std::string url;
                        std::function<std::map<std::string, nlohmann::json>(const std::string&)> parser;
                        if (server) {
                            url = server->get_additional_telemetry_url();
                            parser = server->get_additional_telemetry_parser();
                        }
                        telemetry::end_llm_span_async(span, url, parser, usage_payload, final_output);
                    }
                });
        }, span);
    } catch (const std::exception& e) {
        if (span) span->end_with_error(e.what());
        throw;
    } catch (...) {
        if (span) span->end_with_error("Unknown error during streaming");
        throw;
    }
}

void Router::completion_stream(const std::string& request_body, httplib::DataSink& sink) {
    json request_json;
    try {
        request_json = json::parse(request_body);
    } catch (...) {}
    std::string requested_model = request_json.value("model", "");

    std::shared_ptr<telemetry::InferenceSpan> span = telemetry::TelemetryTracker::start_span("LLM", "completions", requested_model, request_json);

    bool hide_outputs = false;
    if (auto* config = RuntimeConfig::global()) {
        hide_outputs = config->telemetry_hide_outputs();
    }

    auto accumulated_text = std::make_shared<std::string>();
    auto line_buffer = std::make_shared<std::string>();

    httplib::DataSink telemetry_sink;
    telemetry_sink.write = [accumulated_text, line_buffer, &sink, hide_outputs](const char* data, size_t len) -> bool {
        bool success = false;
        if (sink.write) {
            success = sink.write(data, len);
        }
        line_buffer->append(data, len);
        StreamingProxy::process_sse_lines(*line_buffer, [accumulated_text, hide_outputs](const std::string& line) {
            if (line.rfind("data: ", 0) == 0) {
                std::string json_str = line.substr(6);
                if (json_str.find("[DONE]") == std::string::npos) {
                    try {
                        auto parsed = json::parse(json_str);
                        if (parsed.contains("choices") && parsed["choices"].is_array() && !parsed["choices"].empty()) {
                            auto choice = parsed["choices"][0];
                            if (!hide_outputs) {
                                if (choice.contains("text") && choice["text"].is_string()) {
                                    *accumulated_text += choice["text"].get<std::string>();
                                }
                            }
                        }
                    } catch (...) {}
                }
            }
        });
        return success;
    };

    telemetry_sink.is_writable = [&sink]() -> bool {
        return sink.is_writable ? sink.is_writable() : true;
    };

    telemetry_sink.done = [&sink]() {
        if (sink.done) {
            sink.done();
        }
    };

    telemetry_sink.done_with_trailer = [&sink](const httplib::Headers& trailer) {
        if (sink.done_with_trailer) {
            sink.done_with_trailer(trailer);
        } else if (sink.done) {
            sink.done();
        }
    };

    try {
        execute_streaming(request_body, telemetry_sink, [&](WrappedServer* server) {
            ModelTelemetryIdentity identity = get_telemetry_identity(server);

            if (span) {
                span->set_attribute("llm.backend", identity.recipe);
                span->set_attribute("llm.device_type", identity.device);
                span->set_attribute("llm.checkpoint", identity.checkpoint);
                span->set_attribute("llm.recipe", identity.recipe);
                if (request_json.contains("temperature")) span->set_attribute("llm.config.temperature", request_json["temperature"]);
                if (request_json.contains("top_p")) span->set_attribute("llm.config.top_p", request_json["top_p"]);
                if (request_json.contains("max_tokens")) span->set_attribute("llm.config.max_tokens", request_json["max_tokens"]);
            }

            server->forward_streaming_request("/v1/completions", request_body, telemetry_sink, true, 0,
                [this, identity, span, accumulated_text, server](int input_tokens,
                                 int output_tokens,
                                 double time_to_first_token,
                                 double tokens_per_second,
                                 const std::string& error_message) {
                    if (!error_message.empty()) {
                        if (span) {
                            span->end_with_error(error_message);
                        }
                        return;
                    }
                    record_telemetry_for_model(identity, input_tokens, output_tokens,
                                               time_to_first_token, tokens_per_second);
                    record_prompt_tokens_for_model(identity, input_tokens);

                    if (span) {
                        nlohmann::json usage_payload = {
                            {"prompt_tokens", input_tokens},
                            {"completion_tokens", output_tokens}
                        };
                        span->set_attribute("llm.performance.time_to_first_token", time_to_first_token);
                        span->set_attribute("llm.performance.tokens_per_second", tokens_per_second);

                        std::string url;
                        std::function<std::map<std::string, nlohmann::json>(const std::string&)> parser;
                        if (server) {
                            url = server->get_additional_telemetry_url();
                            parser = server->get_additional_telemetry_parser();
                        }
                        telemetry::end_llm_span_async(span, url, parser, usage_payload, *accumulated_text);
                    }
                });
        }, span);
    } catch (const std::exception& e) {
        if (span) span->end_with_error(e.what());
        throw;
    } catch (...) {
        if (span) span->end_with_error("Unknown error during streaming");
        throw;
    }
}

void Router::responses_stream(const std::string& request_body, httplib::DataSink& sink) {
    json request_json;
    try {
        request_json = json::parse(request_body);
    } catch (...) {}
    std::string requested_model = request_json.value("model", "");

    std::shared_ptr<telemetry::InferenceSpan> span = telemetry::TelemetryTracker::start_span("LLM", "responses", requested_model, request_json);

    bool hide_outputs = false;
    if (auto* config = RuntimeConfig::global()) {
        hide_outputs = config->telemetry_hide_outputs();
    }

    auto accumulated_text = std::make_shared<std::string>();
    auto line_buffer = std::make_shared<std::string>();

    httplib::DataSink telemetry_sink;
    telemetry_sink.write = [accumulated_text, line_buffer, &sink, hide_outputs](const char* data, size_t len) -> bool {
        bool success = false;
        if (sink.write) {
            success = sink.write(data, len);
        }
        line_buffer->append(data, len);
        StreamingProxy::process_sse_lines(*line_buffer, [accumulated_text, hide_outputs](const std::string& line) {
            if (line.rfind("data: ", 0) == 0) {
                std::string json_str = line.substr(6);
                if (json_str.find("[DONE]") == std::string::npos) {
                    try {
                        auto parsed = json::parse(json_str);
                        if (!hide_outputs) {
                            StreamingProxy::accumulate_responses_delta(parsed, *accumulated_text);
                        }
                    } catch (...) {}
                }
            }
        });
        return success;
    };

    telemetry_sink.is_writable = [&sink]() -> bool {
        return sink.is_writable ? sink.is_writable() : true;
    };

    telemetry_sink.done = [&sink]() {
        if (sink.done) {
            sink.done();
        }
    };

    telemetry_sink.done_with_trailer = [&sink](const httplib::Headers& trailer) {
        if (sink.done_with_trailer) {
            sink.done_with_trailer(trailer);
        } else if (sink.done) {
            sink.done();
        }
    };

    try {
        execute_streaming(request_body, telemetry_sink, [&](WrappedServer* server) {
            ModelTelemetryIdentity identity = get_telemetry_identity(server);

            if (span) {
                span->set_attribute("llm.backend", identity.recipe);
                span->set_attribute("llm.device_type", identity.device);
                span->set_attribute("llm.checkpoint", identity.checkpoint);
                span->set_attribute("llm.recipe", identity.recipe);
                if (request_json.contains("temperature")) span->set_attribute("llm.config.temperature", request_json["temperature"]);
                if (request_json.contains("top_p")) span->set_attribute("llm.config.top_p", request_json["top_p"]);
                if (request_json.contains("max_tokens")) span->set_attribute("llm.config.max_tokens", request_json["max_tokens"]);
            }

            server->forward_streaming_request("/v1/responses", request_body, telemetry_sink, true, 0,
                [this, identity, span, accumulated_text, server](int input_tokens,
                                 int output_tokens,
                                 double time_to_first_token,
                                 double tokens_per_second,
                                 const std::string& error_message) {
                    if (!error_message.empty()) {
                        if (span) {
                            span->end_with_error(error_message);
                        }
                        return;
                    }
                    record_telemetry_for_model(identity, input_tokens, output_tokens,
                                               time_to_first_token, tokens_per_second);
                    record_prompt_tokens_for_model(identity, input_tokens);

                    if (span) {
                        nlohmann::json usage_payload = {
                            {"prompt_tokens", input_tokens},
                            {"completion_tokens", output_tokens}
                        };
                        span->set_attribute("llm.performance.time_to_first_token", time_to_first_token);
                        span->set_attribute("llm.performance.tokens_per_second", tokens_per_second);

                        std::string url;
                        std::function<std::map<std::string, nlohmann::json>(const std::string&)> parser;
                        if (server) {
                            url = server->get_additional_telemetry_url();
                            parser = server->get_additional_telemetry_parser();
                        }
                        telemetry::end_llm_span_async(span, url, parser, usage_payload, *accumulated_text);
                    }
                });
        }, span);
    } catch (const std::exception& e) {
        if (span) span->end_with_error(e.what());
        throw;
    } catch (...) {
        if (span) span->end_with_error("Unknown error during streaming");
        throw;
    }
}

int Router::count_pinned_servers_by_type(ModelType type) const {
    int count = 0;
    for (const auto& server : loaded_servers_) {
        // Unmetered servers (cloud) never occupy a slot, so they don't count.
        if (is_unmetered_recipe(server->get_recipe_options().get_recipe())) {
            continue;
        }
        if (server->is_backend_alive() && server->get_model_type() == type && server->is_pinned()) {
            count++;
        }
    }
    return count;
}

json Router::get_pinned_model_counts() const {
    std::lock_guard<std::mutex> lock(load_mutex_);
    return {
        {"llm", count_pinned_servers_by_type(ModelType::LLM)},
        {"embedding", count_pinned_servers_by_type(ModelType::EMBEDDING)},
        {"reranking", count_pinned_servers_by_type(ModelType::RERANKING)},
        {"transcription", count_pinned_servers_by_type(ModelType::TRANSCRIPTION)},
        {"image", count_pinned_servers_by_type(ModelType::IMAGE)},
        {"tts", count_pinned_servers_by_type(ModelType::TTS)},
        {"classification", count_pinned_servers_by_type(ModelType::CLASSIFICATION)}
    };
}

void Router::set_model_pinned(const std::string& model_name, bool pinned) {
    std::unique_lock<std::mutex> lock(load_mutex_);
    wait_for_slot_clearance(lock);
    WrappedServer* server = find_server_by_model_name(model_name);
    if (!server) {
        throw std::runtime_error("Model not loaded: " + model_name);
    }
    server->set_pinned(pinned);
}

} // namespace lemon
