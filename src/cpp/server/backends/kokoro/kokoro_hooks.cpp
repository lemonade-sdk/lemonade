#include "lemon/backends/kokoro/kokoro_hooks.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/json_utils.h"

#include <filesystem>
#include <stdexcept>

namespace lemon::backends::kokoro {

json voice_index(const LaunchHookContext& context) {
    const std::filesystem::path index = context.values.at("artifacts").at("main").get<std::string>();
    LOG(INFO, context.category.c_str()) << "Reading " << index.filename() << std::endl;
    json model_index;
    try {
        model_index = utils::JsonUtils::load_from_file(index.string());
    } catch (const std::exception& error) {
        throw std::runtime_error("Warning: Could not load " + index.filename().string() + ": " + error.what());
    }
    LOG(INFO, context.category.c_str()) << "Using model: " << model_index["model"] << std::endl;
    return model_index;
}

}
