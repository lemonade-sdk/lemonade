#pragma once

#include "lemon/backends/configured/backend_hook_registry.h"

namespace lemon::backends::kokoro {

json voice_index(const LaunchHookContext& context);

}
