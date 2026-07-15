#pragma once

#include "lemon/backends/backend_registry.h"

#include "lemon/wrapped_server.h"
#include "lemon/server_capabilities.h"
#include "lemon/backends/backend_utils.h"
#include <string>

namespace lemon {
namespace backends {

// Runs an exported ONNX model as an ort-server subprocess. v1 serves text
// classification (/v1/classify): input text -> {label: score}, CPU EP. The
// server is generic; embeddings/reranking are future capabilities on the same
// backend (issue #2592).
class OnnxRuntimeServer : public WrappedServer, public IClassificationServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    explicit OnnxRuntimeServer(const std::string& log_level,
                               ModelManager* model_manager,
                               BackendManager* backend_manager);

    ~OnnxRuntimeServer() override;

    void load(const std::string& model_name,
              const ModelInfo& model_info,
              const RecipeOptions& options,
              bool do_not_upgrade = false) override;

    void unload() override;

    // IClassificationServer
    json classify(const json& request) override;

private:
    // Forward a classify request to the subprocess and normalize its response.
    json forward_classify(const std::string& text, const json& params);
};

namespace onnxruntime {
// Factory for the onnxruntime backend (constructs the server class — lemond only).
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
}  // namespace onnxruntime
}  // namespace backends
}  // namespace lemon
