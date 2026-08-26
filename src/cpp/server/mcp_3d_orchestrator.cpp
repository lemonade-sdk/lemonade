#include "lemon/mcp_3d_orchestrator.h"

#include <stdexcept>
#include <utility>

namespace lemon::mcp_3d {

namespace {

bool has_nonempty_string(const json& object, const char* key) {
    auto it = object.find(key);
    return it != object.end() && it->is_string() && !it->get<std::string>().empty();
}

std::string error_from_json(const json& payload) {
    if (!payload.is_object()) return {};

    auto error = payload.find("error");
    if (error != payload.end()) {
        if (error->is_string()) return error->get<std::string>();
        if (error->is_object()) {
            auto message = error->find("message");
            if (message != error->end() && message->is_string()) {
                return message->get<std::string>();
            }
        }
    }

    auto message = payload.find("message");
    if (message != payload.end() && message->is_string()) {
        return message->get<std::string>();
    }
    return {};
}

GenerationResponse invoke_stage(const char* label,
                                const GenerationFn& fn,
                                const json& request) {
    if (!fn) {
        throw std::runtime_error(std::string(label) + ": server generation callback is not configured");
    }
    try {
        return fn(request);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string(label) + ": " + e.what());
    }
}

}  // namespace

std::string build_reconstruction_reference_prompt(const std::string& user_prompt) {
    return user_prompt +
        "\n\n3D reconstruction reference requirements: " +
        kReconstructionReferenceGuidance + ".";
}

std::optional<std::string> validate_3d_options(const json& arguments) {
    if (arguments.contains("resolution")) {
        if (!arguments["resolution"].is_number_integer()) {
            return "'resolution' must be an integer: 512, 1024, or 1536";
        }
        const int resolution = arguments["resolution"].get<int>();
        if (resolution != 512 && resolution != 1024 && resolution != 1536) {
            return "'resolution' must be 512, 1024, or 1536";
        }
    }
    if (arguments.contains("bg_removal")) {
        const auto& value = arguments["bg_removal"];
        if (!value.is_string() || (value != "threshold" && value != "birefnet")) {
            return "'bg_removal' must be 'threshold' or 'birefnet'";
        }
    }
    if (arguments.contains("seed") && !arguments["seed"].is_number_integer()) {
        return "'seed' must be an integer";
    }
    if (arguments.contains("uv")) {
        const auto& value = arguments["uv"];
        if (!value.is_string() || (value != "box" && value != "xatlas")) {
            return "'uv' must be 'box' or 'xatlas'";
        }
    }
    return std::nullopt;
}

std::string response_error_message(const GenerationResponse& response) {
    if (!response.body.empty()) {
        try {
            const json payload = json::parse(response.body);
            const std::string parsed = error_from_json(payload);
            if (!parsed.empty()) return parsed;
        } catch (const std::exception&) {
            // Fall through to the raw body for non-JSON backend errors.
        }
        return response.body;
    }
    if (response.status >= 400) {
        return "server returned HTTP " + std::to_string(response.status) + " with an empty body";
    }
    return "server returned an empty response";
}

PipelineResult run_pipeline(const json& arguments,
                            const std::string& model_3d,
                            const std::string& image_model,
                            const GenerationFn& generate_image,
                            const GenerationFn& generate_3d) {
    if (!arguments.is_object()) {
        throw std::invalid_argument("3D tool arguments must be an object");
    }

    const bool has_image = arguments.contains("image");
    const bool has_prompt = arguments.contains("prompt");
    if (has_image == has_prompt) {
        throw std::invalid_argument("Provide exactly one of 'image' or 'prompt'");
    }
    if (model_3d.empty()) {
        throw std::invalid_argument("3D model must not be empty");
    }
    if (auto validation_error = validate_3d_options(arguments)) {
        throw std::invalid_argument(*validation_error);
    }

    PipelineResult result;
    std::string reconstruction_image;

    if (has_prompt) {
        if (!has_nonempty_string(arguments, "prompt")) {
            throw std::invalid_argument("'prompt' must be a non-empty string");
        }
        if (image_model.empty()) {
            throw std::invalid_argument("image model must not be empty for prompt-to-3D");
        }

        const json image_request = {
            {"model", image_model},
            {"prompt", build_reconstruction_reference_prompt(
                arguments["prompt"].get<std::string>())},
            {"n", 1},
            {"size", kReferenceImageSize},
            {"response_format", "b64_json"},
        };

        const GenerationResponse image_response = invoke_stage(
            "Reference image generation failed", generate_image, image_request);
        if (image_response.status >= 400) {
            throw std::runtime_error(
                "Reference image generation failed: " + response_error_message(image_response));
        }

        json image_payload;
        try {
            image_payload = json::parse(image_response.body);
        } catch (const std::exception& e) {
            throw std::runtime_error(
                std::string("Reference image generation failed: invalid JSON response: ") + e.what());
        }
        const std::string payload_error = error_from_json(image_payload);
        if (!payload_error.empty()) {
            throw std::runtime_error("Reference image generation failed: " + payload_error);
        }
        if (!image_payload.contains("data") || !image_payload["data"].is_array() ||
            image_payload["data"].empty() ||
            !image_payload["data"][0].contains("b64_json") ||
            !image_payload["data"][0]["b64_json"].is_string() ||
            image_payload["data"][0]["b64_json"].get<std::string>().empty()) {
            throw std::runtime_error(
                "Reference image generation failed: backend returned no image");
        }

        reconstruction_image = image_payload["data"][0]["b64_json"].get<std::string>();
        result.reference_generated = true;
        result.reference_image_base64 = reconstruction_image;
    } else {
        if (!has_nonempty_string(arguments, "image")) {
            throw std::invalid_argument("'image' must be a non-empty string");
        }
        reconstruction_image = arguments["image"].get<std::string>();
    }

    json reconstruction_request = {
        {"model", model_3d},
        {"image", std::move(reconstruction_image)},
        {"response_format", "glb"},
    };
    for (const char* key : {"resolution", "bg_removal", "seed", "uv"}) {
        if (arguments.contains(key)) reconstruction_request[key] = arguments[key];
    }

    const GenerationResponse reconstruction_response = invoke_stage(
        "3D reconstruction failed", generate_3d, reconstruction_request);
    if (reconstruction_response.status >= 400) {
        throw std::runtime_error(
            "3D reconstruction failed: " + response_error_message(reconstruction_response));
    }
    if (reconstruction_response.body.empty()) {
        throw std::runtime_error("3D reconstruction failed: backend returned an empty GLB");
    }

    result.glb_bytes = reconstruction_response.body;
    if (!reconstruction_response.content_type.empty()) {
        result.glb_mime_type = reconstruction_response.content_type;
    }
    return result;
}

}  // namespace lemon::mcp_3d
