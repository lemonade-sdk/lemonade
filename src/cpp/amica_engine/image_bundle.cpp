#include "image_bundle.h"

#include "graph_abi.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace amica_engine {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// GenaiApp's pipeline catalog (cgc_pipeline_catalog.py): the latent-space
// constants of each supported source model, which the bundle does not record.
struct Family {
    const char* source;
    std::size_t vae_channels;
    std::size_t vae_scale;
    float scale_factor;
    float shift_factor;
};

constexpr Family kFamilies[] = {
    {"FLUX.1-schnell", 16, 8, 0.3611f, 0.1159f},
};

const char* const kComponents[] = {"text_encoder", "text_encoder_2", "transformer", "vae_decoder"};

std::string path_of(const fs::path& path) { return path.u8string(); }

}  // namespace

bool ImageBundle::is_image_bundle(const std::string& model_dir) {
    return fs::is_regular_file(fs::u8path(model_dir) / "cgc_pipeline.json");
}

ImageBundle::ImageBundle(const std::string& model_dir, const std::string& transformer_precision)
    : dir_(model_dir) {
    if (transformer_precision != "source" && transformer_precision != "int4-weight-only") {
        throw std::runtime_error("transformer precision must be source or int4-weight-only, not " +
                                 transformer_precision);
    }
    const fs::path root = fs::u8path(model_dir);
    const std::string manifest_path = path_of(root / "cgc_pipeline.json");
    const json manifest = json::parse(read_text_file(manifest_path));
    source_ = manifest.value("source", "");
    width_ = manifest.at("width").get<std::size_t>();
    height_ = manifest.at("height").get<std::size_t>();
    text_length_ = manifest.at("text_length").get<std::size_t>();

    const Family* family = nullptr;
    for (const auto& candidate : kFamilies) {
        if (source_.find(candidate.source) != std::string::npos) family = &candidate;
    }
    if (family == nullptr) {
        throw std::runtime_error(manifest_path + ": source '" + source_ +
                                 "' is not an image model amica-engine supports (FLUX.1-schnell)");
    }
    vae_channels_ = family->vae_channels;
    vae_scale_ = family->vae_scale;
    scale_factor_ = family->scale_factor;
    shift_factor_ = family->shift_factor;
    if (width_ == 0 || height_ == 0 || width_ % (vae_scale_ * 2) != 0 || height_ % (vae_scale_ * 2) != 0) {
        throw std::runtime_error(manifest_path + ": the image size must be a non-zero multiple of " +
                                 std::to_string(vae_scale_ * 2));
    }

    const auto listed = manifest.value("components", std::vector<std::string>());
    for (const char* name : kComponents) {
        if (std::find(listed.begin(), listed.end(), name) == listed.end()) {
            throw std::runtime_error(manifest_path + " does not list the " + std::string(name) + " component");
        }
        fs::path folder = root / name;
        if (std::string(name) == "transformer" && transformer_precision != "source") folder /= transformer_precision;
        Component component{path_of(folder / "source_fp16act.mlir"), path_of(folder / "weights.safetensors")};
        for (const auto& file : {component.graph, component.weights}) {
            if (!fs::is_regular_file(fs::u8path(file))) {
                throw std::runtime_error(file + " was not found; prepare the bundle with GenaiApp first");
            }
        }
        components_.emplace(name, std::move(component));
    }
    for (const auto& tokenizer : {clip_tokenizer(), t5_tokenizer()}) {
        if (!fs::is_regular_file(fs::u8path(tokenizer))) throw std::runtime_error(tokenizer + " was not found");
    }

    const std::string scheduler_path = path_of(root / "onnx" / "scheduler" / "scheduler_config.json");
    const json scheduler = json::parse(read_text_file(scheduler_path));
    if (scheduler.value("use_dynamic_shifting", false)) {
        throw std::runtime_error(scheduler_path + ": dynamic shifting is not supported");
    }
    shift_ = scheduler.value("shift", 1.0f);
}

const ImageBundle::Component& ImageBundle::component(const std::string& name) const {
    return components_.at(name);
}

std::string ImageBundle::clip_tokenizer() const {
    return path_of(fs::u8path(dir_) / "onnx" / "tokenizer" / "model.onnx");
}

std::string ImageBundle::t5_tokenizer() const {
    return path_of(fs::u8path(dir_) / "onnx" / "tokenizer_2" / "model.onnx");
}

// As GenaiApp: np.linspace(1, 1/steps, steps) in f32, then the fixed shift.
std::vector<float> ImageBundle::sigmas(std::size_t steps) const {
    std::vector<float> result;
    const double start = 1.0;
    const double stop = 1.0 / static_cast<double>(steps);
    const double step = steps > 1 ? (stop - start) / static_cast<double>(steps - 1) : 0.0;
    for (std::size_t i = 0; i < steps; ++i) {
        const float value = i + 1 == steps && steps > 1 ? static_cast<float>(stop)
                                                        : static_cast<float>(start + static_cast<double>(i) * step);
        result.push_back(shift_ * value / (1.0f + (shift_ - 1.0f) * value));
    }
    result.push_back(0.0f);
    return result;
}

}  // namespace amica_engine
