// amica-engine: keeps an AMICA ChatApp bundle or GenaiApp image bundle
// resident on a D3D12 GPU and serves it through an OpenAI-compatible HTTP API
// (openai_api.cpp for chat, image_api.cpp for images).
//
//   amica-engine --model-dir <bundle> --amica-dir <AMICA install>\amica --port N
//                [--host 127.0.0.1] [--model-name NAME] [--adapter <index|name>]
//                [--arch gfxNNNN] [--weights-vram-mib N]
//   amica-engine --model-dir <image bundle> --print-tokens TEXT

#include "amica_runtime.h"

#include <shellapi.h>

#include <nlohmann/json.hpp>

#include "bundle.h"
#include "chat_frontend.h"
#include "image_api.h"
#include "image_bundle.h"
#include "image_engine.h"
#include "onnx_tokenizers.h"
#include "openai_api.h"
#include "png.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace amica_engine;
using json = nlohmann::json;

// The token embedding table, read on the CPU one FP16 row at a time.
class EmbeddingTable {
public:
    EmbeddingTable(const std::string& path, const std::string& tensor_name)
        : file_(path), tensors_(file_), tensor_(tensors_.at(tensor_name)) {
        if (tensor_.dtype != "F16" || tensor_.shape.size() != 2 ||
            tensor_.size != tensor_.shape[0] * tensor_.shape[1] * sizeof(std::uint16_t)) {
            throw std::runtime_error("embedding tensor " + tensor_name + " must be a rank-2 F16 matrix");
        }
    }

    std::size_t rows() const { return static_cast<std::size_t>(tensor_.shape[0]); }
    std::size_t columns() const { return static_cast<std::size_t>(tensor_.shape[1]); }

    void write_f32_row(std::size_t token, float* destination) const {
        if (token >= rows()) {
            throw std::runtime_error("token " + std::to_string(token) + " is outside the embedding table");
        }
        const auto* row = tensor_.data + token * columns() * sizeof(std::uint16_t);
        for (std::size_t column = 0; column < columns(); ++column) {
            std::uint16_t value;
            std::memcpy(&value, row + column * sizeof(value), sizeof(value));
            destination[column] = half_to_float(value);
        }
    }

private:
    MappedFile file_;
    SafeTensors tensors_;
    const Tensor& tensor_;
};

// The per-request inputs of a prefill or decoder graph: an embedding row per
// token, or token IDs with an attention mask, plus optional position inputs.
struct PromptInputs {
    HostInput embedding;
    HostInput token;
    HostInput mask;
    HostInput position;
    HostInput sequence;
};

struct PrefillVariant {
    Graph graph;
    std::size_t capacity = 0;
    PromptInputs inputs;
};

struct InputNames {
    std::string embedding;
    std::string token;
    std::string mask;
    std::string position;
    std::string sequence;
};

class Engine : public amica_engine::Generator {
public:
    Engine(const json& plan, const std::string& adapter, const std::string& arch, std::int64_t weights_vram_mib)
        : runtime_(adapter, arch), gpu_(runtime_.gpu()) {
        const auto started = std::chrono::steady_clock::now();

        graph_logits_ = plan.at("logits").get<std::string>() == "graph";
        prompt_right_aligned_ = plan.at("prompt_padding").get<std::string>() == "left";
        chunked_prefill_ = plan.value("chunked_prefill", false);
        external_weights_min_bytes_ = plan.value("external_weights_min_bytes", std::uint64_t{0});
        const auto& names = plan.at("inputs");
        const auto name = [&](const char* key) { return names.value(key, json()).is_string()
                                                            ? names.at(key).get<std::string>()
                                                            : std::string(); };
        prefill_names_ = {name("prefill_embedding"), name("prefill_token"), name("prefill_attention_mask"),
                          name("prefill_position"), name("prefill_sequence")};
        decoder_names_ = {name("decoder_embedding"), name("decoder_token"), name("decoder_attention_mask"),
                          name("decoder_position"), name("decoder_sequence")};
        for (const auto& input : plan.value("zero_initialize", std::vector<std::string>())) {
            zero_initialize_.insert(input);
        }
        const json fills = plan.value("initial_input_fill", json::object());
        for (const auto& [input, value] : fills.items()) {
            initial_fill_[input] = value.get<std::int64_t>();
        }

        if (graph_logits_) {
            vocabulary_ = plan.at("vocab_size").get<std::size_t>();
            pad_token_id_ = plan.value("pad_token_id", std::int64_t{0});
        } else {
            embeddings_ = std::make_unique<EmbeddingTable>(plan.at("embedding").at("file").get<std::string>(),
                                                           plan.at("embedding").at("tensor").get<std::string>());
            vocabulary_ = embeddings_->rows();
            tail_row_offset_ = plan.value("tail_row_offset", 0);
        }

        Resources& resources = runtime_.open_resources(plan.at("resources").get<std::string>());
        decoder_ = compile("decoder", plan.at("decoder").get<std::string>(), resources);
        for (const auto& entry : plan.at("prefill")) {
            auto variant = std::make_unique<PrefillVariant>();
            variant->capacity = entry.at("capacity").get<std::size_t>();
            variant->graph = compile("prefill[" + std::to_string(variant->capacity) + "]",
                                     entry.at("graph").get<std::string>(), resources);
            for (const auto& [output, input] : entry.at("cache_map").items()) {
                cache_maps_[variant.get()].emplace_back(output, input.get<std::string>());
            }
            prefill_.push_back(std::move(variant));
        }
        std::sort(prefill_.begin(), prefill_.end(),
                  [](const auto& a, const auto& b) { return a->capacity < b->capacity; });
        if (!graph_logits_) {
            const auto& lm_head = plan.at("lm_head");
            lm_head_ = compile("lm_head", lm_head.at("graph").get<std::string>(),
                               runtime_.open_resources(lm_head.at("resources").get<std::string>()));
            tail_input_name_ = plan.at("tail_input").get<std::string>();
        }

        runtime_.upload_weights(graphs(), weights_vram_mib);
        allocate_decoder();
        for (auto& variant : prefill_) {
            allocate_prefill(*variant);
        }
        if (!graph_logits_) {
            allocate_lm_head(plan.at("lm_head").value("resource_map", json::object()));
        }
        allocate_logits_readback();
        allocate_zero_source();
        runtime_.initialize_graphs(graphs());

        load_ms_ = milliseconds_since(started);
    }

