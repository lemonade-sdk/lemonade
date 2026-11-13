#pragma once

#include "lemon/backends/configured/backend_hook_registry.h"

namespace lemon::backends::moonshine {

json model_arch(const LaunchHookContext& context);
json transcription(const OperationHookContext& context, const json& request);

}
