#pragma once

#include "lemon/backends/configured/configured_server.h"

namespace lemon::backends {

struct ConfiguredServerTestAccess {
    class Launch final : public LaunchServices {
    public:
        explicit Launch(int port) : port_(port) {}
        bool exists(const std::string&) const override { return true; }
        std::vector<std::string> supported_backends(const std::string&) const override { return {"cpu"}; }
        void validate_backend(const std::string&, const std::string&) const override {}
        std::string external_binary(const BackendSpec&, const std::string&) const override { return "fake-llama-server"; }
        void install_backend(const std::string&, const std::string&) override {}
        std::string managed_binary(const BackendSpec&, const std::string&) const override { return "fake-llama-server"; }
        int allocate_port() override { return port_; }
        int find_free_port(int) override { return port_; }
        std::vector<std::pair<std::string, std::string>> environment(const std::string&, const std::string&,
            const std::string&, const std::string&) const override { return {}; }
        PlatformInfo platform() const override { return {"linux", "x86_64", ""}; }
        long default_timeout_seconds() const override { return 1; }
    private:
        int port_;
    };

    static void load(WrappedServer& server, int port, const std::string& name,
        std::function<std::unique_ptr<ServerProcess>()> process, ModelType mode = ModelType::LLM) {
        auto& configured = dynamic_cast<ConfiguredServer&>(server);
        configured.services_ = {std::make_shared<Launch>(port),
            [process](ProcessOutput, const std::string&) { return process(); }};
        RecipeOptions options("llamacpp", {});
        options.set_option("llamacpp_backend", "");
        ModelInfo model;
        model.model_name = name;
        model.recipe = "llamacpp";
        model.type = mode;
        model.checkpoints["main"] = "fixture/repo:main.gguf";
        model.resolved_paths["main"] = "fake-main.gguf";
        server.set_model_metadata(name, "fixture/repo", mode, DEVICE_CPU, options);
        configured.load(name, model, options);
        server.set_state(ModelState::READY);
    }
};

}