    std::string summary() const {
        std::string capacities;
        for (const auto& variant : prefill_) {
            capacities += (capacities.empty() ? "" : ",") + std::to_string(variant->capacity);
        }
        return "adapter='" + gpu_.adapter_name() + "' arch=" + gpu_.gfx_arch() + " prefill_capacity=[" + capacities +
               "]" + (chunked_prefill_ ? " chunked_prefill" : "") +
               " load_ms=" + std::to_string(static_cast<long long>(load_ms_));
    }

    amica_engine::Generation generate(const std::vector<std::int64_t>& prompt_ids, std::size_t max_new_tokens,
                                      const std::set<std::int64_t>& stop_ids,
                                      const std::function<bool(std::int64_t)>& on_token) override {
        if (prompt_ids.empty()) {
            throw std::runtime_error("the prompt is empty");
        }
        if (max_new_tokens == 0) {
            throw std::runtime_error("max_new_tokens must be at least 1");
        }
        std::vector<std::size_t> prompt;
        for (const std::int64_t id : prompt_ids) {
            if (id < 0 || static_cast<std::size_t>(id) >= vocabulary_) {
                throw std::runtime_error("prompt token " + std::to_string(id) + " is outside the vocabulary");
            }
            prompt.push_back(static_cast<std::size_t>(id));
        }
        PrefillVariant* variant = nullptr;
        for (auto& candidate : prefill_) {
            if (candidate->capacity >= prompt.size()) {
                variant = candidate.get();
                break;
            }
        }
        if (variant == nullptr && !chunked_prefill_) {
            throw std::runtime_error("the prompt has " + std::to_string(prompt.size()) +
                                     " tokens, more than the largest prefill capacity " +
                                     std::to_string(prefill_.back()->capacity));
        }

        amica_engine::Generation result;
        const auto started = std::chrono::steady_clock::now();
        std::size_t token = variant ? prefill(*variant, prompt) : prefill_chunked(prompt);
        result.ttft_ms = milliseconds_since(started);
        result.ids.push_back(static_cast<std::int64_t>(token));
        bool keep_going = on_token(static_cast<std::int64_t>(token));

        const auto decode_started = std::chrono::steady_clock::now();
        while (keep_going && result.ids.size() < max_new_tokens &&
               stop_ids.count(static_cast<std::int64_t>(token)) == 0) {
            const auto position = static_cast<std::int64_t>(prompt.size() + result.ids.size() - 1);
            token = decode(token, position);
            result.ids.push_back(static_cast<std::int64_t>(token));
            keep_going = on_token(static_cast<std::int64_t>(token));
        }
        result.decode_ms = result.ids.size() > 1 ? milliseconds_since(decode_started) : 0.0;
        result.tokens_per_second =
            result.decode_ms > 0.0 ? (result.ids.size() - 1) * 1000.0 / result.decode_ms : 0.0;
        return result;
    }

