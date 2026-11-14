#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lemon {

class BackendManager;

namespace backends {

struct BackendSpec;

struct BackendBinaryServices {
    std::function<std::string(const std::string&, const std::string&)> find_external;
    std::function<bool(const std::string&)> exists;
    std::function<void(const std::string&, const std::string&)> install;
    std::function<std::string(const BackendSpec&, const std::string&)> managed_path;
};

std::string resolve_external_or_managed_binary(
    const BackendSpec& spec,
    const std::string& backend,
    const BackendBinaryServices& services);

BackendBinaryServices backend_binary_services(BackendManager& manager);

std::string first_supported_backend(
    const std::vector<std::string>& supported,
    const std::string& operation,
    const std::string& target);

std::pair<std::string, std::string> make_loader_path_override(
    const std::string& executable_directory,
    const std::string& extra_directories,
    const std::optional<std::string>& inherited_value,
    bool windows);

}
}
