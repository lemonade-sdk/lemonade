#pragma once

#include "lemon/wrapped_server.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/container_backend.h"
#include <cstdint>

namespace lemon {
namespace backends {

// Wraps antirez's ds4-server (DwarfStar) as a lemonade backend. ds4-server is
// a self-contained OpenAI-compatible HTTP server for the DeepSeek V4 family;
// lemonade runs it inside the pinned DS4 toolbox image and proxies requests,
// exactly like the other LLM backends.
class Ds4Server : public WrappedServer {
public:
    Ds4Server(const std::string& log_level, ModelManager* model_manager,
              BackendManager* backend_manager);
    ~Ds4Server() override;

    void load(const std::string& model_name, const ModelInfo& model_info,
              const RecipeOptions& options, bool do_not_upgrade = false) override;
    void unload() override;

    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;
};

// DS4's container needs host IPC and SYS_PTRACE on top of the shared ROCm
// passthrough, so it names its own device profile. It also carries the
// migration off the binary build it used to install.
class Ds4Ops : public ContainerBackendOps {
public:
    Ds4Ops() : ContainerBackendOps("ds4") {}
    std::string profile_id(const std::string& variant) const override {
        (void)variant;
        return "ds4-rocm";
    }
    bool install(const std::string& backend, bool force,
                 DownloadProgressCallback progress) const override;
    bool uninstall(const std::string& backend) const override;

    // DS4 used to install a ds4-server binary from lemonade-sdk/ds4-rocm. That
    // build is gone; nothing will ever launch it again, so it is removed rather
    // than left occupying several GB. Returns the directory it removed, or "".
    static std::string remove_legacy_binary_install();
};

namespace ds4 {
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<Ds4Server>(); }
}  // namespace ds4

}  // namespace backends
}  // namespace lemon
