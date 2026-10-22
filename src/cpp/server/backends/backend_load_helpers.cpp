#include "lemon/backends/backend_load_helpers.h"

#include <filesystem>

#include "lemon/backend_manager.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/error_types.h"

namespace lemon::backends {

std::string resolve_external_or_managed_binary(
    const BackendSpec& spec,
    const std::string& backend,
    const BackendBinaryServices& services) {
    std::string external = services.find_external(spec.recipe, backend);
    if (!external.empty() && services.exists(external)) {
        return external;
    }
    services.install(spec.recipe, backend);
    return services.managed_path(spec, backend);
}

BackendBinaryServices backend_binary_services(BackendManager& manager) {
    return {
        BackendUtils::find_external_backend_binary,
        [](const std::string& path) { return std::filesystem::exists(path); },
        [&manager](const std::string& recipe, const std::string& backend) {
            manager.install_backend(recipe, backend);
        },
        BackendUtils::get_backend_binary_path,
    };
}

std::string first_supported_backend(
    const std::vector<std::string>& supported,
    const std::string& operation,
    const std::string& target) {
    if (supported.empty()) {
        throw UnsupportedOperationException(operation, target);
    }
    return supported.front();
}

std::pair<std::string, std::string> make_loader_path_override(
    const std::string& executable_directory,
    const std::string& extra_directories,
    const std::optional<std::string>& inherited_value,
    bool windows) {
    const std::string separator = windows ? ";" : ":";
    std::string value = extra_directories.empty()
        ? executable_directory : extra_directories + separator + executable_directory;
    if (inherited_value) {
        value += separator + *inherited_value;
    }
    return {windows ? "PATH" : "LD_LIBRARY_PATH", value};
}

}