    bool device_removed() override { return runtime_.device_removed(); }

private:
    Graph compile(const std::string& label, const std::string& path, Resources& resources) {
        // Logged first: if amica.dll terminates the process while compiling,
        // this names the graph it was compiling.
        log("compiling " + label + " from " + path);
        MappedFile ir(path);
        amica::compile_options options;
        options.prompt_right_aligned = prompt_right_aligned_ ? 1 : 0;
        options.external_weights_min_bytes = external_weights_min_bytes_;
        return runtime_.compile(label, ir.text(), &resources, options);
    }

    std::vector<Graph*> graphs() {
        std::vector<Graph*> result{&decoder_};
        for (auto& variant : prefill_) result.push_back(&variant->graph);
        if (!graph_logits_) result.push_back(&lm_head_);
        return result;
    }

    HostInput host_input(const amica_io_desc& io, bool integer) { return runtime_.host_input(io, integer); }

    void bind_input(Graph& graph, int index, const ComPtr<ID3D12Resource>& buffer) {
        graph.bind_input(index, buffer);
    }

    void bind_output(Graph& graph, int index, const ComPtr<ID3D12Resource>& buffer) {
        graph.bind_output(index, buffer);
    }

    // Binds the inputs a request writes; returns their indices.
    std::set<int> bind_prompt_inputs(Graph& graph, const InputNames& names, PromptInputs& inputs) {
        std::set<int> bound;
        const auto bind = [&](const std::string& name, HostInput& input, bool integer, bool required) {
            const int index = graph.input_index(name);
            if (index < 0) {
                if (required) {
                    throw std::runtime_error(graph.label + " input " + name + " was not found");
                }
                return;
            }
            input = host_input(graph.input(index), integer);
            bind_input(graph, index, input.buffer);
            bound.insert(index);
        };
        if (graph_logits_) {
            bind(names.token, inputs.token, true, true);
            bind(names.mask, inputs.mask, true, true);
        } else {
            bind(names.embedding, inputs.embedding, false, true);
        }
        bind(names.position, inputs.position, true, false);
        bind(names.sequence, inputs.sequence, true, false);
        return bound;
    }

    // A buffer shared by prefill and decoder is reset once, under prefill's
    // name, because the manifest's initial_input_fill lists prefill names.
    void add_reset(const ComPtr<ID3D12Resource>& buffer, const amica_io_desc& io) {
        Reset reset{buffer, nullptr, io.size_bytes};
        const auto fill = initial_fill_.find(io.name);
        if (fill != initial_fill_.end()) {
            const std::size_t width = element_bytes(io.element_type);
            if ((io.element_type != AMICA_ELEMENT_INT32 && io.element_type != AMICA_ELEMENT_INT64) ||
                io.size_bytes % width != 0) {
                throw std::runtime_error(std::string("initial_input_fill input ") + io.name +
                                         " is not int32/int64 storage");
            }
            reset.source = gpu_.create_buffer(io.size_bytes, D3D12_HEAP_TYPE_UPLOAD);
            void* mapped = nullptr;
            check_hr(reset.source->Map(0, nullptr, &mapped), "Map");
            HostInput writer;
            writer.mapped = static_cast<std::uint8_t*>(mapped);
            writer.size = io.size_bytes;
            writer.width = width;
            writer.fill(fill->second);
            reset.source->Unmap(0, nullptr);
        } else {
            zero_size_ = std::max(zero_size_, io.size_bytes);
        }
        resets_[buffer.Get()] = reset;
    }

    void bind_outputs(Graph& graph) {
        for (std::uint32_t o = 0; o < graph.requirements.num_outputs; ++o) {
            if (graph.output_buffers[o]) continue;
            const auto& output = graph.output(static_cast<int>(o));
            bind_output(graph, static_cast<int>(o),
                        output.aliased_input >= 0 ? graph.input_buffers[output.aliased_input]
                                                  : gpu_.create_buffer(output.size_bytes));
        }
    }

    void allocate_decoder() {
        Graph& graph = decoder_;
        bind_prompt_inputs(graph, decoder_names_, decoder_inputs_);
        if (!graph_logits_ && decoder_inputs_.embedding.size != embeddings_->columns() * sizeof(float)) {
            throw std::runtime_error("decoder embedding input does not hold one FP32 embedding row");
        }
        if (graph_logits_ && decoder_inputs_.token.elements() != 1) {
            throw std::runtime_error("decoder token input must hold one token");
        }
        // State inputs; see allocate_prefill for how prefill hands them over.
        for (std::uint32_t i = 0; i < graph.requirements.num_inputs; ++i) {
            if (graph.input_buffers[i]) continue;
            const auto& io = graph.input(static_cast<int>(i));
            const auto buffer = gpu_.create_buffer(io.size_bytes);
            bind_input(graph, static_cast<int>(i), buffer);
            add_reset(buffer, io);
        }
        bind_outputs(graph);
    }

