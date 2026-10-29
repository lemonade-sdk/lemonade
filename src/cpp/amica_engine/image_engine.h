#pragma once

#include "amica_runtime.h"
#include "cgc_graph.h"
#include "image_api.h"
#include "image_bundle.h"
#include "onnx_tokenizers.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace amica_engine {

// Keeps a FLUX-style image bundle resident on the GPU: the CLIP and T5 text
// encoders, the transformer, the VAE decoder and the bridge graphs between
// them. A request runs every graph in one submission, so no intermediate
// tensor returns to the CPU; only the final image is read back.
class ImageEngine : public ImageGenerator {
public:
    ImageEngine(const ImageBundle& bundle, const std::string& adapter, const std::string& arch,
                std::int64_t weights_vram_mib);

    std::string summary() const;

    std::size_t width() const override { return bundle_.width(); }
    std::size_t height() const override { return bundle_.height(); }
    std::size_t default_steps() const override { return 4; }
    std::size_t max_steps() const override;

    Image generate(const std::string& prompt, std::size_t steps, std::uint64_t seed) override;

    bool device_removed() override { return runtime_.device_removed(); }

private:
    Graph compile_component(const std::string& name);
    Graph compile_bridge(const std::string& label, const std::string& ir);
    std::vector<Graph*> graphs();
    std::int64_t weights_budget_mib(std::int64_t requested);
    void allocate();
    ComPtr<ID3D12Resource> output_buffer(Graph& graph, int index);
    void connect(Graph& consumer, std::size_t parameter, Graph& producer, int output);

    const ImageBundle& bundle_;
    ClipTokenizer clip_tokenizer_;
    SentencePieceTokenizer t5_tokenizer_;
    Runtime runtime_;
    Gpu& gpu_;
    LatentShape latent_shape_;

    Graph clip_;
    Graph t5_;
    Graph transformer_;
    Graph vae_;
    Graph initial_;
    Graph scheduler_;
    Graph decode_input_;

    HostInput clip_ids_;
    HostInput t5_ids_;
    HostInput noise_;
    HostInput image_positions_;
    HostInput text_positions_;

    // Each step's sigma and step size, at a fixed stride so that one upload
    // serves the whole request and each step binds its own slot.
    ComPtr<ID3D12Resource> step_table_;
    ComPtr<ID3D12Resource> step_table_upload_;
    std::uint8_t* step_table_mapped_ = nullptr;
    std::uint64_t step_table_size_ = 0;

    // The latent alternates between two buffer pairs so a scheduler step never
    // overwrites the latent it reads.
    ComPtr<ID3D12Resource> latent_[2];
    ComPtr<ID3D12Resource> latent_half_[2];

    ComPtr<ID3D12Resource> readback_;
    const std::uint8_t* readback_mapped_ = nullptr;

    int transformer_latent_ = -1;
    int transformer_sigma_ = -1;
    int scheduler_latent_ = -1;
    int scheduler_step_ = -1;
    int decode_latent_ = -1;
    double load_ms_ = 0.0;
};

}  // namespace amica_engine
