#include "lemon/server/job_ops.h"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>

#include <lemon/utils/aixlog.hpp>

#include "lemon/model_manager.h"
#include "lemon/router.h"
#include "lemon/server/model_json.h"
#include "lemon/system_info.h"
#include "lemon/system_metrics_platform.h"

namespace lemon {

lemon::jobs::OpProviders JobOps::build(ServerContext& ctx) {
    lemon::jobs::OpProviders providers;
    Router* router = ctx.router;
    ModelManager* model_manager = ctx.model_manager;
    ModelJson* model_json = ctx.model_json;
    SystemMetricsPlatform* metrics = ctx.metrics;

    // A running job remembers which models were loaded before it started (snapshot)
    // and which ones it loaded itself (owned), so pausing it can unload exactly its own
    // models and resuming can restore them.
    struct JobModelState {
        std::map<std::string, bool> snapshot;
        std::map<std::string, nlohmann::json> owned;
    };
    auto job_states = std::make_shared<std::map<std::string, JobModelState>>();
    auto current_job = std::make_shared<std::string>();
    auto state_mutex = std::make_shared<std::mutex>();

    providers.system_info = [] {
        return lemon::jobs::json::parse(SystemInfoCache::get_system_info_with_cache().dump());
    };
    providers.system_stats = [metrics] {
        lemon::jobs::json stats;
        const double cpu_percent = metrics->get_cpu_usage();
        stats["cpu_percent"] =
            cpu_percent >= 0 ? lemon::jobs::json(cpu_percent) : lemon::jobs::json();
        stats["memory_gb"] = metrics->get_memory_usage_gb();
        const double gpu_percent = metrics->get_gpu_usage();
        stats["gpu_percent"] =
            gpu_percent >= 0 ? lemon::jobs::json(gpu_percent) : lemon::jobs::json();
        const double vram_gb = metrics->get_vram_usage_gb();
        stats["vram_gb"] = vram_gb >= 0 ? lemon::jobs::json(vram_gb) : lemon::jobs::json();
        const double npu_percent = metrics->get_npu_utilization();
        stats["npu_percent"] =
            npu_percent >= 0 ? lemon::jobs::json(npu_percent) : lemon::jobs::json();
        return stats;
    };
    providers.models_list = [model_manager, model_json] {
        nlohmann::json data = nlohmann::json::array();
        for (const auto& [model_id, info] : model_manager->get_supported_models())
            data.push_back(model_json->to_json(model_id, info));
        nlohmann::json response = {{"object", "list"}, {"data", data}};
        return lemon::jobs::json::parse(response.dump());
    };
    providers.model_get = [model_manager, model_json](const std::string& id) -> lemon::jobs::json {
        auto models = model_manager->get_supported_models();
        auto it = models.find(id);
        if (it == models.end()) return lemon::jobs::json(nullptr);
        return lemon::jobs::json::parse(model_json->to_json(id, it->second).dump());
    };
    providers.load_op = [router, model_manager, job_states, current_job, state_mutex](
                            const lemon::jobs::json& params,
                            lemon::jobs::CancelFlag& cancel) -> lemon::jobs::json {
        if (!params.contains("model") || !params["model"].is_string())
            throw lemon::jobs::JobError(400, "load requires a 'model' string");
        const std::string model = params["model"].get<std::string>();
        if (!model_manager->model_exists(model))
            throw lemon::jobs::JobError(404, "unknown model '" + model + "'");
        if (!model_manager->is_model_downloaded(model))
            throw lemon::jobs::JobError(404, "model '" + model + "' is not downloaded");
        auto info = model_manager->get_model_info(model);
        nlohmann::json opt_json = nlohmann::json::parse(params.dump());
        RecipeOptions options(info.recipe, opt_json);
        std::optional<bool> pinned = std::nullopt;
        if (params.contains("pinned") && params["pinned"].is_boolean())
            pinned = params["pinned"].get<bool>();
        try {
            router->load_model(model, info, options, true, true, pinned,
                               LoadPurpose::UserInference, &cancel);
        } catch (const std::exception& e) {
            throw lemon::jobs::JobError(500, e.what());
        }
        {
            const std::string canonical = router->canonical_model_name(model);
            const int pid = router->loaded_model_pid(model);
            std::lock_guard<std::mutex> lk(*state_mutex);
            if (!current_job->empty()) {
                auto it = job_states->find(*current_job);
                if (it != job_states->end())
                    it->second.owned[canonical] = {{"pid", pid}};
            }
        }
        if (cancel.load()) {
            throw lemon::jobs::JobError(499, "interrupted");
        }
        RecipeOptions effective = router->get_model_recipe_options(model);
        lemon::jobs::json out;
        out["loaded"] = true;
        out["model"] = model;
        const nlohmann::json backend_json = effective.get_option(info.recipe + "_backend");
        if (backend_json.is_string()) out["backend"] = backend_json.get<std::string>();
        const nlohmann::json ctx_json = effective.get_option("ctx_size");
        if (ctx_json.is_number()) out["ctx_size"] = ctx_json.get<int64_t>();
        return out;
    };
    providers.unload_op = [router, job_states, current_job, state_mutex](
                              const lemon::jobs::json& params,
                              lemon::jobs::CancelFlag&) -> lemon::jobs::json {
        std::string model;
        if (params.contains("model") && params["model"].is_string())
            model = params["model"].get<std::string>();
        if (!model.empty()) {
            if (router->is_model_loaded(model)) router->unload_model(model);
        } else {
            router->unload_model("");
        }
        {
            std::lock_guard<std::mutex> lk(*state_mutex);
            if (!current_job->empty()) {
                auto it = job_states->find(*current_job);
                if (it != job_states->end()) {
                    if (model.empty()) it->second.owned.clear();
                    else it->second.owned.erase(router->canonical_model_name(model));
                }
            }
        }
        return lemon::jobs::json::object();
    };
    providers.chat_op = [router](const lemon::jobs::json& params,
                                 lemon::jobs::CancelFlag& cancel) -> lemon::jobs::json {
        nlohmann::json request = nlohmann::json::parse(params.dump());
        nlohmann::json response = router->chat_completion(request, &cancel);
        if (response.contains("error")) {
            std::string msg = "chat failed";
            const auto& err = response["error"];
            if (err.is_object() && err.contains("message") && err["message"].is_string())
                msg = err["message"].get<std::string>();
            else if (err.is_string())
                msg = err.get<std::string>();
            throw lemon::jobs::JobError(424, msg);
        }
        return lemon::jobs::json::parse(response.dump());
    };
    providers.begin_exclusive = [router, job_states, current_job, state_mutex](
                                    const std::string& job_id,
                                    lemon::jobs::CancelFlag* cancel) -> bool {
        if (!router->begin_exclusive(cancel)) return false;
        std::lock_guard<std::mutex> lk(*state_mutex);
        if (!job_states->count(job_id))
            (*job_states)[job_id].snapshot = router->snapshot_loaded_models();
        *current_job = job_id;
        return true;
    };
    providers.end_exclusive = [router, current_job, state_mutex] {
        {
            std::lock_guard<std::mutex> lk(*state_mutex);
            current_job->clear();
        }
        router->end_exclusive();
    };
    providers.reconcile_unload = [router, job_states, state_mutex](
                                     const std::string& job_id) {
        std::map<std::string, int> owned_live;
        std::map<std::string, bool> snapshot;
        {
            std::lock_guard<std::mutex> lk(*state_mutex);
            auto it = job_states->find(job_id);
            if (it == job_states->end()) return;
            for (const auto& kv : it->second.owned)
                if (kv.second.is_object() && kv.second.contains("pid"))
                    owned_live[kv.first] = kv.second["pid"].get<int>();
            snapshot = it->second.snapshot;
        }
        if (owned_live.empty()) return;
        auto captured = router->unload_job_models(owned_live, snapshot);
        std::lock_guard<std::mutex> lk(*state_mutex);
        auto it = job_states->find(job_id);
        if (it == job_states->end()) return;
        for (auto& kv : captured) it->second.owned[kv.first] = std::move(kv.second);
    };
    providers.restore_exclusive = [router, model_manager, job_states, state_mutex](
                                      const std::string& job_id,
                                      const lemon::jobs::json& manifest,
                                      lemon::jobs::CancelFlag* cancel) -> bool {
        std::map<std::string, nlohmann::json> to_restore;
        std::set<std::string> tracked;
        {
            std::lock_guard<std::mutex> lk(*state_mutex);
            auto it = job_states->find(job_id);
            if (it != job_states->end()) {
                for (const auto& kv : it->second.owned) {
                    tracked.insert(kv.first);
                    if (kv.second.is_object() && kv.second.contains("options"))
                        to_restore[kv.first] = kv.second;
                }
            }
        }
        for (auto it = manifest.begin(); it != manifest.end(); ++it) {
            const std::string canonical = router->canonical_model_name(it.key());
            if (tracked.count(canonical) || to_restore.count(canonical)) continue;
            nlohmann::json params = nlohmann::json::parse(it.value().dump());
            nlohmann::json entry = {{"options", params}};
            if (params.contains("pinned") && params["pinned"].is_boolean())
                entry["pinned"] = params["pinned"].get<bool>();
            to_restore[canonical] = std::move(entry);
        }
        for (const auto& kv : to_restore) {
            if (cancel && cancel->load()) return false;
            if (router->is_model_loaded(kv.first)) {
                std::lock_guard<std::mutex> lk(*state_mutex);
                auto it = job_states->find(job_id);
                if (it != job_states->end()) it->second.owned.erase(kv.first);
                continue;
            }
            try {
                auto info = model_manager->get_model_info(kv.first);
                RecipeOptions options(info.recipe, kv.second.value("options", nlohmann::json::object()));
                std::optional<bool> pinned = std::nullopt;
                if (kv.second.contains("pinned") && kv.second["pinned"].is_boolean())
                    pinned = kv.second["pinned"].get<bool>();
                router->load_model(kv.first, info, options, true, true, pinned,
                                   LoadPurpose::UserInference, cancel);
                const int pid = router->loaded_model_pid(kv.first);
                std::lock_guard<std::mutex> lk(*state_mutex);
                auto it = job_states->find(job_id);
                if (it != job_states->end()) it->second.owned[kv.first] = {{"pid", pid}};
            } catch (const std::exception& e) {
                if (cancel && cancel->load()) return false;
                LOG(WARNING, "Jobs") << "could not restore job model '" << kv.first
                                     << "' on resume: " << e.what() << std::endl;
                std::lock_guard<std::mutex> lk(*state_mutex);
                auto it = job_states->find(job_id);
                if (it != job_states->end()) it->second.owned.erase(kv.first);
            }
        }
        return true;
    };
    providers.discard_exclusive = [job_states, state_mutex](const std::string& job_id) {
        std::lock_guard<std::mutex> lk(*state_mutex);
        job_states->erase(job_id);
    };
    return providers;
}

} // namespace lemon
