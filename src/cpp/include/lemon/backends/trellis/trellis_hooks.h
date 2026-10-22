#pragma once

#include "lemon/backends/configured/backend_hook_registry.h"

namespace lemon::backends::trellis {

void mesh(const OperationHookContext& context, const json& request, httplib::DataSink& sink);

}
