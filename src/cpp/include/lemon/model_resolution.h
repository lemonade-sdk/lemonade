#pragma once

#include <string>

namespace lemon {

class ModelManager;

/// When `requested` is registered, returns it unchanged. Otherwise, if
/// `default_model` is configured in config.json and that model exists,
/// returns the default. If no fallback applies, returns `requested` as-is
/// (caller should check existence before loading).
std::string resolve_model_with_default(const std::string& requested,
                                       ModelManager* model_manager);

} // namespace lemon
