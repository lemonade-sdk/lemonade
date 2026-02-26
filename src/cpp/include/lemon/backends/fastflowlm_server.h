#pragma once

#include "../wrapped_server.h"
#include "backend_utils.h"
#include <string>
#include <stdexcept>

namespace lemon {
namespace backends {

// Structured exception for FLM check failures
class FLMCheckException : public std::runtime_error {
public:
    enum class ErrorType {
        NOT_INSTALLED,
        DRIVER_TOO_OLD,
        VALIDATION_FAILED,
        NPU_NOT_AVAILABLE
    };

    FLMCheckException(ErrorType type, const std::string& message, const std::string& fix_url = "")
        : std::runtime_error(message), type_(type), fix_url_(fix_url) {}

    ErrorType type() const { return type_; }
    const std::string& fix_url() const { return fix_url_; }

private:
    ErrorType type_;
    std::string fix_url_;
};

class FastFlowLMServer : public WrappedServer, public IEmbeddingsServer, public IRerankingServer {
public:
    inline static const BackendSpec SPEC = BackendSpec(
        // recipe
            "flm",
        // executable
    #ifdef _WIN32
            "flm.exe"
    #else
            "flm"
    #endif
    );

    FastFlowLMServer(const std::string& log_level = "info", ModelManager* model_manager = nullptr,
                     BackendManager* backend_manager = nullptr);

    ~FastFlowLMServer() override;

    void install(const std::string& backend = "");
    bool check();

    std::string download_model(const std::string& checkpoint,
                              bool do_not_upgrade = false);

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;

    void unload() override;

    // ICompletionServer implementation
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    // IEmbeddingsServer implementation
    json embeddings(const json& request) override;

    // IRerankingServer implementation
    json reranking(const json& request) override;

    // FLM uses /api/tags for readiness check instead of /health
    bool wait_for_ready();

    // Override to transform model name to checkpoint for FLM
    void forward_streaming_request(const std::string& endpoint,
                                   const std::string& request_body,
                                   httplib::DataSink& sink,
                                   bool sse = true) override;

private:
    // Static helpers for install logic (no instance state needed)
    static std::string get_flm_path();
    static bool check_npu_available();

    // Version management
    static std::string get_flm_required_version();
    static std::string get_flm_installed_version();
    static bool compare_versions(const std::string& v1, const std::string& v2);

    // NPU driver check (static - no instance state needed)
    static std::string get_min_npu_driver_version();
    static std::string get_npu_driver_version();
    static bool check_npu_driver_version();
    bool validate();

    // Installation - returns true if FLM was upgraded (may invalidate existing models)
    static bool install_flm_if_needed();
    static bool download_flm_installer(const std::string& output_path);
    static void run_flm_installer(const std::string& installer_path, bool silent);

    // Environment management
    static void refresh_environment_path();
    static bool verify_flm_installation(const std::string& expected_version, int max_retries = 10);

    // Cache management (function-local static in .cpp)
    static void invalidate_version_cache();

    bool is_loaded_ = false;
    bool flm_was_upgraded_ = false;
};

} // namespace backends
} // namespace lemon