    void allocate_prefill(PrefillVariant& variant) {
        Graph& graph = variant.graph;
        const auto prompt_inputs = bind_prompt_inputs(graph, prefill_names_, variant.inputs);
        const std::size_t capacity = graph_logits_ ? variant.inputs.token.elements()
                                                   : static_cast<std::size_t>(variant.inputs.embedding.size /
                                                                              (embeddings_->columns() * sizeof(float)));
        if (capacity != variant.capacity) {
            throw std::runtime_error(graph.label + " holds " + std::to_string(capacity) +
                                     " prompt tokens, not the planned " + std::to_string(variant.capacity));
        }

        // The cache map names the decoder input each prefill output feeds. An
        // output updated in place shares its buffer with the input it aliases,
        // so that prefill input becomes the decoder's state buffer too.
        for (const auto& [output_name, decoder_input] : cache_maps_[&variant]) {
            int output = -1;
            for (std::uint32_t o = 0; o < graph.requirements.num_outputs; ++o) {
                if (output_name == graph.output(static_cast<int>(o)).name) output = static_cast<int>(o);
            }
            const int target = decoder_.input_index(decoder_input);
            if (output < 0 || target < 0) {
                throw std::runtime_error(graph.label + " cache map " + output_name + "=" + decoder_input +
                                         " names a missing output or decoder input");
            }
            if (graph.output(output).size_bytes != decoder_.input(target).size_bytes) {
                throw std::runtime_error(graph.label + " cache map " + output_name + "=" + decoder_input +
                                         " joins buffers of different sizes");
            }
            const auto& buffer = decoder_.input_buffers[target];
            bind_output(graph, output, buffer);
            const int aliased = graph.output(output).aliased_input;
            if (aliased >= 0) {
                bind_input(graph, aliased, buffer);
            }
        }
        for (std::uint32_t i = 0; i < graph.requirements.num_inputs; ++i) {
            const int index = static_cast<int>(i);
            if (prompt_inputs.count(index) != 0) continue;
            const auto& io = graph.input(index);
            if (!graph.input_buffers[i]) {
                if (zero_initialize_.count(io.name) == 0 && initial_fill_.count(io.name) == 0) {
                    log(std::string("warning: ") + graph.label + " input " + io.name +
                        " has no plan entry; it starts zeroed");
                }
                bind_input(graph, index, gpu_.create_buffer(io.size_bytes));
            }
            add_reset(graph.input_buffers[i], io);
        }
        bind_outputs(graph);
    }

    void allocate_lm_head(const json& resource_map) {
        Graph& graph = lm_head_;
        tail_input_index_ = graph.input_index(tail_input_name_);
        if (tail_input_index_ < 0) {
            throw std::runtime_error("LM head input " + tail_input_name_ + " was not found");
        }
        if (graph.input(tail_input_index_).size_bytes != embeddings_->columns() * sizeof(float)) {
            throw std::runtime_error("LM head input " + tail_input_name_ + " does not hold one FP32 hidden row");
        }
        lm_input_ = gpu_.create_buffer(graph.input(tail_input_index_).size_bytes);
        bind_input(graph, tail_input_index_, lm_input_);

        std::vector<ComPtr<ID3D12Resource>> staging;
        gpu_.begin_list();
        for (const auto& [input_name, tensor_name] : resource_map.items()) {
            const int index = graph.input_index(input_name);
            if (index < 0) {
                throw std::runtime_error("LM head resource map names missing input " + input_name);
            }
            const auto& tensor = graph.resources->tensors.at(tensor_name.get<std::string>());
            if (tensor.size != graph.input(index).size_bytes) {
                throw std::runtime_error("LM head input " + input_name + " takes " +
                                         std::to_string(graph.input(index).size_bytes) + " bytes; tensor " +
                                         tensor_name.get<std::string>() + " has " + std::to_string(tensor.size));
            }
            const auto buffer = gpu_.create_buffer(tensor.size);
            const auto upload = gpu_.create_buffer(tensor.size, D3D12_HEAP_TYPE_UPLOAD);
            void* mapped = nullptr;
            check_hr(upload->Map(0, nullptr, &mapped), "Map");
            std::memcpy(mapped, tensor.data, tensor.size);
            upload->Unmap(0, nullptr);
            gpu_.copy_into(buffer.Get(), 0, upload.Get(), 0, tensor.size);
            staging.push_back(upload);
            bind_input(graph, index, buffer);
        }
        gpu_.submit_and_wait();
        for (std::uint32_t i = 0; i < graph.requirements.num_inputs; ++i) {
            if (!graph.input_buffers[i]) {
                throw std::runtime_error(std::string("LM head input ") + graph.input(static_cast<int>(i)).name +
                                         " is neither the hidden-state input nor in the resource map");
            }
        }
        bind_outputs(graph);
    }

