#include "lemon/mcp_3d_orchestrator.h"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

using lemon::mcp_3d::GenerationResponse;
using lemon::mcp_3d::PipelineResult;
using lemon::mcp_3d::json;

namespace {

#define CHECK_TRUE(expr)                                                        \
    do {                                                                        \
        if (!(expr)) {                                                          \
            std::cerr << __func__ << ": check failed at line " << __LINE__     \
                      << ": " #expr << std::endl;                              \
            return false;                                                       \
        }                                                                       \
    } while (false)

bool throws_with(const std::function<void()>& fn, const std::string& needle) {
    try {
        fn();
    } catch (const std::exception& e) {
        return std::string(e.what()).find(needle) != std::string::npos;
    }
    return false;
}

GenerationResponse image_ok(const std::string& b64 = "ZmFrZS1wbmc=") {
    return GenerationResponse{
        200,
        "application/json",
        json{{"data", json::array({json{{"b64_json", b64}}})}}.dump(),
    };
}

GenerationResponse glb_ok() {
    return GenerationResponse{200, "model/gltf-binary", std::string("glb\0payload", 11)};
}

bool test_image_only_skips_reference_generation() {
    int image_calls = 0;
    int reconstruction_calls = 0;
    json seen_3d;
    json args = {
        {"image", "existing-image-base64"},
        {"resolution", 1024},
        {"bg_removal", "threshold"},
        {"seed", 42},
        {"uv", "xatlas"},
    };

    PipelineResult result = lemon::mcp_3d::run_pipeline(
        args,
        "Trellis-Model",
        "",
        [&](const json&) {
            ++image_calls;
            return image_ok();
        },
        [&](const json& request) {
            ++reconstruction_calls;
            seen_3d = request;
            return glb_ok();
        });

    CHECK_TRUE(image_calls == 0);
    CHECK_TRUE(reconstruction_calls == 1);
    CHECK_TRUE(!result.reference_generated);
    CHECK_TRUE(seen_3d["image"] == "existing-image-base64");
    CHECK_TRUE(seen_3d["resolution"] == 1024);
    CHECK_TRUE(seen_3d["bg_removal"] == "threshold");
    CHECK_TRUE(seen_3d["seed"] == 42);
    CHECK_TRUE(seen_3d["uv"] == "xatlas");
    CHECK_TRUE(seen_3d["response_format"] == "glb");
    return true;
}

bool test_prompt_only_generates_reference_once_and_keeps_resolutions_independent() {
    int image_calls = 0;
    int reconstruction_calls = 0;
    json seen_image;
    json seen_3d;
    json args = {
        {"prompt", "a small industrial centrifugal pump"},
        {"resolution", 1536},
        {"bg_removal", "birefnet"},
        {"seed", 7},
        {"uv", "box"},
    };

    PipelineResult result = lemon::mcp_3d::run_pipeline(
        args,
        "Trellis-Model",
        "SD-Model",
        [&](const json& request) {
            ++image_calls;
            seen_image = request;
            return image_ok("cmVmZXJlbmNlLWltYWdl");
        },
        [&](const json& request) {
            ++reconstruction_calls;
            seen_3d = request;
            return glb_ok();
        });

    CHECK_TRUE(image_calls == 1);
    CHECK_TRUE(reconstruction_calls == 1);
    CHECK_TRUE(result.reference_generated);
    CHECK_TRUE(result.reference_image_base64 == "cmVmZXJlbmNlLWltYWdl");
    CHECK_TRUE(seen_image["model"] == "SD-Model");
    CHECK_TRUE(seen_image["n"] == 1);
    CHECK_TRUE(seen_image["size"] == "1024x1024");
    CHECK_TRUE(seen_image["response_format"] == "b64_json");
    CHECK_TRUE(seen_image["prompt"].get<std::string>().find(
                   "a small industrial centrifugal pump") != std::string::npos);
    CHECK_TRUE(seen_image["prompt"].get<std::string>().find(
                   "single centered subject") != std::string::npos);
    CHECK_TRUE(!seen_image.contains("resolution"));
    CHECK_TRUE(!seen_image.contains("bg_removal"));
    CHECK_TRUE(!seen_image.contains("seed"));
    CHECK_TRUE(!seen_image.contains("uv"));
    CHECK_TRUE(seen_3d["image"] == "cmVmZXJlbmNlLWltYWdl");
    CHECK_TRUE(seen_3d["resolution"] == 1536);
    CHECK_TRUE(seen_3d["bg_removal"] == "birefnet");
    CHECK_TRUE(seen_3d["seed"] == 7);
    CHECK_TRUE(seen_3d["uv"] == "box");
    return true;
}

bool test_neither_and_both_rejected_before_backends() {
    int image_calls = 0;
    int reconstruction_calls = 0;
    auto image = [&](const json&) {
        ++image_calls;
        return image_ok();
    };
    auto reconstruction = [&](const json&) {
        ++reconstruction_calls;
        return glb_ok();
    };

    CHECK_TRUE(throws_with(
        [&] { lemon::mcp_3d::run_pipeline(json::object(), "Trellis", "SD", image, reconstruction); },
        "exactly one"));
    CHECK_TRUE(throws_with(
        [&] {
            lemon::mcp_3d::run_pipeline(
                json{{"image", "a"}, {"prompt", "b"}},
                "Trellis", "SD", image, reconstruction);
        },
        "exactly one"));
    CHECK_TRUE(image_calls == 0);
    CHECK_TRUE(reconstruction_calls == 0);
    return true;
}

bool test_reference_generation_error_stops_pipeline() {
    int reconstruction_calls = 0;
    CHECK_TRUE(throws_with(
        [&] {
            lemon::mcp_3d::run_pipeline(
                json{{"prompt", "pump"}},
                "Trellis",
                "SD",
                [](const json&) {
                    return GenerationResponse{
                        500,
                        "application/json",
                        json{{"error", json{{"message", "diffusion exploded"}}}}.dump(),
                    };
                },
                [&](const json&) {
                    ++reconstruction_calls;
                    return glb_ok();
                });
        },
        "Reference image generation failed: diffusion exploded"));
    CHECK_TRUE(reconstruction_calls == 0);
    return true;
}

bool test_empty_reference_image_is_clear_error() {
    int reconstruction_calls = 0;
    CHECK_TRUE(throws_with(
        [&] {
            lemon::mcp_3d::run_pipeline(
                json{{"prompt", "pump"}},
                "Trellis",
                "SD",
                [](const json&) {
                    return GenerationResponse{
                        200,
                        "application/json",
                        json{{"data", json::array()}}.dump(),
                    };
                },
                [&](const json&) {
                    ++reconstruction_calls;
                    return glb_ok();
                });
        },
        "Reference image generation failed: backend returned no image"));
    CHECK_TRUE(reconstruction_calls == 0);
    return true;
}

bool test_3d_error_has_stage_name() {
    CHECK_TRUE(throws_with(
        [&] {
            lemon::mcp_3d::run_pipeline(
                json{{"image", "existing"}},
                "Trellis",
                "",
                [](const json&) { return image_ok(); },
                [](const json&) {
                    return GenerationResponse{
                        502,
                        "application/json",
                        json{{"error", json{{"message", "mesh backend OOM"}}}}.dump(),
                    };
                });
        },
        "3D reconstruction failed: mesh backend OOM"));
    return true;
}

bool test_invalid_3d_options_rejected_before_backends() {
    const std::vector<json> invalid = {
        json{{"prompt", "pump"}, {"resolution", 999}},
        json{{"prompt", "pump"}, {"bg_removal", "boolean-ish"}},
        json{{"prompt", "pump"}, {"uv", "magic"}},
    };

    for (const auto& args : invalid) {
        int image_calls = 0;
        int reconstruction_calls = 0;
        const bool threw = throws_with(
            [&] {
                lemon::mcp_3d::run_pipeline(
                    args,
                    "Trellis",
                    "SD",
                    [&](const json&) {
                        ++image_calls;
                        return image_ok();
                    },
                    [&](const json&) {
                        ++reconstruction_calls;
                        return glb_ok();
                    });
            },
            "must be");
        CHECK_TRUE(threw);
        CHECK_TRUE(image_calls == 0);
        CHECK_TRUE(reconstruction_calls == 0);
    }
    return true;
}

}  // namespace

int main() {
    const std::vector<std::pair<const char*, bool (*)()>> tests = {
        {"image-only regression", test_image_only_skips_reference_generation},
        {"prompt orchestration", test_prompt_only_generates_reference_once_and_keeps_resolutions_independent},
        {"xor validation", test_neither_and_both_rejected_before_backends},
        {"reference backend failure", test_reference_generation_error_stops_pipeline},
        {"empty reference", test_empty_reference_image_is_clear_error},
        {"3d backend failure", test_3d_error_has_stage_name},
        {"3d option validation", test_invalid_3d_options_rejected_before_backends},
    };

    for (const auto& [name, fn] : tests) {
        if (!fn()) {
            std::cerr << "FAILED: " << name << std::endl;
            return 1;
        }
        std::cout << "PASS: " << name << std::endl;
    }
    return 0;
}
