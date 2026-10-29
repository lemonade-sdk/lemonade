#pragma once

#include <nlohmann/json.hpp>

#include <sstream>
#include <string>

namespace lemon {
namespace backends {
namespace onnxruntime {

// Whether the ort-server that answered GET /health with `health` reports the
// cuts it makes to fit its token window (the `truncated` field). That started
// in 0.4.0, the first release whose /health carries a "version".
inline bool reports_truncation(const nlohmann::json& health) {
    if (!health.is_object() || !health.contains("version") || !health["version"].is_string()) {
        return false;
    }
    std::istringstream in(health["version"].get<std::string>());
    int major = -1;
    int minor = -1;
    char dot = 0;
    if (!(in >> major >> dot >> minor) || dot != '.' || major < 0 || minor < 0) {
        return false;
    }
    return major > 0 || minor >= 4;
}

}  // namespace onnxruntime
}  // namespace backends
}  // namespace lemon