    // The graphs whose output 0 holds logits: the LM head, or prefill and the
    // decoder themselves. Each produces one or more rows of vocabulary logits.
    void allocate_logits_readback() {
        std::vector<Graph*> producers;
        if (graph_logits_) {
            producers.push_back(&decoder_);
            for (auto& variant : prefill_) producers.push_back(&variant->graph);
        } else {
            producers.push_back(&lm_head_);
        }
        std::uint64_t row_bytes = 0;
        for (Graph* graph : producers) {
            const auto& logits = graph->output(0);
            const std::size_t width = element_bytes(logits.element_type);
            if (logits.element_type != AMICA_ELEMENT_FP16 && logits.element_type != AMICA_ELEMENT_FP32) {
                throw std::runtime_error(graph->label + " logits must be FP16 or FP32");
            }
            if (logits.size_bytes == 0 || logits.size_bytes % (vocabulary_ * width) != 0) {
                throw std::runtime_error(graph->label + " output 0 holds " + std::to_string(logits.size_bytes) +
                                         " bytes, not rows of " + std::to_string(vocabulary_) + " logits");
            }
            row_bytes = std::max<std::uint64_t>(row_bytes, vocabulary_ * width);
        }
        logits_readback_ = gpu_.create_buffer(row_bytes, D3D12_HEAP_TYPE_READBACK);
        void* mapped = nullptr;
        check_hr(logits_readback_->Map(0, nullptr, &mapped), "Map");
        logits_mapped_ = static_cast<const std::uint8_t*>(mapped);
    }

    // The zero source resets state per request.
    void allocate_zero_source() {
        if (zero_size_ != 0) {
            zero_source_ = gpu_.create_buffer(zero_size_, D3D12_HEAP_TYPE_UPLOAD);
            void* mapped = nullptr;
            check_hr(zero_source_->Map(0, nullptr, &mapped), "Map");
            std::memset(mapped, 0, static_cast<std::size_t>(zero_size_));
            zero_source_->Unmap(0, nullptr);
        }
    }

    void encode(Graph& graph) { runtime_.encode(graph); }

    void upload(HostInput& input) { runtime_.upload(input); }

    void upload(PromptInputs& inputs) {
        upload(inputs.embedding);
        upload(inputs.token);
        upload(inputs.mask);
        upload(inputs.position);
        upload(inputs.sequence);
    }

    // Reads one row of logits from output 0 of graph and returns its argmax.
    std::size_t argmax_logits(Graph& graph, std::uint64_t row) {
        const auto& logits = graph.output(0);
        const std::size_t width = element_bytes(logits.element_type);
        const std::uint64_t row_bytes = vocabulary_ * width;
        if (row >= logits.size_bytes / row_bytes) {
            throw std::runtime_error("the logits row is outside " + graph.label + " output 0");
        }
        gpu_.read_back(logits_readback_.Get(), graph.output_buffers[0].Get(), row * row_bytes, row_bytes);
        gpu_.submit_and_wait();

        float best = -std::numeric_limits<float>::infinity();
        std::size_t best_token = 0;
        for (std::size_t i = 0; i < vocabulary_; ++i) {
            float value;
            if (width == 4) {
                std::memcpy(&value, logits_mapped_ + i * 4, 4);
            } else {
                std::uint16_t half;
                std::memcpy(&half, logits_mapped_ + i * 2, 2);
                value = half_to_float(half);
            }
            if (std::isfinite(value) && value > best) {
                best = value;
                best_token = i;
            }
        }
        if (!std::isfinite(best)) {
            throw std::runtime_error("the logits contain no finite value");
        }
        return best_token;
    }

    std::size_t run_lm_head(std::uint64_t hidden_va) {
        lm_head_.inputs[tail_input_index_] = hidden_va;
        encode(lm_head_);
        return argmax_logits(lm_head_, 0);
    }

    // The output row of the last prompt token in a prefill output with `rows`
    // rows: the final row when prompts are right-aligned (left-padded), else
    // row n-1.
    std::uint64_t last_prompt_row(std::uint64_t rows, std::size_t prompt_tokens, int offset) const {
        const std::int64_t row =
            static_cast<std::int64_t>(prompt_right_aligned_ ? rows - 1 : prompt_tokens - 1) + offset;
        if (row < 0 || static_cast<std::uint64_t>(row) >= rows) {
            throw std::runtime_error("the prefill row of the last prompt token is outside the prefill output");
        }
        return static_cast<std::uint64_t>(row);
    }

