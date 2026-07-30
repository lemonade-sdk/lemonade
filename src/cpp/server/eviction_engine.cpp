#include "lemon/eviction_engine.h"
#include "lemon/router.h"
#include "lemon/global_vram_monitor.h"
#include "lemon/wrapped_server.h"
#include "lemon/utils/aixlog.hpp"
#include <chrono>
#include <thread>

namespace lemon {

EvictionEngine::EvictionEngine(Router* router, GlobalVramMonitor* vram_monitor)
    : router_(router), vram_monitor_(vram_monitor), running_(false), interval_ms_(5000) {
    if (vram_monitor_) {
        vram_monitor_->set_pressure_callback([this](double pct) {
            this->on_vram_pressure(pct);
        });
    }
}

EvictionEngine::~EvictionEngine() {
    stop();
}

void EvictionEngine::start(int interval_ms) {
    if (running_) return;
    interval_ms_ = interval_ms;
    running_ = true;
    engine_thread_ = std::thread(&EvictionEngine::evaluation_loop, this);
}

void EvictionEngine::stop() {
    running_ = false;
    if (engine_thread_.joinable()) {
        engine_thread_.join();
    }
}

void EvictionEngine::on_vram_pressure(double pct) {
    double threshold = RuntimeConfig::global()->auto_evict_threshold_pct();
    if (pct >= threshold) {
        LOG(INFO) << "VRAM pressure critical (" << (pct * 100.0) << "% >= " << (threshold * 100.0) << "%). Evaluating eviction." << std::endl;
        evaluate_servers(pct);
    }
}

void EvictionEngine::evaluation_loop() {
    while (running_) {
        // Also perform regular time-based idle checks
        evaluate_servers(-1.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
    }
}

void EvictionEngine::evaluate_servers(double current_vram_pct) {
    std::string model_to_evict;
    std::vector<std::string> models_to_downsize;

    {
        std::lock_guard<std::mutex> lock(router_->load_mutex_);

        // An exclusive job session owns model residency; auto-eviction and
        // downsizing would yank models out from under its next step.
        if (router_->exclusive_active_) return;

        auto now = std::chrono::steady_clock::now();
        double threshold = RuntimeConfig::global()->auto_evict_threshold_pct();
        bool pressure_evict = (current_vram_pct >= threshold);

        WrappedServer* best_candidate_for_eviction = nullptr;
        double highest_eviction_score = -1.0;

        for (auto& server_ptr : router_->loaded_servers_) {
            WrappedServer* server = server_ptr.get();
            if (!server) continue;

            // Pinned models must never be auto-evicted or downsized
            if (server->is_pinned()) continue;

            // Check auto_evict config
            bool auto_evict = RuntimeConfig::global()->auto_evict();
            auto recipe_opts = server->get_recipe_options().to_json();
            if (recipe_opts.contains("auto_evict") && recipe_opts["auto_evict"].is_boolean()) {
                auto_evict = recipe_opts["auto_evict"].get<bool>();
            }

            if (!auto_evict) continue;

            long evict_timeout_sec = 300;
            long downsize_timeout_sec = 60;
            double weight_factor = 1.0;

            if (recipe_opts.contains("evict_idle_timeout") && recipe_opts["evict_idle_timeout"].is_number_integer()) {
                evict_timeout_sec = recipe_opts["evict_idle_timeout"].get<long>();
            }
            if (recipe_opts.contains("downsize_idle_timeout") && recipe_opts["downsize_idle_timeout"].is_number_integer()) {
                downsize_timeout_sec = recipe_opts["downsize_idle_timeout"].get<long>();
            }
            if (recipe_opts.contains("evict_weight_factor") && recipe_opts["evict_weight_factor"].is_number()) {
                weight_factor = recipe_opts["evict_weight_factor"].get<double>();
            }
            if (weight_factor <= 0.0) {
                weight_factor = 1.0;  // guard against divide-by-zero / non-positive config
            }

            auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - server->get_last_access_time()).count();
            long load_duration_ms = server->get_load_duration_ms() > 0 ? server->get_load_duration_ms() : 1000;

            // Higher score => more disposable. Fast-loading models score high (cheap
            // to reload) and are evicted first; slow/expensive loads are protected.
            // A larger evict_weight_factor further protects a model from eviction.
            //   eviction_score = idle_time_ms / (load_duration_ms * weight_factor)
            double eviction_score =
                static_cast<double>(idle_ms) / (static_cast<double>(load_duration_ms) * weight_factor);

            ModelState state = server->get_state();

            // 1. Time-based hard idle eviction
            if (idle_ms >= evict_timeout_sec * 1000 && state != ModelState::EVICTING && state != ModelState::UNLOADED && state != ModelState::IN_USE) {
                LOG(INFO) << "Model " << server->get_model_name() << " reached evict idle timeout (" << evict_timeout_sec << "s). Evicting." << std::endl;
                server->set_state(ModelState::EVICTING);
                best_candidate_for_eviction = server;
                pressure_evict = true;
                break;
            }

            // 2. Time-based soft idle (downsize) — collect the candidate only. The
            // model is not claimed here; try_begin_downsize() below atomically
            // re-checks that it is still idle and transitions it to DOWNSIZING.
            if (idle_ms >= downsize_timeout_sec * 1000 && state == ModelState::READY) {
                LOG(INFO) << "Model " << server->get_model_name() << " reached downsize idle timeout (" << downsize_timeout_sec << "s). Marking for downsize." << std::endl;
                models_to_downsize.push_back(server->get_model_name());
            }

            // 3. VRAM Pressure tracking
            if (pressure_evict && state != ModelState::EVICTING && state != ModelState::UNLOADED && state != ModelState::IN_USE) {
                if (eviction_score > highest_eviction_score) {
                    highest_eviction_score = eviction_score;
                    best_candidate_for_eviction = server;
                }
            }
        }

        if (pressure_evict && best_candidate_for_eviction) {
            model_to_evict = best_candidate_for_eviction->get_model_name();
            best_candidate_for_eviction->set_state(ModelState::EVICTING);
            LOG(INFO) << "Eviction Engine unloading model: " << model_to_evict << " due to score/pressure/idle." << std::endl;
        }
    } // release lock

    // Perform downsizes outside the lock so they don't block the router. Each
    // downsize is an owned maintenance operation: try_begin_downsize() (under the
    // router lock) atomically claims the model and marks it busy, so a concurrent
    // evict_server() waits in wait_until_not_busy() instead of unloading and
    // destroying the server while we still hold a raw pointer to it. The matching
    // finish_downsize() releases that guard and records success/failure.
    for (const auto& name : models_to_downsize) {
        WrappedServer* s = nullptr;
        {
            std::lock_guard<std::mutex> lk(router_->load_mutex_);
            if (router_->exclusive_active_) break;
            s = router_->find_server_by_model_name(name);
            if (!s || !s->try_begin_downsize()) {
                continue;  // gone, busy, or no longer idle since phase 1
            }
        }
        // s is kept alive by the maintenance guard set in try_begin_downsize().
        bool ok = s->downsize();
        s->finish_downsize(ok);
        if (ok) {
            LOG(INFO) << "Model " << name << " downsized." << std::endl;
        } else {
            LOG(WARNING) << "Downsize of " << name << " failed; left ready." << std::endl;
        }
    }

    if (!model_to_evict.empty()) {
        // Race-safe: only unloads if the model hasn't been rescued by an
        // in-flight request since we marked it EVICTING above.
        router_->evict_if_committed(model_to_evict);
    }
}

} // namespace lemon
