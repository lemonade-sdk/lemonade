#pragma once

#include "lemon/backends/configured/backend_hook_registry.h"

namespace lemon::backends::onnxruntime {

const BackendOps* model_directory_ops();
json servable_directory(const LaunchHookContext& context);
json classify(const OperationHookContext& context, const json& request);
std::string startup_error(const std::string& executable, const std::vector<std::string>& args);

}
