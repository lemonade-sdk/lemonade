#pragma once

#include <functional>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace lemon::mcp_3d {

using json = nlohmann::json;

// Small transport-neutral adapter result used to compose MCP orchestration with
// the existing Lemonade server generation handlers. The body may contain JSON
// (image generation) or arbitrary binary bytes (GLB generation).
struct GenerationResponse {
    int status = 200;
    std::string content_type;
    std::string body;
};

using GenerationFn = std::function<GenerationResponse(const json&)>;

struct PipelineResult {
    bool reference_generated = false;
    std::string reference_image_base64;
    std::string glb_bytes;
    std::string glb_mime_type = "model/gltf-binary";
};

inline constexpr const char* kReferenceImageSize = "1024x1024";
inline constexpr const char* kReconstructionReferenceGuidance =
    "single centered subject; whole object visible; three-quarter view; "
    "slightly elevated camera; plain background; even studio lighting; "
    "clear geometry and surface detail";

std::string build_reconstruction_reference_prompt(const std::string& user_prompt);

// Validate reconstruction options before any reference-image backend is called,
// so a bad 3D option cannot waste an expensive image generation.
std::optional<std::string> validate_3d_options(const json& arguments);

// Pull a concise human-readable message out of a server-handler response.
std::string response_error_message(const GenerationResponse& response);

// Pure orchestration core. Model selection is deliberately outside this helper
// so tests can inject fakes without constructing Router/ModelManager instances.
PipelineResult run_pipeline(const json& arguments,
                            const std::string& model_3d,
                            const std::string& image_model,
                            const GenerationFn& generate_image,
                            const GenerationFn& generate_3d);

}  // namespace lemon::mcp_3d
