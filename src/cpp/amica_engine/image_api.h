#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace amica_engine {

struct Image {
    std::size_t width = 0;
    std::size_t height = 0;
    std::vector<std::uint8_t> rgb;  // rows top to bottom, 3 bytes per pixel
    double generate_ms = 0.0;
};

// The image model the HTTP API generates with.
class ImageGenerator {
public:
    virtual ~ImageGenerator() = default;

    // The image size the model was prepared for; requests cannot change it.
    virtual std::size_t width() const = 0;
    virtual std::size_t height() const = 0;
    virtual std::size_t default_steps() const = 0;
    virtual std::size_t max_steps() const = 0;

    virtual Image generate(const std::string& prompt, std::size_t steps, std::uint64_t seed) = 0;

    virtual bool device_removed() = 0;
};

// Serves /v1/images/generations, /v1/models and /health until the process
// exits. Returns non-zero when the port cannot be bound.
int serve_images(const std::string& host, int port, const std::string& model_name, ImageGenerator& generator);

}  // namespace amica_engine
