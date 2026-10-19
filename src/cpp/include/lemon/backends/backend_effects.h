#pragma once

#include "lemon/backends/backend_load_helpers.h"
#include "lemon/server_process.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lemon::backends {

struct BackendEffects {
    BackendBinaryServices binary;
    std::function<bool(const std::string&)> exists;
    std::function<std::vector<std::string>(const std::string&)> supported_backends;
    std::function<void(const std::string&, const std::string&)> validate_backend;
    std::function<int()> allocate_port;
    std::function<int(int)> find_free_port;
    std::function<std::string()> rocm_arch;
    std::function<std::string(const std::string&)> runtime_directories;
    std::function<std::optional<std::string>()> inherited_loader;
    std::function<std::optional<std::string>()> inherited_posix_loader;
    std::function<void(std::vector<std::pair<std::string, std::string>>&, const std::string&)> append_cuda;
    std::function<std::unique_ptr<ServerProcess>(ProcessOutput, const std::string&)> make_process;
};

BackendEffects production_backend_effects(BackendManager* manager, std::function<int()> allocate_port);

}
