#pragma once

#include "../wrapped_server.h"
#include "../server_capabilities.h"
#include <string>
#include <filesystem>

namespace lemon {
namespace backends {

class SDServer : public WrappedServer, public IImageServer {
public:
    explicit SDServer(const std::string& log_level = "info",
                      ModelManager* model_manager = nullptr,
                      bool save_images = false,
                      const std::string& images_dir = "");

    ~SDServer() override;

    // WrappedServer interface
    void install(const std::string& backend = "") override;

    std::string download_model(const std::string& checkpoint,
                              const std::string& mmproj = "",
                              bool do_not_upgrade = false) override;

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             int ctx_size,
             bool do_not_upgrade = false,
             const std::string& llamacpp_backend = "",
             const std::string& llamacpp_args = "") override;

    void unload() override;

    // ICompletionServer implementation (not supported - return errors)
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    // IImageServer implementation
    json image_generations(const json& request) override;

private:
    std::string get_sd_executable_path();
    std::string find_executable_in_install_dir(const std::string& install_dir);
    std::string find_external_sd_executable();

    // Image generation helpers
    std::string generate_output_path();
    std::string run_sd_cli(const std::string& prompt,
                           const std::string& output_path,
                           int width,
                           int height,
                           int steps,
                           float cfg_scale,
                           int64_t seed);
    std::string read_image_as_base64(const std::string& path);
    void cleanup_temp_file(const std::string& path);

    std::string model_path_;
    std::filesystem::path temp_dir_;  // Directory for temporary output images
    bool save_images_;                // Whether to keep generated images
    std::string images_dir_;          // Directory for saved images (if save_images_ is true)
};

} // namespace backends
} // namespace lemon
