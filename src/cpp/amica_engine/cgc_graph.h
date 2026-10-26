#pragma once

#include <cstddef>
#include <string>

namespace amica_engine {

// The small CGC graphs a flow-matching image pipeline runs between its model
// graphs on the GPU, emitted as CGC MLIR. A latent of C channels and H x W is
// packed into 2x2 patches as [1, H*W/4, C*4] for the transformer.
struct LatentShape {
    std::size_t channels = 0;
    std::size_t height = 0;
    std::size_t width = 0;
};

// noise [1,C,H,W] f32 -> the packed latent as f32 and f16.
std::string latent_initial_graph(const LatentShape& shape);

// packed latent f32, predicted velocity f16, step size f32[1] -> the next
// packed latent as f32 and f16.
std::string scheduler_step_graph(const LatentShape& shape);

// packed latent f32 -> the [1,C,H,W] f32 VAE decoder input.
std::string decode_input_graph(const LatentShape& shape, float scale_factor, float shift_factor);

}  // namespace amica_engine