    std::size_t prefill(PrefillVariant& variant, const std::vector<std::size_t>& prompt) {
        return prefill_chunk(variant, prompt.data(), prompt.size(), 0, true);
    }

    // A prompt longer than every prefill graph goes in as one partial chunk
    // and then full chunks of the largest graph: a chunked graph places its
    // rows after the ones already cached, which it can tell only when every
    // chunk after the first is full.
    std::size_t prefill_chunked(const std::vector<std::size_t>& prompt) {
        PrefillVariant& full = *prefill_.back();
        const std::size_t partial = prompt.size() % full.capacity;
        const std::size_t first = partial == 0 ? full.capacity : partial;
        PrefillVariant* first_variant = &full;
        for (auto& candidate : prefill_) {
            if (candidate->capacity >= first) {
                first_variant = candidate.get();
                break;
            }
        }
        std::size_t done = 0;
        std::size_t token = 0;
        while (done < prompt.size()) {
            const std::size_t n = done == 0 ? first : full.capacity;
            PrefillVariant& variant = done == 0 ? *first_variant : full;
            const bool last = done + n == prompt.size();
            token = prefill_chunk(variant, prompt.data() + done, n, done, last);
            done += n;
        }
        return token;
    }

    // Feeds `n` prompt tokens after `past` cached ones. The first chunk resets
    // the state; only the last reads back its next token.
    std::size_t prefill_chunk(PrefillVariant& variant, const std::size_t* prompt, std::size_t n,
                              std::size_t past, bool last) {
        PromptInputs& inputs = variant.inputs;
        if (graph_logits_) {
            const std::size_t first = prompt_right_aligned_ ? variant.capacity - n : 0;
            inputs.token.fill(pad_token_id_);
            for (std::size_t i = 0; i < n; ++i) {
                inputs.token.write(first + i, static_cast<std::int64_t>(prompt[i]));
            }
            const std::size_t mask_capacity = inputs.mask.elements();
            const std::size_t visible = past + n;
            if (visible > mask_capacity) {
                throw std::runtime_error("the prompt exceeds the prefill attention-mask capacity");
            }
            const std::size_t mask_first = prompt_right_aligned_ ? mask_capacity - visible : 0;
            inputs.mask.fill(0);
            for (std::size_t i = 0; i < visible; ++i) {
                inputs.mask.write(mask_first + i, 1);
            }
        } else {
            const std::size_t row_bytes = embeddings_->columns() * sizeof(float);
            std::memset(inputs.embedding.mapped, 0, static_cast<std::size_t>(inputs.embedding.size));
            const std::size_t first = prompt_right_aligned_ ? variant.capacity - n : 0;
            for (std::size_t i = 0; i < n; ++i) {
                embeddings_->write_f32_row(prompt[i],
                                           reinterpret_cast<float*>(inputs.embedding.mapped + (first + i) * row_bytes));
            }
        }
        if (inputs.position) inputs.position.write(0, static_cast<std::int64_t>(n) - 1);
        if (inputs.sequence) inputs.sequence.write(0, static_cast<std::int64_t>(n));

        gpu_.begin_list();
        if (past == 0) {
            for (const auto& [resource, reset] : resets_) {
                gpu_.copy_into(reset.buffer.Get(), 0, reset.source ? reset.source.Get() : zero_source_.Get(), 0,
                               reset.size);
            }
        }
        upload(inputs);
        gpu_.uav_barrier();
        encode(variant.graph);
        if (!last) {
            gpu_.submit_and_wait();
            return 0;
        }

        if (graph_logits_) {
            const std::uint64_t rows = variant.graph.output(0).size_bytes /
                                       (vocabulary_ * element_bytes(variant.graph.output(0).element_type));
            return argmax_logits(variant.graph, rows == 1 ? 0 : last_prompt_row(rows, n, 0));
        }
        const std::uint64_t row_bytes = embeddings_->columns() * sizeof(float);
        const std::uint64_t row = last_prompt_row(variant.graph.output(0).size_bytes / row_bytes, n, tail_row_offset_);
        gpu_.copy_between(lm_input_.Get(), 0, variant.graph.output_buffers[0].Get(), row * row_bytes, row_bytes);
        gpu_.uav_barrier();
        return run_lm_head(lm_input_->GetGPUVirtualAddress());
    }

