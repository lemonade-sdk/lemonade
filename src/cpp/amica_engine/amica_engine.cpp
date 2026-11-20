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
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
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
    Gpu::Sequence step;  // uploads the inputs and runs the graph

    // With MTP, the head's prefill over the same rows, its token and mask
    // inputs, and its step: the uploads, the graph and its logits readback.
    Graph head;
    HostInput head_token;
    HostInput head_mask;
    Gpu::Sequence head_step;
};

// One prefill graph run: `n` prompt tokens from position `begin`.
struct Chunk {
    PrefillVariant* variant;
    std::size_t begin;
    std::size_t n;
};

// AMICA names a graph's entry parameters argN by declaration position, which
// can differ from the order of its inputs; -1 for any other name.
int entry_position(const char* name) {
    const std::string text = name != nullptr ? name : "";
    if (text.size() <= 3 || text.rfind("arg", 0) != 0 ||
        !std::all_of(text.begin() + 3, text.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return -1;
    }
    return std::stoi(text.substr(3));
}

struct InputNames {
    std::string embedding;
    std::string token;
    std::string mask;
    std::string position;
    std::string sequence;
};

class Engine : public amica_engine::Generator {
public:
    // With `speculative` and an "mtp" section in the plan, the engine decodes
    // with MTP drafts: the verify graph takes the decoder's place, and the MTP
    // prefill graphs the plain ones'.
    Engine(const json& plan, const std::string& adapter, const std::string& arch, std::int64_t weights_vram_mib,
           bool speculative)
        : runtime_(adapter, arch), gpu_(runtime_.gpu()) {
        const auto started = std::chrono::steady_clock::now();

        graph_logits_ = plan.at("logits").get<std::string>() == "graph";
        mtp_ = speculative && graph_logits_ && plan.contains("mtp");
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
        const json* mtp = mtp_ ? &plan.at("mtp") : nullptr;
        Resources* head_resources = mtp ? &runtime_.open_resources(mtp->at("resources").get<std::string>()) : nullptr;
        const json& decoder = mtp ? mtp->at("verify") : plan.at("decoder");
        decoder_ = compile(mtp ? "verify" : "decoder", decoder.get<std::string>(), resources);
        for (const auto& entry : mtp ? mtp->at("prefill") : plan.at("prefill")) {
            auto variant = std::make_unique<PrefillVariant>();
            variant->capacity = entry.at("capacity").get<std::size_t>();
            const std::string capacity = "[" + std::to_string(variant->capacity) + "]";
            variant->graph = compile("prefill" + capacity, entry.at("graph").get<std::string>(), resources);
            if (mtp) {
                variant->head = compile("mtp_prefill" + capacity, entry.at("head").get<std::string>(), *head_resources);
            }
            for (const auto& [output, input] : entry.at("cache_map").items()) {
                cache_maps_[variant.get()].emplace_back(output, input.get<std::string>());
            }
            prefill_.push_back(std::move(variant));
        }
        std::sort(prefill_.begin(), prefill_.end(),
                  [](const auto& a, const auto& b) { return a->capacity < b->capacity; });
        if (mtp) {
            head_step_ = compile("mtp_step", mtp->at("head_step").get<std::string>(), *head_resources);
            verify_argmax_ = compile("argmax[verify]", mtp->at("argmax").get<std::string>(), *head_resources);
            head_argmax_ = compile("argmax[mtp_step]", mtp->at("argmax").get<std::string>(), *head_resources);
            adaptive_ = mtp->contains("decode");
            if (adaptive_) {
                plain_ = compile("decode", mtp->at("decode").get<std::string>(), resources);
                plain_head_ = compile("mtp_decode", mtp->at("head_decode").get<std::string>(), *head_resources);
            }
        }
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
        if (mtp_) {
            allocate_mtp();
        }
        if (!graph_logits_) {
            allocate_lm_head(plan.at("lm_head").value("resource_map", json::object()));
        }
        allocate_logits_readback();
        allocate_zero_source();
        runtime_.initialize_graphs(graphs());
        record_decode_step();
        record_prefill_steps();

        load_ms_ = milliseconds_since(started);
    }

    std::string summary() const {
        std::string capacities;
        for (const auto& variant : prefill_) {
            capacities += (capacities.empty() ? "" : ",") + std::to_string(variant->capacity);
        }
        return "adapter='" + gpu_.adapter_name() + "' arch=" + gpu_.gfx_arch() + " prefill_capacity=[" + capacities +
               "]" + (chunked_prefill_ ? " chunked_prefill" : "") +
               (mtp_ ? std::string(" speculative=mtp") + (adaptive_ ? "+adaptive" : "") : std::string()) +
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
        const std::vector<Chunk> chunks = prompt_chunks(prompt.size());
        if (mtp_) {
            return generate_mtp(prompt, chunks, max_new_tokens, stop_ids, on_token);
        }

        amica_engine::Generation result;
        const auto started = std::chrono::steady_clock::now();
        auto decode_started = started;
        try {
            std::size_t token = prefill(prompt, chunks);
            result.ttft_ms = milliseconds_since(started);
            result.ids.push_back(static_cast<std::int64_t>(token));
            bool keep_going = on_token(static_cast<std::int64_t>(token));

            decode_started = std::chrono::steady_clock::now();
            while (keep_going && result.ids.size() < max_new_tokens &&
                   stop_ids.count(static_cast<std::int64_t>(token)) == 0) {
                const auto position = static_cast<std::int64_t>(prompt.size() + result.ids.size() - 1);
                token = decode(token, position);
                result.ids.push_back(static_cast<std::int64_t>(token));
                keep_going = on_token(static_cast<std::int64_t>(token));
            }
        } catch (...) {
            // The next request must not submit this one's half-recorded
            // lists, nor rewrite inputs the GPU may still be reading.
            gpu_.abandon();
            throw;
        }
        result.decode_ms = result.ids.size() > 1 ? milliseconds_since(decode_started) : 0.0;
        result.tokens_per_second =
            result.decode_ms > 0.0 ? (result.ids.size() - 1) * 1000.0 / result.decode_ms : 0.0;
        return result;
    }

    bool device_removed() override { return runtime_.device_removed(); }

private:
    // MTP speculative greedy decoding with one draft token, as amica-driver's
    // --mtp-* mode runs it. Each drafting step verifies [newest, draft] in one
    // two-row target pass. The draft is accepted when it equals the target's
    // greedy token after the newest one, and then the second row's token is
    // emitted as well, so the tokens are those of one-token greedy decoding up
    // to the verify graph's rounding. A rejected draft leaves its row's
    // attention-cache entries past the mask, where the next step overwrites
    // them, and the next step's restore flag rolls the recurrent state back
    // from the verify graph's slots. The head then drafts the next token from
    // the verify rows' residual streams. One submission runs verify, its GPU
    // argmax, the head and the head's argmax, and reads back four token ids.
    //
    // With the one-row graphs, drafting is adaptive: a drafting step costs
    // verify_cost and emits 1 + acceptance tokens, a one-row step costs
    // plain_cost for one, so drafting pays while acceptance exceeds
    // verify_cost / plain_cost - 1. Both costs and the acceptance are running
    // averages; one-row steps keep scoring the head's drafts.
    amica_engine::Generation generate_mtp(const std::vector<std::size_t>& prompt, const std::vector<Chunk>& chunks,
                                          std::size_t max_new_tokens, const std::set<std::int64_t>& stop_ids,
                                          const std::function<bool(std::int64_t)>& on_token) {
        constexpr double kSmoothing = 0.2;
        constexpr double kHysteresis = 0.05;
        const auto average = [&](double& value, double sample) {
            value = value == 0.0 ? sample : value + kSmoothing * (sample - value);
        };
        double acceptance = 0.7;
        double verify_cost = 0.0;
        double plain_cost = 0.0;
        bool drafting = true;
        std::size_t steps = 0, accepted = 0, plain_steps = 0, plain_hits = 0;

        amica_engine::Generation result;
        const auto started = std::chrono::steady_clock::now();
        auto decode_started = started;
        try {
            auto [token, draft] = prefill_mtp(prompt, chunks);
            result.ttft_ms = milliseconds_since(started);
            bool keep_going = true;
            bool stop = false;
            const auto emit = [&](std::size_t id) {
                result.ids.push_back(static_cast<std::int64_t>(id));
                keep_going = on_token(static_cast<std::int64_t>(id));
                stop = stop_ids.count(static_cast<std::int64_t>(id)) != 0;
            };
            const auto more = [&] { return keep_going && !stop && result.ids.size() < max_new_tokens; };
            emit(token);

            decode_started = std::chrono::steady_clock::now();
            std::size_t newest = prompt.size();  // the position of the newest token
            const std::size_t capacity = decoder_inputs_.mask.elements();
            bool restore = false;
            while (more()) {
                const std::size_t last = static_cast<std::size_t>(result.ids.back());
                if (adaptive_ && verify_cost > 0.0) {
                    const double plain = plain_cost > 0.0 ? plain_cost : 0.75 * verify_cost;
                    const double break_even = verify_cost / plain - 1.0;
                    drafting = acceptance > break_even + (drafting ? -kHysteresis : kHysteresis);
                }
                // The token budget leaves room for the second verify row, so
                // this holds unless the server's limit changes.
                const bool fits = newest + 2 <= capacity;
                if (!fits && !adaptive_) break;
                if (drafting && fits) {
                    decoder_inputs_.token.write(0, static_cast<std::int64_t>(last));
                    decoder_inputs_.token.write(1, static_cast<std::int64_t>(draft));
                    set_mask_run(decoder_inputs_.mask, decoder_mask_ones_, newest + 2);
                    verify_restore_.write(0, restore ? 1 : 0);
                    set_mask_run(head_step_mask_, head_step_mask_ones_, newest + 2);
                    const auto step_started = std::chrono::steady_clock::now();
                    gpu_.execute_and_wait(draft_step_);
                    average(verify_cost, milliseconds_since(step_started));
                    // The verify step's two greedy ids, then the head's two.
                    const std::size_t target = read_id(0);
                    const bool hit = target == draft;
                    const std::size_t bonus = read_id(1);
                    draft = read_id(hit ? 3 : 2);
                    emit(target);
                    if (hit && more()) emit(bonus);
                    ++steps;
                    accepted += hit ? 1 : 0;
                    average(acceptance, hit ? 1.0 : 0.0);
                    restore = !hit;
                    newest += hit ? 2 : 1;
                } else {
                    // One row; after a rejected draft it resumes from the slots.
                    plain_inputs_.token.write(0, static_cast<std::int64_t>(last));
                    set_mask_run(plain_inputs_.mask, plain_mask_ones_, newest + 1);
                    plain_restore_.write(0, restore ? 1 : 0);
                    const auto step_started = std::chrono::steady_clock::now();
                    gpu_.execute_and_wait(plain_step_);
                    const std::size_t target = argmax_read_back(plain_logit_width_);
                    double cost = milliseconds_since(step_started);
                    const bool hit = target == draft;
                    emit(target);
                    ++plain_steps;
                    plain_hits += hit ? 1 : 0;
                    if (more()) {
                        plain_head_token_.write(0, static_cast<std::int64_t>(target));
                        set_mask_run(plain_head_mask_, plain_head_mask_ones_, newest + 1);
                        const auto head_started = std::chrono::steady_clock::now();
                        gpu_.execute_and_wait(plain_head_step_);
                        draft = argmax_read_back(plain_head_logit_width_);
                        cost += milliseconds_since(head_started);
                    }
                    average(acceptance, hit ? 1.0 : 0.0);
                    average(plain_cost, cost);
                    restore = false;
                    newest += 1;
                }
            }
        } catch (...) {
            gpu_.abandon();
            throw;
        }
        result.decode_ms = result.ids.size() > 1 ? milliseconds_since(decode_started) : 0.0;
        result.tokens_per_second =
            result.decode_ms > 0.0 ? (result.ids.size() - 1) * 1000.0 / result.decode_ms : 0.0;
        const std::size_t all_steps = steps + plain_steps;
        char line[256];
        std::snprintf(line, sizeof(line),
                      "mtp steps=%zu accepted=%zu acceptance=%.2f plain_steps=%zu plain_draft_hits=%zu "
                      "tokens_per_step=%.2f",
                      steps, accepted, steps ? static_cast<double>(accepted) / steps : 0.0, plain_steps, plain_hits,
                      all_steps ? static_cast<double>(result.ids.size() - 1) / all_steps : 0.0);
        log(line);
        return result;
    }

    // Id i of the int32 ids the drafting step reads back.
    std::size_t read_id(std::size_t i) const {
        std::int32_t id = 0;
        std::memcpy(&id, ids_mapped_ + i * sizeof(id), sizeof(id));
        if (id < 0 || static_cast<std::size_t>(id) >= vocabulary_) {
            throw std::runtime_error("the MTP argmax returned token " + std::to_string(id) +
                                     ", outside the vocabulary");
        }
        return static_cast<std::size_t>(id);
    }

    // A mask input holding a run of ones over the visible positions, kept in
    // its upload buffer between steps: within a request each step only adds
    // ones, so only a shorter run (a new request) clears it.
    static void set_mask_run(HostInput& mask, std::size_t& ones, std::size_t valid) {
        if (valid > mask.elements()) {
            throw std::runtime_error("the sequence exceeds the attention-mask capacity");
        }
        if (valid < ones) {
            mask.fill(0);
            ones = 0;
        }
        for (std::size_t i = ones; i < valid; ++i) {
            mask.write(i, 1);
        }
        ones = valid;
    }
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
        for (auto& variant : prefill_) {
            result.push_back(&variant->graph);
            if (mtp_) result.push_back(&variant->head);
        }
        if (!graph_logits_) result.push_back(&lm_head_);
        if (mtp_) {
            for (Graph* graph : {&head_step_, &verify_argmax_, &head_argmax_}) result.push_back(graph);
        }
        if (adaptive_) {
            result.push_back(&plain_);
            result.push_back(&plain_head_);
        }
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
    // Without manifest_fill the buffer is zeroed: the manifest names the
    // target's inputs, and other graphs reuse the same argN names.
    void add_reset(const ComPtr<ID3D12Resource>& buffer, const amica_io_desc& io, bool manifest_fill = true) {
        Reset reset{buffer, nullptr, io.size_bytes};
        const auto fill = manifest_fill ? initial_fill_.find(io.name) : initial_fill_.end();
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
        // The verify graph holds the newest token and the draft.
        const std::size_t rows = mtp_ ? 2 : 1;
        if (graph_logits_ && decoder_inputs_.token.elements() != rows) {
            throw std::runtime_error(graph.label + " token input must hold " + std::to_string(rows) + " token(s)");
        }
        if (graph_logits_) {
            decoder_inputs_.mask.fill(0);  // decode() writes only what changes
        }
        if (mtp_) {
            const int restore = restore_input(graph);
            verify_restore_ = host_input(graph.input(restore), true);
            bind_input(graph, restore, verify_restore_.buffer);
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

    // A rollback graph's int32 restore flag is its last entry parameter.
    static int restore_input(const Graph& graph) {
        int index = -1;
        int last = -1;
        for (std::uint32_t i = 0; i < graph.requirements.num_inputs; ++i) {
            const int position = entry_position(graph.input(static_cast<int>(i)).name);
            if (position > last) {
                last = position;
                index = static_cast<int>(i);
            }
        }
        if (index < 0 || graph.input(index).element_type != AMICA_ELEMENT_INT32 ||
            graph.input(index).size_bytes != sizeof(std::int32_t)) {
            throw std::runtime_error(graph.label + "'s last entry parameter is not the int32 restore flag");
        }
        return index;
    }

    // Binds an input to part of another graph's buffer.
    static void bind_input_at(Graph& graph, int index, const ComPtr<ID3D12Resource>& buffer, std::uint64_t offset) {
        graph.input_buffers[index] = buffer;
        graph.inputs[index] = buffer->GetGPUVirtualAddress() + offset;
    }

    void allocate_mtp() {
        for (auto& variant : prefill_) {
            bind_head(variant->head, variant->graph, &variant->head_token, variant->head_mask);
            bind_outputs(variant->head);
            if (variant->head_token.elements() != variant->capacity) {
                throw std::runtime_error(variant->head.label + " does not hold the rows of " + variant->graph.label);
            }
        }

        bind_argmax(verify_argmax_, decoder_, nullptr);
        bind_head(head_step_, decoder_, nullptr, head_step_mask_);
        // The head step reads the verify step's greedy ids, the second half of
        // the verify argmax result, as its token ids.
        const int tokens = head_step_.input_index("arg0");
        const auto& ids = verify_argmax_.output(0);
        if (tokens < 0 || 2 * head_step_.input(tokens).size_bytes != ids.size_bytes) {
            throw std::runtime_error("the mtp_step token ids are not the ids half of the argmax result");
        }
        bind_input_at(head_step_, tokens, verify_argmax_.output_buffers[0], ids.size_bytes / 2);
        bind_outputs(head_step_);
        bind_argmax(head_argmax_, head_step_, &verify_argmax_);
        const auto& result = head_argmax_.output(0);
        if (result.element_type != AMICA_ELEMENT_INT32 || result.size_bytes != 4 * sizeof(std::int32_t)) {
            throw std::runtime_error("the MTP argmax result is not four int32 token ids");
        }
        ids_readback_ = gpu_.create_buffer(result.size_bytes, D3D12_HEAP_TYPE_READBACK);
        void* mapped = nullptr;
        check_hr(ids_readback_->Map(0, nullptr, &mapped), "Map");
        ids_mapped_ = static_cast<const std::uint8_t*>(mapped);

        if (adaptive_) {
            allocate_plain();
            bind_head(plain_head_, plain_, &plain_head_token_, plain_head_mask_);
            bind_outputs(plain_head_);
            if (plain_head_token_.elements() != 1) {
                throw std::runtime_error(plain_head_.label + " token input must hold one token");
            }
        }
    }

    // Head graph ABI (qwen38_mtp.py), by entry position: 0 the token ids, 1
    // the attention mask, 2 the target graph's residual streams (its last
    // output), then the head's attention caches, which it updates in place.
    // The first head graph allocates the caches and the others share them by
    // name. Without `tokens` the caller binds the token ids.
    void bind_head(Graph& head, Graph& target, HostInput* tokens, HostInput& mask) {
        const int streams = static_cast<int>(target.requirements.num_outputs) - 1;
        for (std::uint32_t i = 0; i < head.requirements.num_inputs; ++i) {
            const int index = static_cast<int>(i);
            const auto& io = head.input(index);
            const int position = entry_position(io.name);
            if (position == 0) {
                if (tokens) {
                    *tokens = host_input(io, true);
                    bind_input(head, index, tokens->buffer);
                }
            } else if (position == 1) {
                mask = host_input(io, true);
                mask.fill(0);
                bind_input(head, index, mask.buffer);
            } else if (position == 2) {
                if (streams < 0 || target.output(streams).size_bytes != io.size_bytes) {
                    throw std::runtime_error(head.label + " streams input does not match the last output of " +
                                             target.label);
                }
                bind_input(head, index, target.output_buffers[streams]);
            } else if (position >= 3) {
                auto& cache = head_caches_[io.name];
                if (!cache) {
                    cache = gpu_.create_buffer(io.size_bytes);
                    add_reset(cache, io, false);
                } else if (cache->GetDesc().Width < io.size_bytes) {
                    throw std::runtime_error(head.label + " cache " + io.name + " is larger than the shared one");
                }
                bind_input(head, index, cache);
                bool in_place = false;
                for (std::uint32_t o = 0; o < head.requirements.num_outputs; ++o) {
                    in_place = in_place || head.output(static_cast<int>(o)).aliased_input == index;
                }
                if (!in_place) {
                    throw std::runtime_error(head.label + " does not update its cache " + io.name + " in place");
                }
            } else {
                throw std::runtime_error(head.label + " has an unexpected input " +
                                         (io.name != nullptr ? io.name : "(unnamed)"));
            }
        }
        if (!mask) {
            throw std::runtime_error(head.label + " has no attention-mask input");
        }
    }

    // Argmax graph ABI (qwen38_mtp.py), by entry position: 0 the logits rows
    // it ranks, 1 an int32 id prefix its result starts with. The verify
    // argmax's prefix is zero and the head argmax's is the verify ids, so the
    // head argmax result holds both steps' ids.
    void bind_argmax(Graph& argmax, Graph& source, Graph* prefix) {
        if (argmax.requirements.num_inputs != 2 || argmax.requirements.num_outputs != 1) {
            throw std::runtime_error(argmax.label + " does not take logits and an id prefix and return ids");
        }
        std::uint64_t prefix_bytes = 0;
        for (int index = 0; index < 2; ++index) {
            const auto& io = argmax.input(index);
            const int position = entry_position(io.name);
            if (position == 0) {
                if (io.size_bytes != source.output(0).size_bytes) {
                    throw std::runtime_error(argmax.label + " does not rank the logits of " + source.label);
                }
                bind_input(argmax, index, source.output_buffers[0]);
            } else if (position == 1) {
                prefix_bytes = io.size_bytes;
                if (prefix) {
                    const std::uint64_t half = prefix->output(0).size_bytes / 2;
                    if (half != io.size_bytes) {
                        throw std::runtime_error(argmax.label + " prefix is not the ids half of " + prefix->label);
                    }
                    bind_input_at(argmax, index, prefix->output_buffers[0], half);
                } else {
                    const auto buffer = gpu_.create_buffer(io.size_bytes);
                    add_reset(buffer, io, false);
                    bind_input(argmax, index, buffer);
                }
            } else {
                throw std::runtime_error(argmax.label + " has an unexpected input");
            }
        }
        if (prefix_bytes == 0 || argmax.output(0).size_bytes != 2 * prefix_bytes) {
            throw std::runtime_error(argmax.label + " does not return its prefix followed by the token ids");
        }
        bind_outputs(argmax);
    }

    // The one-row target step declares the verify step's entry parameters, so
    // a same-named input is the same state and shares the verify step's
    // buffer; its token ids, mask and restore flag are its own.
    void allocate_plain() {
        Graph& graph = plain_;
        const auto bound = bind_prompt_inputs(graph, decoder_names_, plain_inputs_);
        if (plain_inputs_.token.elements() != 1) {
            throw std::runtime_error(graph.label + " token input must hold one token");
        }
        plain_inputs_.mask.fill(0);
        const int restore = restore_input(graph);
        plain_restore_ = host_input(graph.input(restore), true);
        bind_input(graph, restore, plain_restore_.buffer);
        for (std::uint32_t i = 0; i < graph.requirements.num_inputs; ++i) {
            const int index = static_cast<int>(i);
            if (bound.count(index) != 0 || index == restore) continue;
            const auto& io = graph.input(index);
            const int state = decoder_.input_index(io.name);
            if (state < 0 || decoder_.input(state).size_bytes != io.size_bytes) {
                throw std::runtime_error(graph.label + " input " + io.name + " has no matching " + decoder_.label +
                                         " state");
            }
            bind_input(graph, index, decoder_.input_buffers[state]);
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
            for (auto& variant : prefill_) {
                producers.push_back(&variant->graph);
                if (mtp_) producers.push_back(&variant->head);
            }
        } else {
            producers.push_back(&lm_head_);
        }
        if (adaptive_) {
            producers.push_back(&plain_);
            producers.push_back(&plain_head_);
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

    // Records the copy of one row of logits from output 0 of graph into the
    // readback buffer; returns the width of a logit in bytes.
    std::size_t read_back_logits(Graph& graph, std::uint64_t row) {
        const auto& logits = graph.output(0);
        const std::size_t width = element_bytes(logits.element_type);
        const std::uint64_t row_bytes = vocabulary_ * width;
        if (row >= logits.size_bytes / row_bytes) {
            throw std::runtime_error("the logits row is outside " + graph.label + " output 0");
        }
        gpu_.read_back(logits_readback_.Get(), graph.output_buffers[0].Get(), row * row_bytes, row_bytes);
        return width;
    }

    // Reads one row of logits from output 0 of graph and returns its argmax.
    std::size_t argmax_logits(Graph& graph, std::uint64_t row) {
        const std::size_t width = read_back_logits(graph, row);
        gpu_.submit_and_wait();
        return argmax_read_back(width);
    }

    // The index of the largest finite logit in the readback buffer, the first
    // of equal ones.
    std::size_t argmax_read_back(std::size_t width) const {
        std::size_t best_token = 0;
        if (width == 4) {
            float best = -std::numeric_limits<float>::infinity();
            for (std::size_t i = 0; i < vocabulary_; ++i) {
                float value;
                std::memcpy(&value, logits_mapped_ + i * 4, 4);
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
        // Finite halves compare as their magnitude bits, negated when the sign
        // is set (so +0 and -0 tie, as they do as floats), which spares a float
        // conversion per logit over a vocabulary of a quarter million.
        int best = (std::numeric_limits<int>::min)();
        const auto* halves = reinterpret_cast<const std::uint16_t*>(logits_mapped_);
        for (std::size_t i = 0; i < vocabulary_; ++i) {
            const std::uint16_t half = halves[i];
            if ((half & 0x7C00u) == 0x7C00u) continue;  // infinity or NaN
            const int magnitude = half & 0x7FFF;
            const int key = (half & 0x8000u) != 0 ? -magnitude : magnitude;
            if (key > best) {
                best = key;
                best_token = i;
            }
        }
        if (best == (std::numeric_limits<int>::min)()) {
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

    // The prefill runs of a prompt: the smallest graph that holds it, or, for
    // a prompt longer than every graph, one partial chunk and then full chunks
    // of the largest graph: a chunked graph places its rows after the ones
    // already cached, which it can tell only when every chunk after the first
    // is full.
    std::vector<Chunk> prompt_chunks(std::size_t tokens) {
        for (auto& candidate : prefill_) {
            if (candidate->capacity >= tokens) return {{candidate.get(), 0, tokens}};
        }
        if (!chunked_prefill_) {
            throw std::runtime_error("the prompt has " + std::to_string(tokens) +
                                     " tokens, more than the largest prefill capacity " +
                                     std::to_string(prefill_.back()->capacity));
        }
        PrefillVariant& full = *prefill_.back();
        const std::size_t partial = tokens % full.capacity;
        const std::size_t first = partial == 0 ? full.capacity : partial;
        PrefillVariant* first_variant = &full;
        for (auto& candidate : prefill_) {
            if (candidate->capacity >= first) {
                first_variant = candidate.get();
                break;
            }
        }
        std::vector<Chunk> chunks{{first_variant, 0, first}};
        for (std::size_t done = first; done < tokens; done += full.capacity) {
            chunks.push_back({&full, done, full.capacity});
        }
        return chunks;
    }

    // Returns the greedy token after the prompt.
    std::size_t prefill(const std::vector<std::size_t>& prompt, const std::vector<Chunk>& chunks) {
        std::size_t token = 0;
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            const Chunk& chunk = chunks[i];
            token = prefill_chunk(*chunk.variant, prompt.data() + chunk.begin, chunk.n, chunk.begin,
                                  i + 1 == chunks.size());
        }
        return token;
    }

    // Returns the greedy token after the prompt and the head's draft of the
    // token after that. Each prefill chunk is followed by the head over its
    // rows. Head row j reads target row j's streams and the token after it,
    // so the head's last prompt row needs the first generated token: the last
    // chunk's head runs once its target's token has been read.
    std::pair<std::size_t, std::size_t> prefill_mtp(const std::vector<std::size_t>& prompt,
                                                    const std::vector<Chunk>& chunks) {
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            const Chunk& chunk = chunks[i];
            PrefillVariant& variant = *chunk.variant;
            write_prompt(variant, prompt.data() + chunk.begin, chunk.n, chunk.begin);
            if (chunk.begin == 0) {
                gpu_.execute(reset_step_);
            }
            gpu_.execute(variant.step);
            if (i + 1 < chunks.size()) {
                write_head_prompt(variant, prompt, chunk.begin, chunk.n, 0);
                gpu_.execute(variant.head_step);
                gpu_.submit_and_wait();  // the next chunk rewrites the inputs
                continue;
            }
            const std::size_t first = read_prompt_token(variant, chunk.n);
            write_head_prompt(variant, prompt, chunk.begin, chunk.n, first);
            gpu_.execute_and_wait(variant.head_step);
            return {first, argmax_read_back(head_logit_width_)};
        }
        throw std::logic_error("a prompt has at least one prefill chunk");
    }

    // The head's inputs for the chunk of `n` rows from `begin`: row j holds
    // the token after prompt row begin + j, and the last prompt row the first
    // generated token.
    void write_head_prompt(PrefillVariant& variant, const std::vector<std::size_t>& prompt, std::size_t begin,
                           std::size_t n, std::size_t first) {
        variant.head_token.fill(pad_token_id_);
        for (std::size_t row = 0; row < n; ++row) {
            const std::size_t next = begin + row + 1;
            variant.head_token.write(row, static_cast<std::int64_t>(next < prompt.size() ? prompt[next] : first));
        }
        const std::size_t visible = begin + n;
        if (visible > variant.head_mask.elements()) {
            throw std::runtime_error("the prompt exceeds the MTP head's attention-mask capacity");
        }
        variant.head_mask.fill(0);
        for (std::size_t i = 0; i < visible; ++i) {
            variant.head_mask.write(i, 1);
        }
    }

    // Feeds `n` prompt tokens after `past` cached ones. The first chunk resets
    // the state; only the last reads back its next token.
    std::size_t prefill_chunk(PrefillVariant& variant, const std::size_t* prompt, std::size_t n,
                              std::size_t past, bool last) {
        write_prompt(variant, prompt, n, past);
        if (past == 0) {
            gpu_.execute(reset_step_);
        }
        gpu_.execute(variant.step);
        if (!last) {
            gpu_.submit_and_wait();  // the next chunk rewrites the inputs
            return 0;
        }
        return read_prompt_token(variant, n);
    }

    // Writes `n` prompt tokens after `past` cached ones into a prefill
    // graph's inputs.
    void write_prompt(PrefillVariant& variant, const std::size_t* prompt, std::size_t n, std::size_t past) {
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
    }

    // The greedy token after the last prompt row of the prefill just
    // submitted, `n` rows long. Which row holds the last prompt token depends
    // on the prompt, so the readback is recorded per request.
    std::size_t read_prompt_token(PrefillVariant& variant, std::size_t n) {
        if (graph_logits_) {
            const std::uint64_t rows = variant.graph.output(0).size_bytes /
                                       (vocabulary_ * element_bytes(variant.graph.output(0).element_type));
            const std::uint64_t row = rows == 1 ? 0 : last_prompt_row(rows, n, 0);
            gpu_.begin_list();
            return argmax_logits(variant.graph, row);
        }
        const std::uint64_t row_bytes = embeddings_->columns() * sizeof(float);
        const std::uint64_t row = last_prompt_row(variant.graph.output(0).size_bytes / row_bytes, n, tail_row_offset_);
        gpu_.begin_list();
        gpu_.copy_between(lm_input_.Get(), 0, variant.graph.output_buffers[0].Get(), row * row_bytes, row_bytes);
        gpu_.uav_barrier();
        return run_lm_head(lm_input_->GetGPUVirtualAddress());
    }

    // A decode step binds the same buffers for every token, so its command
    // lists are recorded once, as amica-driver records its decode replay, and
    // executed for each token. Encoding the decoder anew per token would keep
    // the GPU idle while the CPU records thousands of dispatches.
    void record_decode_step() {
        if (mtp_) {
            record_mtp_steps();
            return;
        }
        gpu_.begin_sequence();
        gpu_.begin_list();
        upload(decoder_inputs_);
        gpu_.uav_barrier();
        encode(decoder_);
        if (graph_logits_) {
            const std::uint64_t rows =
                decoder_.output(0).size_bytes / (vocabulary_ * element_bytes(decoder_.output(0).element_type));
            decode_logit_width_ = read_back_logits(decoder_, rows - 1);
        } else {
            const std::uint64_t row_bytes = embeddings_->columns() * sizeof(float);
            const std::uint64_t rows = decoder_.output(0).size_bytes / row_bytes;
            // Prefill rebinds the LM head's input to lm_input_ for each request.
            lm_head_.inputs[tail_input_index_] =
                decoder_.output_buffers[0]->GetGPUVirtualAddress() + (rows - 1) * row_bytes;
            encode(lm_head_);
            decode_logit_width_ = read_back_logits(lm_head_, 0);
        }
        decode_step_ = gpu_.end_sequence();
    }

    // A drafting step is one submission: the verify step, its argmax, the head
    // over the verify ids and the head's argmax, then the readback of the four
    // ids. The one-row step and its head read back one row of logits each.
    void record_mtp_steps() {
        gpu_.begin_sequence();
        gpu_.begin_list();
        upload(decoder_inputs_);
        upload(verify_restore_);
        upload(head_step_mask_);
        gpu_.uav_barrier();
        for (Graph* graph : {&decoder_, &verify_argmax_, &head_step_, &head_argmax_}) encode(*graph);
        gpu_.read_back(ids_readback_.Get(), head_argmax_.output_buffers[0].Get(), 0, head_argmax_.output(0).size_bytes);
        draft_step_ = gpu_.end_sequence();
        if (!adaptive_) return;

        gpu_.begin_sequence();
        gpu_.begin_list();
        upload(plain_inputs_);
        upload(plain_restore_);
        gpu_.uav_barrier();
        encode(plain_);
        plain_logit_width_ = read_back_logits(plain_, 0);
        plain_step_ = gpu_.end_sequence();

        gpu_.begin_sequence();
        gpu_.begin_list();
        upload(plain_head_token_);
        upload(plain_head_mask_);
        gpu_.uav_barrier();
        encode(plain_head_);
        plain_head_logit_width_ = read_back_logits(plain_head_, 0);
        plain_head_step_ = gpu_.end_sequence();
    }

    // Prefill, too, binds the same buffers for every request: each variant's
    // uploads and graph, and the state reset before a request's first chunk,
    // are recorded once.
    void record_prefill_steps() {
        for (auto& variant : prefill_) {
            gpu_.begin_sequence();
            gpu_.begin_list();
            upload(variant->inputs);
            gpu_.uav_barrier();
            encode(variant->graph);
            variant->step = gpu_.end_sequence();
            if (mtp_) {
                gpu_.begin_sequence();
                gpu_.begin_list();
                upload(variant->head_token);
                upload(variant->head_mask);
                gpu_.uav_barrier();
                encode(variant->head);
                head_logit_width_ = read_back_logits(variant->head, 0);
                variant->head_step = gpu_.end_sequence();
            }
        }
        gpu_.begin_sequence();
        gpu_.begin_list();
        for (const auto& [resource, reset] : resets_) {
            gpu_.copy_into(reset.buffer.Get(), 0, reset.source ? reset.source.Get() : zero_source_.Get(), 0,
                           reset.size);
        }
        reset_step_ = gpu_.end_sequence();
    }

    // Feeds `token` at `position` (zero-based; the prompt fills 0..n-1).
    std::size_t decode(std::size_t token, std::int64_t position) {
        PromptInputs& inputs = decoder_inputs_;
        if (graph_logits_) {
            inputs.token.write(0, static_cast<std::int64_t>(token));
            set_mask_run(inputs.mask, decoder_mask_ones_, static_cast<std::size_t>(position + 1));
        } else {
            embeddings_->write_f32_row(token, reinterpret_cast<float*>(inputs.embedding.mapped));
        }
        if (inputs.position) inputs.position.write(0, position);
        if (inputs.sequence) inputs.sequence.write(0, position + 1);

        gpu_.execute_and_wait(decode_step_);
        return argmax_read_back(decode_logit_width_);
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
    std::size_t decoder_mask_ones_ = 0;
    Gpu::Sequence decode_step_;
    std::size_t decode_logit_width_ = 0;
    int tail_input_index_ = -1;
    ComPtr<ID3D12Resource> lm_input_;
    ComPtr<ID3D12Resource> logits_readback_;
    const std::uint8_t* logits_mapped_ = nullptr;

    std::map<ID3D12Resource*, Reset> resets_;
    std::uint64_t zero_size_ = 0;
    ComPtr<ID3D12Resource> zero_source_;
    Gpu::Sequence reset_step_;
    double load_ms_ = 0.0;

    // MTP: decoder_ is the verify step and prefill_ the MTP prefill graphs,
    // each with its head prefill. adaptive_ adds the one-row step and head.
    bool mtp_ = false;
    bool adaptive_ = false;
    Graph head_step_;
    Graph verify_argmax_;
    Graph head_argmax_;
    HostInput verify_restore_;
    HostInput head_step_mask_;
    std::size_t head_step_mask_ones_ = 0;
    std::map<std::string, ComPtr<ID3D12Resource>> head_caches_;
    Gpu::Sequence draft_step_;
    ComPtr<ID3D12Resource> ids_readback_;
    const std::uint8_t* ids_mapped_ = nullptr;
    std::size_t head_logit_width_ = 0;
    Graph plain_;
    PromptInputs plain_inputs_;
    HostInput plain_restore_;
    std::size_t plain_mask_ones_ = 0;
    Gpu::Sequence plain_step_;
    std::size_t plain_logit_width_ = 0;
    Graph plain_head_;
    HostInput plain_head_token_;
    HostInput plain_head_mask_;
    std::size_t plain_head_mask_ones_ = 0;
    Gpu::Sequence plain_head_step_;
    std::size_t plain_head_logit_width_ = 0;
};

int usage() {
    std::cerr << "usage: amica-engine --model-dir <bundle> --amica-dir <AMICA install>\\amica --port N "
                 "[--host 127.0.0.1] [--model-name NAME] [--adapter <index|name>] [--arch gfxNNNN] "
                 "[--weights-vram-mib N] [--speculative auto|off] "
                 "[--transformer-precision source|int4-weight-only]\n"
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
    // auto: MTP drafts when the bundle's speculative.json has them.
    std::string speculative = "auto";
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
        } else if (option == "--speculative") {
            if (value != "auto" && value != "off") return usage();
            speculative = value;
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
        if (!bundle.speculative_note().empty() && speculative != "off") {
            log(bundle.speculative_note());
        }
        Engine engine(bundle.plan(), adapter, arch, weights_vram_mib, speculative != "off");
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
