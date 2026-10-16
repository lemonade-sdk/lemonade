#pragma once

namespace lemon {

class AliasManager;
class BackendManager;
class CloudProviderRegistry;
class ConfigEffects;
class DownloadManager;
class HttpListener;
class ModelJson;
class ModelLoader;
class ModelManager;
class RouteRegistry;
class Router;
class RuntimeConfig;
class Server;
class SystemMetricsPlatform;

namespace jobs {
class JobManager;
}

// Routes reach every subsystem and service through this one struct, so adding a
// dependency to a route never touches Server's member list. Everything it points
// to lives as long as lemond.
struct ServerContext {
    Server* server = nullptr;
    Router* router = nullptr;
    ModelManager* model_manager = nullptr;
    BackendManager* backend_manager = nullptr;
    CloudProviderRegistry* cloud_registry = nullptr;
    AliasManager* alias_manager = nullptr;
    RuntimeConfig* config = nullptr;
    SystemMetricsPlatform* metrics = nullptr;
    jobs::JobManager* job_manager = nullptr;

    HttpListener* listener = nullptr;
    RouteRegistry* registry = nullptr;
    ConfigEffects* config_effects = nullptr;
    ModelLoader* model_loader = nullptr;
    ModelJson* model_json = nullptr;
    DownloadManager* downloads = nullptr;
};

} // namespace lemon