    // Feeds `token` at `position` (zero-based; the prompt fills 0..n-1).
    std::size_t decode(std::size_t token, std::int64_t position) {
        PromptInputs& inputs = decoder_inputs_;
        if (graph_logits_) {
            inputs.token.write(0, static_cast<std::int64_t>(token));
            const auto valid = static_cast<std::size_t>(position + 1);
            if (valid > inputs.mask.elements()) {
                throw std::runtime_error("the sequence exceeds the decoder attention-mask capacity");
            }
            inputs.mask.fill(0);
            for (std::size_t i = 0; i < valid; ++i) {
                inputs.mask.write(i, 1);
            }
        } else {
            embeddings_->write_f32_row(token, reinterpret_cast<float*>(inputs.embedding.mapped));
        }
        if (inputs.position) inputs.position.write(0, position);
        if (inputs.sequence) inputs.sequence.write(0, position + 1);

        gpu_.begin_list();
        upload(inputs);
        gpu_.uav_barrier();
        encode(decoder_);

        if (graph_logits_) {
            const std::uint64_t rows =
                decoder_.output(0).size_bytes / (vocabulary_ * element_bytes(decoder_.output(0).element_type));
            return argmax_logits(decoder_, rows - 1);
        }
        const std::uint64_t row_bytes = embeddings_->columns() * sizeof(float);
        const std::uint64_t rows = decoder_.output(0).size_bytes / row_bytes;
        return run_lm_head(decoder_.output_buffers[0]->GetGPUVirtualAddress() + (rows - 1) * row_bytes);
    }

    struct Reset {
        ComPtr<ID3D12Resource> buffer;
        ComPtr<ID3D12Resource> source;  // null: copy from the shared zero source
        std::uint64_t size = 0;
    };

    Runtime runtime_;
    Gpu& gpu_;
    std::unique_ptr<EmbeddingTable> embeddings_;

    Graph decoder_;
    std::vector<std::unique_ptr<PrefillVariant>> prefill_;
    std::map<const PrefillVariant*, std::vector<std::pair<std::string, std::string>>> cache_maps_;
    Graph lm_head_;

    bool graph_logits_ = false;
    bool prompt_right_aligned_ = true;
    bool chunked_prefill_ = false;
    std::uint64_t external_weights_min_bytes_ = 0;
    std::size_t vocabulary_ = 0;
    std::int64_t pad_token_id_ = 0;
    int tail_row_offset_ = 0;
    InputNames prefill_names_;
    InputNames decoder_names_;
    std::string tail_input_name_;
    std::set<std::string> zero_initialize_;
    std::map<std::string, std::int64_t> initial_fill_;

    PromptInputs decoder_inputs_;
    int tail_input_index_ = -1;
    ComPtr<ID3D12Resource> lm_input_;
    ComPtr<ID3D12Resource> logits_readback_;
    const std::uint8_t* logits_mapped_ = nullptr;

    std::map<ID3D12Resource*, Reset> resets_;
    std::uint64_t zero_size_ = 0;
    ComPtr<ID3D12Resource> zero_source_;
    double load_ms_ = 0.0;
};

int usage() {
    std::cerr << "usage: amica-engine --model-dir <bundle> --amica-dir <AMICA install>\\amica --port N "
                 "[--host 127.0.0.1] [--model-name NAME] [--adapter <index|name>] [--arch gfxNNNN] "
                 "[--weights-vram-mib N] [--transformer-precision source|int4-weight-only]\n"
                 "       amica-engine --model-dir <image bundle> --print-tokens TEXT\n"
                 "       amica-engine --model-dir <image bundle> --amica-dir <AMICA install>\\amica "
                 "--generate PROMPT [--output FILE.png]\n";
    return 2;
}

// The settings GenaiApp runs FLUX with. amica.dll reads them when it compiles,
// so they are set before the first compile, unless the user set them.
void set_image_compile_environment() {
    const std::pair<const char*, const char*> settings[] = {
        {"AMDXCGC_ONNX_FP16_SEMANTICS", "1"},
        {"AMDXCGC_QGEMM_F32_ACTIVATION_PRECISION", "fp32"},
        {"AMDXCGC_RMSNORM_REDUCTION_PRECISION", "fp32"},
    };
    for (const auto& [name, value] : settings) {
        if (std::getenv(name) != nullptr) continue;
        _putenv_s(name, value);
        SetEnvironmentVariableA(name, value);
    }
}

