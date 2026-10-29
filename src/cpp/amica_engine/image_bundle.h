#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace amica_engine {

// An AMICA GenaiApp image bundle, as GenaiApp prepares it: cgc_pipeline.json,
// a CGC graph and weights per component, and the source model's ONNX
// tokenizers and scheduler config under onnx\.
class ImageBundle {
public:
    // transformer_precision names the transformer variant GenaiApp prepared:
    // "source" (the model's own FP16 weights) or "int4-weight-only".
    explicit ImageBundle(const std::string& model_dir, const std::string& transformer_precision = "source");

    static bool is_image_bundle(const std::string& model_dir);

    struct Component {
        std::string graph;
        std::string weights;
    };
    const Component& component(const std::string& name) const;

    const std::string& source() const { return source_; }
    std::size_t width() const { return width_; }
    std::size_t height() const { return height_; }
    std::size_t text_length() const { return text_length_; }

    // The source model's latent space: SD3/FLUX-style VAE scaling and shift.
    std::size_t vae_channels() const { return vae_channels_; }
    std::size_t vae_scale() const { return vae_scale_; }
    float scale_factor() const { return scale_factor_; }
    float shift_factor() const { return shift_factor_; }

    std::string clip_tokenizer() const;
    std::string t5_tokenizer() const;

    // The flow-matching noise levels of a `steps`-step schedule, ending in 0.
    std::vector<float> sigmas(std::size_t steps) const;

private:
    std::string dir_;
    std::string source_;
    std::size_t width_ = 0;
    std::size_t height_ = 0;
    std::size_t text_length_ = 0;
    std::size_t vae_channels_ = 0;
    std::size_t vae_scale_ = 0;
    float scale_factor_ = 0.0f;
    float shift_factor_ = 0.0f;
    float shift_ = 1.0f;
    std::map<std::string, Component> components_;
};

}  // namespace amica_engine
