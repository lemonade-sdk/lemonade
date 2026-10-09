#include "lemon/server.h"

#include <lemon/utils/aixlog.hpp>

#include "lemon/alias_manager.h"
#include "lemon/backend_manager.h"
#include "lemon/cloud_provider_registry.h"
#include "lemon/jobs/job_manager.h"
#include "lemon/model_manager.h"
#include "lemon/router.h"
#include "lemon/runtime_config.h"
#include "lemon/server/config_effects.h"
#include "lemon/server/download_manager.h"
#include "lemon/server/http_listener.h"
#include "lemon/server/job_ops.h"
#include "lemon/server/model_json.h"
#include "lemon/server/model_loader.h"
#include "lemon/server/request_middleware.h"
#include "lemon/server/route_registry.h"
#include "lemon/server/router_dispatch.h"
#include "lemon/server/web_ui.h"
#include "lemon/system_metrics_platform.h"
#include "telemetry.h"

namespace lemon {

// Each route folder's routes.cpp adds its routes in the order httplib should match them.
void register_openai_text_routes(RouteRegistry& registry, ServerContext& ctx);
void register_openai_audio_routes(RouteRegistry& registry, ServerContext& ctx);
void register_openai_images_routes(RouteRegistry& registry, ServerContext& ctx);
void register_lemonade_models_routes(RouteRegistry& registry, ServerContext& ctx);
void register_openai_models_routes(RouteRegistry& registry, ServerContext& ctx);
void register_lemonade_load_routes(RouteRegistry& registry, ServerContext& ctx);
void register_lemonade_downloads_routes(RouteRegistry& registry, ServerContext& ctx);
void register_lemonade_inference_routes(RouteRegistry& registry, ServerContext& ctx);
void register_lemonade_backends_routes(RouteRegistry& registry, ServerContext& ctx);
void register_lemonade_system_routes(RouteRegistry& registry, ServerContext& ctx);
void register_lemonade_jobs_routes(RouteRegistry& registry, ServerContext& ctx);
void register_llamacpp_routes(RouteRegistry& registry, ServerContext& ctx);
void register_router_routes(RouteRegistry& registry, ServerContext& ctx);
void register_internal_routes(RouteRegistry& registry, ServerContext& ctx);
void register_ollama_routes(RouteRegistry& registry, ServerContext& ctx);
void register_anthropic_routes(RouteRegistry& registry, ServerContext& ctx);
void register_mcp_routes(RouteRegistry& registry, ServerContext& ctx);

Server::Server(std::shared_ptr<RuntimeConfig> config,
               const std::string& cache_dir,
               const std::string& config_dir)
    : config_(config), config_dir_(config_dir) {
    ctx_.server = this;
    ctx_.config = config_.get();

    config_effects_ = std::make_unique<ConfigEffects>(ctx_);
    ctx_.config_effects = config_effects_.get();
    config_effects_->apply("global_timeout");
    config_effects_->apply("download_rate_limit");

    cloud_registry_ = std::make_unique<CloudProviderRegistry>();
    // Seed installed providers from config.json. Runtime keys stay empty
    // until either an env var resolves them per-request or a client POSTs
    // /v1/cloud/auth — by design we never persist secrets to disk.
    {
        nlohmann::json snap = config_->snapshot();
        if (snap.contains("cloud_providers")) {
            cloud_registry_->load_from_config(snap["cloud_providers"]);
        }
    }

    alias_manager_ = std::make_unique<AliasManager>(cache_dir);
    model_manager_ = std::make_unique<ModelManager>(config_->extra_models_dir());
    model_manager_->set_cloud_registry(cloud_registry_.get());
    model_manager_->set_default_model_source_provider(
        [this]() { return config_->default_model_source(); });

    backend_manager_ = std::make_unique<BackendManager>();
    BackendManager::set_global(backend_manager_.get());

    router_ = std::make_unique<Router>(config_.get(),
                                       model_manager_.get(),
                                       backend_manager_.get());
    router_->set_cloud_registry(cloud_registry_.get());

    // When a router collection is added, edited, or removed (via the API or an
    // on-disk edit), reclaim any routing helper no remaining policy references.
    model_manager_->set_models_changed_callback([this](uint64_t generation) {
        router_->reconcile_routing_helpers(active_policy_helper_models(*model_manager_),
                                           generation);
    });

    // Seed the router's needed-helper set from policies already present at
    // startup so a helper loaded before the first policy change still validates
    // against an authoritative set (see Router::load_model). Reserve the
    // generation BEFORE snapshotting the policies: the directory watcher may
    // already be publishing newer snapshots, and evaluating next_notify_generation()
    // after computing the snapshot (argument evaluation order is unspecified in
    // C++) could stamp this stale snapshot with a newer generation and clobber
    // the watcher's authoritative state.
    const uint64_t seed_generation = model_manager_->next_notify_generation();
    const std::set<std::string> seed_needed = active_policy_helper_models(*model_manager_);
    router_->reconcile_routing_helpers(seed_needed, seed_generation);

    model_manager_->set_model_updated_callback([this](const std::string& model_name) {
        if (router_ && router_->is_model_loaded(model_name)) {
            LOG(INFO, "Server") << "Evicting updated model from memory: " << model_name << std::endl;
            router_->unload_model(model_name);
        }
    });

    metrics_platform_ = create_metrics_platform();
    model_json_ = std::make_unique<ModelJson>(model_manager_.get(), router_.get(),
                                              alias_manager_.get());
    model_loader_ = std::make_unique<ModelLoader>(router_.get(), model_manager_.get());
    downloads_ = std::make_unique<DownloadManager>();

    ctx_.router = router_.get();
    ctx_.model_manager = model_manager_.get();
    ctx_.backend_manager = backend_manager_.get();
    ctx_.cloud_registry = cloud_registry_.get();
    ctx_.alias_manager = alias_manager_.get();
    ctx_.metrics = metrics_platform_.get();
    ctx_.model_json = model_json_.get();
    ctx_.model_loader = model_loader_.get();
    ctx_.downloads = downloads_.get();

    job_manager_ = std::make_unique<lemon::jobs::JobManager>(
        cache_dir, config_dir, lemon::jobs::build_op_registry(JobOps::build(ctx_)));
    ctx_.job_manager = job_manager_.get();

    LOG(DEBUG, "Server") << "Debug logging enabled - subprocess output will be visible" << std::endl;

    registry_ = std::make_unique<RouteRegistry>();
    ctx_.registry = registry_.get();
    // Pages list routes in registration order. lemonade/models adds the 405 answers for
    // GET models/register and models/check-updates, which must precede openai/models'
    // models/{id} pattern.
    register_openai_text_routes(*registry_, ctx_);
    register_openai_audio_routes(*registry_, ctx_);
    register_openai_images_routes(*registry_, ctx_);
    register_lemonade_models_routes(*registry_, ctx_);
    register_openai_models_routes(*registry_, ctx_);
    register_lemonade_load_routes(*registry_, ctx_);
    register_lemonade_downloads_routes(*registry_, ctx_);
    register_lemonade_inference_routes(*registry_, ctx_);
    register_lemonade_backends_routes(*registry_, ctx_);
    register_lemonade_system_routes(*registry_, ctx_);
    register_lemonade_jobs_routes(*registry_, ctx_);
    register_llamacpp_routes(*registry_, ctx_);
    register_router_routes(*registry_, ctx_);
    register_internal_routes(*registry_, ctx_);
    register_ollama_routes(*registry_, ctx_);
    register_anthropic_routes(*registry_, ctx_);
    register_mcp_routes(*registry_, ctx_);

    middleware_ = std::make_unique<RequestMiddleware>(config_.get(), registry_.get());
    web_ui_ = std::make_unique<WebUi>(ctx_);
    listener_ = std::make_unique<HttpListener>(
        config_.get(), router_.get(),
        [this](httplib::Server& server) {
            middleware_->install(server);
            registry_->apply(server);
            web_ui_->register_routes(server);
        },
        [this]() { stop(); }, middleware_->api_key_set(), middleware_->admin_api_key_set());
    ctx_.listener = listener_.get();

    start_model_cache_warmup();
}

Server::~Server() {
    downloads_->cancel_all();
    model_manager_->cancel_sync();
    stop();
}

void Server::start_model_cache_warmup() {
    model_cache_warmup_thread_ = std::thread([this]() {
        try {
            LOG(DEBUG, "Server") << "Warming model list cache..." << std::endl;
            model_manager_->get_supported_models();
            LOG(DEBUG, "Server") << "Model list cache warmup complete" << std::endl;
        } catch (const std::exception& e) {
            LOG(WARNING, "Server") << "Model list cache warmup failed: " << e.what() << std::endl;
        } catch (...) {
            LOG(WARNING, "Server") << "Model list cache warmup failed with unknown error" << std::endl;
        }

        if (config_->auto_check_model_updates()) {
            try {
                LOG(DEBUG, "Server") << "Checking downloaded models for updates..." << std::endl;
                auto check_res = model_manager_->check_for_model_updates();
                LOG(DEBUG, "Server") << "Model update check complete" << std::endl;

                for (const auto& [model_name, err] : check_res.failed_models) {
                    LOG(WARNING, "Server") << "Model update check failed for " << model_name << ": " << err << std::endl;
                }

                std::vector<std::string> auto_update_targets;
                for (const auto& public_name : check_res.updated_models) {
                    try {
                        std::string canonical_name = model_manager_->resolve_model_name(public_name);
                        ModelInfo info = model_manager_->get_model_info(canonical_name);
                        if (model_manager_->should_auto_update(info)) {
                            auto_update_targets.push_back(public_name);
                        }
                    } catch (...) {}
                }

                if (!auto_update_targets.empty()) {
                    LOG(INFO, "Server") << "Auto-updating " << auto_update_targets.size() << " model(s)..." << std::endl;
                    (void)model_manager_->sync_models(auto_update_targets, /*dry_run=*/false);
                }
            } catch (const std::exception& e) {
                LOG(WARNING, "Server") << "Model update check failed: " << e.what() << std::endl;
            } catch (...) {
                LOG(WARNING, "Server") << "Model update check failed with unknown error" << std::endl;
            }
        } else {
            LOG(DEBUG, "Server")
                << "Automatic model update checks are disabled" << std::endl;
        }

        update_check_done_ = true;
    });
}

void Server::run() {
    listener_->start();
}

void Server::stop() {
    std::lock_guard<std::mutex> lock(stop_mutex_);
    if (listener_->stop()) {
        LOG(INFO, "Server") << "Unloading models and stopping backend servers..." << std::endl;
        try {
            router_->unload_model();
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "Error during cleanup: " << e.what() << std::endl;
        }

        LOG(INFO, "Server") << "Shutting down telemetry queue..." << std::endl;
        lemon::telemetry::shutdown();

        LOG(INFO, "Server") << "Cleanup complete" << std::endl;
    }

    model_manager_->cancel_sync();

    if (model_cache_warmup_thread_.joinable()) {
        model_cache_warmup_thread_.join();
    }

    model_manager_->join_background_syncs();
}

bool Server::should_shutdown() const {
    return listener_->shutdown_requested();
}

void Server::set_shutdown_requested(bool requested) {
    if (requested) {
        listener_->request_shutdown();
    }
}

bool Server::is_running() const {
    return listener_->is_running();
}

bool Server::startup_failed() const {
    return listener_->startup_failed();
}

void Server::handle_request(const httplib::Request& req, httplib::Response& res) {
    if (!registry_->dispatch(req, res)) {
        res.status = 404;
    }
}

} // namespace lemon