// The raw token IDs of both text encoders' tokenizers, for comparison with
// the ONNX tokenizers GenaiApp runs.
int print_tokens(const std::string& model_dir, const std::string& text) {
    const ImageBundle bundle(model_dir);
    const ClipTokenizer clip(bundle.clip_tokenizer());
    const SentencePieceTokenizer t5(bundle.t5_tokenizer());
    std::cout << json{{"clip", clip.encode(text)}, {"t5", t5.encode(text)}}.dump() << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string model_dir;
    std::string amica_dir;
    std::string host = "127.0.0.1";
    std::string model_name = "amica";
    int port = 0;
    std::string adapter;
    std::string arch;
    std::int64_t weights_vram_mib = -1;
    std::optional<std::string> print_text;
    std::optional<std::string> generate_prompt;
    std::string output_path = "amica-image.png";
    std::string transformer_precision = "source";
    // argv is in the ANSI code page; the wide command line keeps every
    // character of model paths and prompts.
    std::vector<std::string> arguments;
    if (LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 0; i < argc; ++i) arguments.push_back(narrow(wide[i]));
        LocalFree(wide);
    } else {
        arguments.assign(argv, argv + argc);
    }
    for (int i = 1; i < argc; ++i) {
        const std::string option = arguments[i];
        if (i + 1 >= argc) return usage();
        const std::string value = arguments[++i];
        if (option == "--model-dir") {
            model_dir = value;
        } else if (option == "--amica-dir") {
            amica_dir = value;
        } else if (option == "--host") {
            host = value;
        } else if (option == "--model-name") {
            model_name = value;
        } else if (option == "--port") {
            try {
                port = std::stoi(value);
            } catch (const std::exception&) {
                return usage();
            }
            if (port <= 0 || port > 65535) return usage();
        } else if (option == "--adapter") {
            adapter = value;
        } else if (option == "--arch") {
            arch = value;
        } else if (option == "--weights-vram-mib") {
            try {
                weights_vram_mib = std::stoll(value);
            } catch (const std::exception&) {
                return usage();
            }
            if (weights_vram_mib < 0) return usage();
        } else if (option == "--print-tokens") {
            print_text = value;
        } else if (option == "--generate") {
            generate_prompt = value;
        } else if (option == "--output") {
            output_path = value;
        } else if (option == "--transformer-precision") {
            transformer_precision = value;
        } else {
            std::cerr << "unknown option " << option << "\n";
            return usage();
        }
    }
    if (print_text && !model_dir.empty()) {
        try {
            return print_tokens(model_dir, *print_text);
        } catch (const std::exception& error) {
            log(std::string("cannot tokenize: ") + error.what());
            return 1;
        }
    }
    if (model_dir.empty() || amica_dir.empty() || (port == 0 && !generate_prompt)) return usage();

    try {
        // amica.dll is delay-loaded; it and the DLLs it depends on load from
        // the package's amica folder.
        if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS) ||
            AddDllDirectory(widen(amica_dir).c_str()) == nullptr) {
            throw std::runtime_error("cannot add " + amica_dir + " to the DLL search path");
        }
        if (GetFileAttributesW(widen(amica_dir + "\\amica.dll").c_str()) == INVALID_FILE_ATTRIBUTES) {
            throw std::runtime_error("amica.dll was not found in " + amica_dir);
        }
        // The bundle and its tokenizers load first: they fail in seconds, while
        // the GPU load can take minutes.
        if (ImageBundle::is_image_bundle(model_dir)) {
            const ImageBundle bundle(model_dir, transformer_precision);
            set_image_compile_environment();
            ImageEngine engine(bundle, adapter, arch, weights_vram_mib);
            log("model=" + model_dir + " " + engine.summary() + " source=" + bundle.source() +
                " transformer=" + transformer_precision);
            if (generate_prompt) {
                const Image image = engine.generate(*generate_prompt, engine.default_steps(), 42);
                std::ofstream(std::filesystem::u8path(output_path), std::ios::binary)
                    << encode_png(image.width, image.height, image.rgb);
                log("wrote " + output_path + " in " + std::to_string(static_cast<long long>(image.generate_ms)) +
                    " ms");
                return 0;
            }
            return amica_engine::serve_images(host, port, model_name, engine);
        }
        const amica_engine::Bundle bundle(model_dir);
        const amica_engine::ChatFrontend frontend(bundle.source_dir());
        Engine engine(bundle.plan(), adapter, arch, weights_vram_mib);
        const auto context = bundle.context_capacity();
        log("model=" + model_dir + " " + engine.summary() +
            " context=" + (context ? std::to_string(*context) : std::string("unknown")) +
            " padding=" + bundle.prompt_padding() +
            " manifest=" + (bundle.manifest_derived() ? "derived" : "amica_app.json"));
        return amica_engine::serve(host, port, model_name, bundle, frontend, engine);
    } catch (const std::exception& error) {
        log(std::string("failed to load the model: ") + error.what());
        return 1;
    }
}
