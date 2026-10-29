#include "lemon/backends/amicaimage/amicaimage_server.h"
#include "lemon/backends/amicaimage/amicaimage.h"
#include "lemon/backends/amica/amica_server.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/error_types.h"
#include <stdexcept>
#include <string>
#include <lemon/utils/aixlog.hpp>

namespace lemon {
namespace backends {

InstallParams AmicaImageServer::get_install_params(const std::string& backend, const std::string& version) {
    return AmicaServer::get_install_params(backend, version);
}

AmicaImageServer::AmicaImageServer(const std::string& log_level, ModelManager* model_manager,
                                   BackendManager* backend_manager)
    : WrappedServer("amica-image-server", log_level, model_manager, backend_manager) {
}

void AmicaImageServer::load(const std::string& model_name,
                            const ModelInfo& model_info,
                            const RecipeOptions& options,
                            bool do_not_upgrade) {
    (void)do_not_upgrade;
    LOG(INFO, "AMICA") << "Loading image model: " << model_name << std::endl;
    LOG(DEBUG, "AMICA") << "Per-model settings: " << options.to_log_string() << std::endl;

    image_defaults_ = model_info.image_defaults;
    port_ = choose_port();
    const ServerCommand command =
        amica::engine_command(model_info, model_name, port_, options.get_option("amica_args"));
    const bool inherit_output = log_level_ == "info" || is_debug();
    start_server(std::make_unique<NativeProcess>(ProcessOutput{inherit_output, true}), command);
}

json AmicaImageServer::chat_completion(const json& /* request */) {
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Chat completion", "amica-image (image generation model)"));
}

json AmicaImageServer::completion(const json& /* request */) {
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Text completion", "amica-image (image generation model)"));
}

json AmicaImageServer::responses(const json& /* request */) {
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Responses", "amica-image (image generation model)"));
}

json AmicaImageServer::image_generations(const json& request) {
    json body = request;
    body.erase("model");
    if (image_defaults_.has_defaults) {
        if (!body.contains("steps") && !body.contains("num_inference_steps") && image_defaults_.steps > 0) {
            body["steps"] = image_defaults_.steps;
        }
        if (!body.contains("size") && !body.contains("width") && !body.contains("height") &&
            image_defaults_.width > 0 && image_defaults_.height > 0) {
            body["size"] = std::to_string(image_defaults_.width) + "x" + std::to_string(image_defaults_.height);
        }
    }
    return forward_request("/v1/images/generations", body);
}

json AmicaImageServer::image_edits(const json& /* request */) {
    return ErrorResponse::from_exception(UnsupportedOperationException("Image edits", "amica-image"));
}

json AmicaImageServer::image_variations(const json& /* request */) {
    return ErrorResponse::from_exception(UnsupportedOperationException("Image variations", "amica-image"));
}

namespace amicaimage {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<AmicaImageServer>(ctx);
}

const BackendSpec* spec() { return make_spec<AmicaImageServer>(descriptor); }

// Installation and model registration are the amica recipe's: the same AMICA
// install, the same engine, and local bundles only.
const BackendOps* ops() { return amica::ops(); }

}  // namespace amicaimage
}  // namespace backends
}  // namespace lemon
