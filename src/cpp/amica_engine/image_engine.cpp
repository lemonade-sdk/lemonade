#include "image_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>
#include <stdexcept>

namespace amica_engine {

namespace {

// Weights at least this large stay outside the compiled graphs, so they are
// uploaded once under the VRAM budget instead of embedded in persistent buffers.
constexpr std::uint64_t kExternalWeightsMinBytes = 1ull << 20;

constexpr std::size_t kMaxSteps = 50;
constexpr std::uint64_t kStepStride = 256;

// The requirements list a graph's inputs in parameter order, named after the
// parameters.
int parameter_input(const Graph& graph, std::size_t parameter) {
    const int named = graph.input_index("arg" + std::to_string(parameter));
    if (named >= 0) return named;
    if (parameter < graph.requirements.num_inputs) return static_cast<int>(parameter);
    throw std::runtime_error(graph.label + " has no input for parameter arg" + std::to_string(parameter));
}

void write_float(std::uint8_t* destination, std::int32_t element_type, float value) {
    if (element_type == AMICA_ELEMENT_FP32) {
        std::memcpy(destination, &value, sizeof(value));
    } else if (element_type == AMICA_ELEMENT_FP16) {
        const std::uint16_t half = float_to_half(value);
        std::memcpy(destination, &half, sizeof(half));
    } else {
        throw std::runtime_error("a floating-point input is neither FP32 nor FP16");
    }
}

// Truncated to `length` with the end token kept last, or padded.
std::vector<std::int64_t> fit(std::vector<std::int64_t> ids, std::size_t length, std::int64_t end, std::int64_t pad) {
    if (ids.size() >= length) {
        ids.resize(length);
        ids.back() = end;
    } else {
        ids.resize(length, pad);
    }
    return ids;
}

}  // namespace

ImageEngine::ImageEngine(const ImageBundle& bundle, const std::string& adapter, const std::string& arch,
                         std::int64_t weights_vram_mib)
    : bundle_(bundle),
      clip_tokenizer_(bundle.clip_tokenizer()),
      t5_tokenizer_(bundle.t5_tokenizer()),
      runtime_(adapter, arch),
      gpu_(runtime_.gpu()) {
    const auto started = std::chrono::steady_clock::now();
    latent_shape_ = {bundle.vae_channels(), bundle.height() / bundle.vae_scale(), bundle.width() / bundle.vae_scale()};

    clip_ = compile_component("text_encoder");
    t5_ = compile_component("text_encoder_2");
    transformer_ = compile_component("transformer");
    vae_ = compile_component("vae_decoder");
    initial_ = compile_bridge("latent_initial", latent_initial_graph(latent_shape_));
    scheduler_ = compile_bridge("scheduler_step", scheduler_step_graph(latent_shape_));
    decode_input_ = compile_bridge(
        "decode_input", decode_input_graph(latent_shape_, bundle.scale_factor(), bundle.shift_factor()));

    runtime_.upload_weights(graphs(), weights_budget_mib(weights_vram_mib));
    allocate();
    runtime_.initialize_graphs(graphs());
    load_ms_ = milliseconds_since(started);
}

std::string ImageEngine::summary() const {
    return "adapter='" + gpu_.adapter_name() + "' arch=" + gpu_.gfx_arch() + " size=" +
           std::to_string(bundle_.width()) + "x" + std::to_string(bundle_.height()) +
           " load_ms=" + std::to_string(static_cast<long long>(load_ms_));
}

std::size_t ImageEngine::max_steps() const { return kMaxSteps; }

Graph ImageEngine::compile_component(const std::string& name) {
    const auto& component = bundle_.component(name);
    // Logged first: if amica.dll terminates the process while compiling,
    // this names the graph it was compiling.
    log("compiling " + name + " from " + component.graph);
    MappedFile ir(component.graph);
    amica::compile_options options;
    options.external_weights_min_bytes = kExternalWeightsMinBytes;
    return runtime_.compile(name, ir.text(), &runtime_.open_resources(component.weights), options);
}

Graph ImageEngine::compile_bridge(const std::string& label, const std::string& ir) {
    log("compiling " + label);
    return runtime_.compile(label, ir, nullptr, amica::compile_options());
}

std::vector<Graph*> ImageEngine::graphs() {
    return {&clip_, &t5_, &transformer_, &vae_, &initial_, &scheduler_, &decode_input_};
}

// Without an explicit budget the weights fill the VRAM the OS grants this
// process, less what the graphs' own buffers need, and the rest are read from
// system memory. FLUX.1-schnell's weights alone exceed a 16 GB GPU.
std::int64_t ImageEngine::weights_budget_mib(std::int64_t requested) {
    if (requested >= 0) return requested;
    const std::uint64_t available = gpu_.local_memory_budget();
    if (available == 0) return -1;
    std::uint64_t reserve = 1ull << 30;
    std::uint64_t scratch = 0;
    std::uint64_t init_scratch = 0;
    for (Graph* graph : graphs()) {
        const auto& requirements = graph->requirements;
        reserve += requirements.persistent_size;
        for (std::uint32_t i = 0; i < requirements.num_inputs; ++i) reserve += requirements.inputs[i].size_bytes;
        for (std::uint32_t i = 0; i < requirements.num_outputs; ++i) reserve += requirements.outputs[i].size_bytes;
        scratch = std::max(scratch, requirements.inference_temporary_size);
        init_scratch = std::max(init_scratch, requirements.init_temporary_size);
    }
    reserve += scratch + init_scratch;
    const std::uint64_t budget = available > reserve ? available - reserve : 0;
    log("weights VRAM budget " + gib(budget) + " of " + gib(available) +
        " available; --weights-vram-mib overrides it");
    return static_cast<std::int64_t>(budget >> 20);
}

ComPtr<ID3D12Resource> ImageEngine::output_buffer(Graph& graph, int index) {
    const auto& output = graph.output(index);
    if (output.aliased_input >= 0) {
        throw std::runtime_error(graph.label + " output " + std::to_string(index) + " updates an input in place");
    }
    const auto buffer = gpu_.create_buffer(output.size_bytes);
    graph.bind_output(index, buffer);
    return buffer;
}

void ImageEngine::connect(Graph& consumer, std::size_t parameter, Graph& producer, int output) {
    const int input = parameter_input(consumer, parameter);
    const auto& to = consumer.input(input);
    const auto& from = producer.output(output);
    if (to.size_bytes != from.size_bytes ||
        (to.element_type != AMICA_ELEMENT_UNKNOWN && from.element_type != AMICA_ELEMENT_UNKNOWN &&
         to.element_type != from.element_type)) {
        throw std::runtime_error(consumer.label + " arg" + std::to_string(parameter) + " (" +
                                 std::to_string(to.size_bytes) + " bytes) cannot read " + producer.label + " output " +
                                 std::to_string(output) + " (" + std::to_string(from.size_bytes) + " bytes)");
    }
    consumer.bind_input(input, producer.output_buffers[output]);
}

void ImageEngine::allocate() {
    const auto bind_host_input = [&](Graph& graph, std::size_t parameter, HostInput& input, bool integer) {
        const int index = parameter_input(graph, parameter);
        input = runtime_.host_input(graph.input(index), integer);
        graph.bind_input(index, input.buffer);
        return index;
    };

    bind_host_input(clip_, 0, clip_ids_, true);
    bind_host_input(t5_, 0, t5_ids_, true);
    if (t5_ids_.elements() != bundle_.text_length()) {
        throw std::runtime_error("text_encoder_2 takes " + std::to_string(t5_ids_.elements()) +
                                 " tokens, not the bundle's text length " + std::to_string(bundle_.text_length()));
    }
    for (std::uint32_t o = 0; o < clip_.requirements.num_outputs; ++o) output_buffer(clip_, static_cast<int>(o));
    for (std::uint32_t o = 0; o < t5_.requirements.num_outputs; ++o) output_buffer(t5_, static_cast<int>(o));
    if (clip_.requirements.num_outputs < 2) throw std::runtime_error("text_encoder has no pooled output");

    const int noise = bind_host_input(initial_, 0, noise_, false);
    if (initial_.input(noise).element_type != AMICA_ELEMENT_FP32) {
        throw std::runtime_error("latent_initial noise input is not FP32");
    }
    for (int k = 0; k < 2; ++k) {
        latent_[k] = gpu_.create_buffer(initial_.output(0).size_bytes);
        latent_half_[k] = gpu_.create_buffer(initial_.output(1).size_bytes);
    }
    initial_.bind_output(0, latent_[0]);
    initial_.bind_output(1, latent_half_[0]);

    transformer_latent_ = parameter_input(transformer_, 0);
    if (transformer_.input(transformer_latent_).size_bytes != initial_.output(1).size_bytes) {
        throw std::runtime_error("the transformer latent input does not match the packed latent");
    }
    transformer_.bind_input(transformer_latent_, latent_half_[0]);
    connect(transformer_, 1, t5_, 0);
    connect(transformer_, 2, clip_, 1);
    transformer_sigma_ = parameter_input(transformer_, 3);
    if (transformer_.input(transformer_sigma_).size_bytes !=
        element_bytes(transformer_.input(transformer_sigma_).element_type)) {
        throw std::runtime_error("the transformer timestep input is not one floating-point value");
    }
    bind_host_input(transformer_, 4, image_positions_, true);
    bind_host_input(transformer_, 5, text_positions_, true);
    const std::size_t patches = latent_shape_.height / 2 * (latent_shape_.width / 2);
    if (image_positions_.elements() != patches * 3 || text_positions_.elements() != bundle_.text_length() * 3) {
        throw std::runtime_error("the transformer position inputs do not match the latent and text lengths");
    }
    output_buffer(transformer_, 0);

    scheduler_latent_ = parameter_input(scheduler_, 0);
    scheduler_.bind_input(scheduler_latent_, latent_[0]);
    connect(scheduler_, 1, transformer_, 0);
    scheduler_step_ = parameter_input(scheduler_, 2);
    if (scheduler_.output(0).size_bytes != initial_.output(0).size_bytes ||
        scheduler_.output(1).size_bytes != initial_.output(1).size_bytes) {
        throw std::runtime_error("the scheduler step does not return the packed latent");
    }
    scheduler_.bind_output(0, latent_[1]);
    scheduler_.bind_output(1, latent_half_[1]);

    step_table_size_ = 2 * kMaxSteps * kStepStride;
    step_table_ = gpu_.create_buffer(step_table_size_);
    step_table_upload_ = gpu_.create_buffer(step_table_size_, D3D12_HEAP_TYPE_UPLOAD);
    void* mapped = nullptr;
    check_hr(step_table_upload_->Map(0, nullptr, &mapped), "Map");
    step_table_mapped_ = static_cast<std::uint8_t*>(mapped);
    transformer_.bind_input(transformer_sigma_, step_table_);
    scheduler_.bind_input(scheduler_step_, step_table_);

    decode_latent_ = parameter_input(decode_input_, 0);
    decode_input_.bind_input(decode_latent_, latent_[0]);
    output_buffer(decode_input_, 0);
    connect(vae_, 0, decode_input_, 0);
    output_buffer(vae_, 0);
    const auto& image = vae_.output(0);
    const std::size_t pixels = 3 * bundle_.width() * bundle_.height();
    if ((image.element_type != AMICA_ELEMENT_FP32 && image.element_type != AMICA_ELEMENT_FP16) ||
        image.size_bytes != pixels * element_bytes(image.element_type)) {
        throw std::runtime_error("vae_decoder does not return a 3-channel FP32 or FP16 image of the bundle's size");
    }
    readback_ = gpu_.create_buffer(image.size_bytes, D3D12_HEAP_TYPE_READBACK);
    check_hr(readback_->Map(0, nullptr, &mapped), "Map");
    readback_mapped_ = static_cast<const std::uint8_t*>(mapped);

    for (Graph* graph : graphs()) {
        for (std::uint32_t i = 0; i < graph->requirements.num_inputs; ++i) {
            if (!graph->input_buffers[i]) {
                throw std::runtime_error(graph->label + " input " + graph->requirements.inputs[i].name +
                                         " is not connected");
            }
        }
    }

    // FLUX's image position IDs: 0, the patch row and the patch column.
    for (std::size_t row = 0; row < latent_shape_.height / 2; ++row) {
        for (std::size_t column = 0; column < latent_shape_.width / 2; ++column) {
            const std::size_t patch = row * (latent_shape_.width / 2) + column;
            image_positions_.write(patch * 3, 0);
            image_positions_.write(patch * 3 + 1, static_cast<std::int64_t>(row));
            image_positions_.write(patch * 3 + 2, static_cast<std::int64_t>(column));
        }
    }
    text_positions_.fill(0);
    gpu_.begin_list();
    runtime_.upload(image_positions_);
    runtime_.upload(text_positions_);
    gpu_.submit_and_wait();
}

Image ImageEngine::generate(const std::string& prompt, std::size_t steps, std::uint64_t seed) {
    if (steps < 1 || steps > kMaxSteps) {
        throw std::runtime_error("steps must be between 1 and " + std::to_string(kMaxSteps));
    }
    const auto started = std::chrono::steady_clock::now();

    const std::int64_t clip_end = clip_tokenizer_.end_id();
    const auto clip = fit(clip_tokenizer_.encode(prompt), clip_ids_.elements(), clip_end, clip_end);
    for (std::size_t i = 0; i < clip.size(); ++i) clip_ids_.write(i, clip[i]);
    const auto t5 = fit(t5_tokenizer_.encode(prompt), t5_ids_.elements(), t5_tokenizer_.eos_id(),
                        std::max<std::int64_t>(t5_tokenizer_.pad_id(), 0));
    for (std::size_t i = 0; i < t5.size(); ++i) t5_ids_.write(i, t5[i]);

    std::mt19937_64 random(seed);
    std::normal_distribution<float> normal;
    auto* noise = reinterpret_cast<float*>(noise_.mapped);
    for (std::size_t i = 0; i < noise_.size / sizeof(float); ++i) noise[i] = normal(random);

    const auto sigmas = bundle_.sigmas(steps);
    const auto sigma_type = transformer_.input(transformer_sigma_).element_type;
    const auto step_type = scheduler_.input(scheduler_step_).element_type;
    std::memset(step_table_mapped_, 0, static_cast<std::size_t>(step_table_size_));
    for (std::size_t i = 0; i < steps; ++i) {
        write_float(step_table_mapped_ + i * kStepStride, sigma_type, sigmas[i]);
        write_float(step_table_mapped_ + (kMaxSteps + i) * kStepStride, step_type, sigmas[i + 1] - sigmas[i]);
    }

    const std::uint64_t table = step_table_->GetGPUVirtualAddress();
    gpu_.begin_list();
    runtime_.upload(clip_ids_);
    runtime_.upload(t5_ids_);
    runtime_.upload(noise_);
    gpu_.copy_into(step_table_.Get(), 0, step_table_upload_.Get(), 0, step_table_size_);
    gpu_.uav_barrier();
    runtime_.encode(clip_);
    runtime_.encode(t5_);
    runtime_.encode(initial_);
    int current = 0;
    for (std::size_t i = 0; i < steps; ++i) {
        transformer_.inputs[transformer_latent_] = latent_half_[current]->GetGPUVirtualAddress();
        transformer_.inputs[transformer_sigma_] = table + i * kStepStride;
        runtime_.encode(transformer_);
        scheduler_.inputs[scheduler_latent_] = latent_[current]->GetGPUVirtualAddress();
        scheduler_.inputs[scheduler_step_] = table + (kMaxSteps + i) * kStepStride;
        scheduler_.outputs[0] = latent_[1 - current]->GetGPUVirtualAddress();
        scheduler_.outputs[1] = latent_half_[1 - current]->GetGPUVirtualAddress();
        runtime_.encode(scheduler_);
        current = 1 - current;
    }
    decode_input_.inputs[decode_latent_] = latent_[current]->GetGPUVirtualAddress();
    runtime_.encode(decode_input_);
    runtime_.encode(vae_);
    const auto& output = vae_.output(0);
    gpu_.read_back(readback_.Get(), vae_.output_buffers[0].Get(), 0, output.size_bytes);
    gpu_.submit_and_wait();

    Image image;
    image.width = bundle_.width();
    image.height = bundle_.height();
    image.rgb.resize(3 * image.width * image.height);
    const std::size_t plane = image.width * image.height;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        for (std::size_t pixel = 0; pixel < plane; ++pixel) {
            const std::size_t index = channel * plane + pixel;
            float value;
            if (output.element_type == AMICA_ELEMENT_FP32) {
                std::memcpy(&value, readback_mapped_ + index * 4, 4);
            } else {
                std::uint16_t half;
                std::memcpy(&half, readback_mapped_ + index * 2, 2);
                value = half_to_float(half);
            }
            if (!std::isfinite(value)) throw std::runtime_error("the image pipeline produced non-finite pixels");
            const float level = std::nearbyint(std::clamp(value / 2.0f + 0.5f, 0.0f, 1.0f) * 255.0f);
            image.rgb[pixel * 3 + channel] = static_cast<std::uint8_t>(level);
        }
    }
    image.generate_ms = milliseconds_since(started);
    return image;
}

}  // namespace amica_engine
