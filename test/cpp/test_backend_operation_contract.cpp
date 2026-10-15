#include "lemon/backends/backend_registry.h"
#include "lemon/backends/hrx/hrx_server.h"
#include "lemon/backends/llamacpp/llamacpp_server.h"
#include "lemon/wrapped_server.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <mutex>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

thread_local bool track_allocations = false;
std::atomic<size_t> query_allocations{0};
int failures = 0;
size_t checks = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

using namespace lemon;
using Op = BackendOperation;
using Delivery = OperationDelivery;

struct ExpectedBackend {
    const char* recipe;
    std::set<Op> buffered;
    std::set<Op> streaming;
    std::set<Op> realtime;
    std::set<Op> legacy_streaming;
};

const std::vector<ExpectedBackend> k_expected_backends = {
    {"llamacpp", {Op::ChatCompletion, Op::Completion, Op::Responses, Op::Embeddings, Op::Reranking,
                   Op::Slots, Op::SlotAction, Op::Tokenize}, {Op::ChatCompletion, Op::Completion, Op::Responses}, {}, {}},
    {"llamacpp-hrx", {Op::ChatCompletion, Op::Completion, Op::Responses, Op::Embeddings, Op::Reranking,
                       Op::Slots, Op::SlotAction, Op::Tokenize}, {Op::ChatCompletion, Op::Completion, Op::Responses}, {}, {}},
    {"ds4", {Op::ChatCompletion, Op::Completion, Op::Responses}, {Op::ChatCompletion, Op::Completion, Op::Responses}, {}, {}},
    {"ryzenai-llm", {Op::ChatCompletion, Op::Completion, Op::Responses}, {Op::ChatCompletion, Op::Completion, Op::Responses}, {}, {}},
    {"vllm", {Op::ChatCompletion, Op::Completion, Op::Responses}, {Op::ChatCompletion, Op::Completion, Op::Responses}, {}, {}},
    {"cloud", {Op::ChatCompletion, Op::Completion}, {Op::ChatCompletion, Op::Completion, Op::Responses}, {}, {Op::Responses}},
    {"flm", {Op::ChatCompletion, Op::Completion, Op::Embeddings, Op::Transcription},
             {Op::ChatCompletion, Op::Completion, Op::Responses}, {}, {Op::Responses}},
    {"whispercpp", {Op::Transcription}, {Op::ChatCompletion, Op::Completion, Op::Responses}, {},
                    {Op::ChatCompletion, Op::Completion, Op::Responses}},
    {"moonshine", {Op::Transcription}, {Op::ChatCompletion, Op::Completion, Op::Responses}, {Op::StreamingTranscription},
                   {Op::ChatCompletion, Op::Completion, Op::Responses}},
    {"kokoro", {Op::TextToSpeech}, {Op::ChatCompletion, Op::Completion, Op::Responses, Op::TextToSpeech}, {},
                {Op::ChatCompletion, Op::Completion, Op::Responses}},
    {"sd-cpp", {Op::ImageGeneration, Op::ImageEdits, Op::ImageVariations, Op::Upscale},
                {Op::ChatCompletion, Op::Completion, Op::Responses}, {}, {Op::ChatCompletion, Op::Completion, Op::Responses}},
    {"thenoise", {Op::ImageGeneration, Op::ImageEdits, Op::Upscale},
                  {Op::ChatCompletion, Op::Completion, Op::Responses}, {}, {Op::ChatCompletion, Op::Completion, Op::Responses}},
    {"onnxruntime", {Op::Classification}, {Op::ChatCompletion, Op::Completion, Op::Responses}, {},
                     {Op::ChatCompletion, Op::Completion, Op::Responses}},
    {"thinksound", {Op::AudioGeneration}, {Op::ChatCompletion, Op::Completion, Op::Responses}, {},
                    {Op::ChatCompletion, Op::Completion, Op::Responses}},
    {"acestep", {Op::AudioGeneration}, {Op::ChatCompletion, Op::Completion, Op::Responses}, {},
                 {Op::ChatCompletion, Op::Completion, Op::Responses}},
    {"trellis", {Op::Model3DGeneration}, {Op::ChatCompletion, Op::Completion, Op::Responses}, {},
                 {Op::ChatCompletion, Op::Completion, Op::Responses}},
    {"openmoss", {Op::TextToSpeech, Op::AudioGeneration},
                  {Op::ChatCompletion, Op::Completion, Op::Responses, Op::TextToSpeech}, {},
                  {Op::ChatCompletion, Op::Completion, Op::Responses}}
};

bool expected_support(const ExpectedBackend& backend, Op operation, Delivery delivery, ModelType mode) {
    const auto& operations = delivery == Delivery::Buffered ? backend.buffered
        : delivery == Delivery::Streaming ? backend.streaming : backend.realtime;
    if (!operations.count(operation)) return false;
    const std::string recipe = backend.recipe;
    if (recipe == "flm") {
        if (operation == Op::Transcription) return mode == ModelType::TRANSCRIPTION;
        if (operation == Op::Embeddings) return mode != ModelType::TRANSCRIPTION;
        return mode != ModelType::TRANSCRIPTION && mode != ModelType::EMBEDDING;
    }
    if (operation == Op::TextToSpeech) return mode == ModelType::TTS;
    if (operation == Op::AudioGeneration) return mode == ModelType::AUDIO_GENERATION;
    return true;
}

bool expected_rejection(const ExpectedBackend& backend, Op operation, Delivery delivery, ModelType mode) {
    const std::string recipe = backend.recipe;
    if (recipe == "thenoise") return operation == Op::ImageVariations && delivery == Delivery::Buffered;
    if (recipe != "flm") return false;
    if (delivery == Delivery::Streaming && operation <= Op::Responses) {
        return mode == ModelType::EMBEDDING || mode == ModelType::TRANSCRIPTION;
    }
    if (delivery != Delivery::Buffered) return false;
    return (operation == Op::Embeddings && mode == ModelType::TRANSCRIPTION) ||
           (operation == Op::Transcription && mode != ModelType::TRANSCRIPTION);
}

bool has_dispatch_adapter(WrappedServer& server, Op operation, Delivery delivery, ModelType mode) {
    switch (operation) {
        case Op::ChatCompletion:
        case Op::Completion: return operation_adapter<ICompletionServer>(server, operation, delivery, mode);
        case Op::Responses:
            return server.supports_operation(operation, delivery, mode) &&
                (delivery == Delivery::Buffered ? server.operation_bindings()->adapters().responses != nullptr
                                               : server.operation_bindings()->adapters().streaming_forwarder != nullptr);
        case Op::Embeddings: return operation_adapter<IEmbeddingsServer>(server, operation, delivery, mode);
        case Op::Reranking: return operation_adapter<IRerankingServer>(server, operation, delivery, mode);
        case Op::Classification: return operation_adapter<IClassificationServer>(server, operation, delivery, mode);
        case Op::Transcription: return operation_adapter<ITranscriptionServer>(server, operation, delivery, mode);
        case Op::StreamingTranscription: return operation_adapter<IStreamingTranscriptionServer>(server, operation, delivery, mode);
        case Op::TextToSpeech: return operation_adapter<ITextToSpeechServer>(server, operation, delivery, mode);
        case Op::AudioGeneration: return operation_adapter<IAudioGenerationServer>(server, operation, delivery, mode);
        case Op::ImageGeneration:
        case Op::ImageEdits:
        case Op::ImageVariations: return operation_adapter<IImageServer>(server, operation, delivery, mode);
        case Op::Upscale: return operation_adapter<IUpscaleServer>(server, operation, delivery, mode);
        case Op::Model3DGeneration: return operation_adapter<IModel3DServer>(server, operation, delivery, mode);
        case Op::Slots:
        case Op::SlotAction: return operation_adapter<ISlotsServer>(server, operation, delivery, mode);
        case Op::Tokenize: return operation_adapter<ITokenizerServer>(server, operation, delivery, mode);
        default: return false;
    }
}

void check_complete_matrix() {
    static_assert(k_backend_operation_count == 18);
    static_assert(k_operation_delivery_count == 3);
    static_assert(k_operation_model_types.size() == 9);
    check(backends::all_registrations().size() == 17, "every registered backend has an independent fixture");
    std::set<std::string> seen;
    for (const auto& registration : backends::all_registrations()) {
        const auto found = std::find_if(k_expected_backends.begin(), k_expected_backends.end(), [&](const auto& item) {
            return registration.descriptor->recipe == item.recipe;
        });
        check(found != k_expected_backends.end(), "no unclassified backend");
        if (found == k_expected_backends.end()) continue;
        seen.insert(found->recipe);
        ModelInfo model;
        model.cloud_provider = "operation-contract-fixture";
        backends::BackendContext context;
        context.log_level = "error";
        context.model_info = &model;
        auto server = backends::create_server(found->recipe, context);
        check(server && server->operation_bindings(), std::string(found->recipe) + " binds before load");
        for (size_t op = 0; op < 18; ++op) {
            for (size_t delivery = 0; delivery < 3; ++delivery) {
                for (const auto mode : k_operation_model_types) {
                    const auto operation = static_cast<Op>(op);
                    const auto transport = static_cast<Delivery>(delivery);
                    check(server->supports_operation(operation, transport, mode) == expected_support(*found, operation, transport, mode),
                          std::string(found->recipe) + " operation " + std::to_string(op) + " delivery " +
                              std::to_string(delivery) + " mode " + model_type_to_string(mode));
                    const auto label = std::string(found->recipe) + " adapter/rejection operation " + std::to_string(op) +
                        " delivery " + std::to_string(delivery) + " mode " + model_type_to_string(mode);
                    check(has_dispatch_adapter(*server, operation, transport, mode) == expected_support(*found, operation, transport, mode), label);
                    check(server->operation_bindings()->has_rejection({operation, transport, mode}) ==
                          expected_rejection(*found, operation, transport, mode), label);
                }
            }
        }
        for (const auto& description : server->operation_bindings()->descriptions()) {
            const bool legacy = description.delivery == Delivery::Streaming && found->legacy_streaming.count(description.operation);
            check((description.provenance == OperationProvenance::LegacyForwarding) == legacy,
                  std::string(found->recipe) + " independently pins forwarding provenance");
        }
        server->unload();
        for (const auto mode : k_operation_model_types) {
            check(server->supports_operation(Op::Upscale, Delivery::Buffered, mode) == found->buffered.count(Op::Upscale),
                  "upscale query survives unload");
        }
        check(!server->supports_operation(static_cast<Op>(255), Delivery::Buffered, ModelType::LLM), "invalid operation");
        check(!server->supports_operation(Op::ChatCompletion, static_cast<Delivery>(255), ModelType::LLM), "invalid delivery");
        check(!server->supports_operation(Op::ChatCompletion, Delivery::Buffered, static_cast<ModelType>(-1)), "negative model enum");
        check(!server->supports_operation(Op::ChatCompletion, Delivery::Buffered, static_cast<ModelType>(100)), "invalid model enum");
    }
    check(seen.size() == k_expected_backends.size(), "all hand-authored fixtures are exercised");
    backends::LlamaCppServer fallback("error", nullptr, nullptr);
    check(fallback.supports_operation(Op::Embeddings, Delivery::Buffered, ModelType::LLM), "direct llama.cpp fallback constructor binds adapters");
    check(backends::create_server("unknown-fixture", {}) == nullptr, "unknown recipe retains registry miss");
    check(!(backends::hrx::capabilities() & CAP_EMBEDDINGS), "HRX advertised mask remains narrow");
    check(backends::hrx::capabilities() == (backends::llamacpp::capabilities() & ~(CAP_EMBEDDINGS | CAP_RERANKING)),
          "HRX entire advertised set differs only by embeddings and reranking");
}

class WitnessServer : public WrappedServer, public IEmbeddingsServer, public IRerankingServer,
                      public IClassificationServer, public ITranscriptionServer,
                      public IStreamingTranscriptionServer, public ITextToSpeechServer,
                      public IImageServer, public IUpscaleServer, public IAudioGenerationServer,
                      public IModel3DServer, public ISlotsServer, public ITokenizerServer {
public:
    explicit WitnessServer(LegacyOperationProfile profile)
        : WrappedServer("fixture", "error") {
        bind_legacy_operations(profile, *this);
    }
    std::array<int, 18> calls{};
    mutable std::atomic<int> probes{0};
    json hit(Op operation) { ++calls[static_cast<size_t>(operation)]; return {{"witness", static_cast<int>(operation)}}; }
    void load(const std::string&, const ModelInfo&, const RecipeOptions&, bool) override { ++probes; }
    bool is_backend_alive() const override { ++probes; return true; }
    std::string availability() const override { ++probes; return ""; }
    std::unique_lock<std::mutex> hold_state_mutex() { return std::unique_lock<std::mutex>(state_mutex_); }
    std::unique_lock<std::mutex> hold_process_mutex() { return std::unique_lock<std::mutex>(process_mutex_); }
    json chat_completion(const json&) override { return hit(Op::ChatCompletion); }
    json completion(const json&) override { return hit(Op::Completion); }
    json responses(const json&) override { return hit(Op::Responses); }
    json embeddings(const json&) override { return hit(Op::Embeddings); }
    json reranking(const json&) override { return hit(Op::Reranking); }
    json classify(const json&) override { return hit(Op::Classification); }
    json audio_transcriptions(const json&) override { return hit(Op::Transcription); }
    std::string get_streaming_address() override { hit(Op::StreamingTranscription); return "tcp://fixture"; }
    void audio_speech(const json&, httplib::DataSink&) override { hit(Op::TextToSpeech); }
    void audio_generations(const json&, httplib::DataSink&) override { hit(Op::AudioGeneration); }
    json image_generations(const json&) override { return hit(Op::ImageGeneration); }
    json image_edits(const json&) override { return hit(Op::ImageEdits); }
    json image_variations(const json&) override { return hit(Op::ImageVariations); }
    std::string upscale_via_cli(const std::string&, const std::string&) override { hit(Op::Upscale); return "fixture"; }
    void model_3d_generations(const json&, httplib::DataSink&) override { hit(Op::Model3DGeneration); }
    json get_slots() override { return hit(Op::Slots); }
    json slots_action(int, const std::string&, const json&) override { return hit(Op::SlotAction); }
    json tokenize(const json&) override { return hit(Op::Tokenize); }
    void forward_streaming_request(const std::string& endpoint, const std::string&, httplib::DataSink&,
                                   bool, long, TelemetryCallback) override {
        hit(endpoint == "/v1/chat/completions" ? Op::ChatCompletion : endpoint == "/v1/completions" ? Op::Completion : Op::Responses);
    }
};

void check_witnesses() {
    WitnessServer llm(LegacyOperationProfile::LlamaCpp);
    WitnessServer transcription(LegacyOperationProfile::Moonshine);
    WitnessServer tts(LegacyOperationProfile::Kokoro);
    WitnessServer audio(LegacyOperationProfile::ThinkSound);
    WitnessServer image(LegacyOperationProfile::StableDiffusion);
    WitnessServer mesh(LegacyOperationProfile::Trellis);
    WitnessServer classification(LegacyOperationProfile::OnnxRuntime);
    httplib::DataSink sink;
    llm.dispatch_completion_operation(Op::ChatCompletion, {});
    llm.dispatch_completion_operation(Op::Completion, {});
    llm.dispatch_completion_operation(Op::Responses, {});
    operation_adapter<IEmbeddingsServer>(llm, Op::Embeddings, Delivery::Buffered, ModelType::LLM)->embeddings({});
    operation_adapter<IRerankingServer>(llm, Op::Reranking, Delivery::Buffered, ModelType::LLM)->reranking({});
    operation_adapter<ISlotsServer>(llm, Op::Slots, Delivery::Buffered, ModelType::LLM)->get_slots();
    operation_adapter<ISlotsServer>(llm, Op::SlotAction, Delivery::Buffered, ModelType::LLM)->slots_action(2, "save", {});
    operation_adapter<ITokenizerServer>(llm, Op::Tokenize, Delivery::Buffered, ModelType::LLM)->tokenize({});
    for (const auto operation : {Op::ChatCompletion, Op::Completion, Op::Responses}) llm.dispatch_streaming_operation(operation, "{}", sink);
    operation_adapter<IClassificationServer>(classification, Op::Classification, Delivery::Buffered, ModelType::CLASSIFICATION)->classify({});
    operation_adapter<ITranscriptionServer>(transcription, Op::Transcription, Delivery::Buffered, ModelType::TRANSCRIPTION)->audio_transcriptions({});
    operation_adapter<IStreamingTranscriptionServer>(transcription, Op::StreamingTranscription, Delivery::Realtime, ModelType::TRANSCRIPTION)->get_streaming_address();
    for (const auto delivery : {Delivery::Buffered, Delivery::Streaming}) {
        operation_adapter<ITextToSpeechServer>(tts, Op::TextToSpeech, delivery, ModelType::TTS)->audio_speech({}, sink);
    }
    operation_adapter<IAudioGenerationServer>(audio, Op::AudioGeneration, Delivery::Buffered, ModelType::AUDIO_GENERATION)->audio_generations({}, sink);
    operation_adapter<IImageServer>(image, Op::ImageGeneration, Delivery::Buffered, ModelType::IMAGE)->image_generations({});
    operation_adapter<IImageServer>(image, Op::ImageEdits, Delivery::Buffered, ModelType::IMAGE)->image_edits({});
    operation_adapter<IImageServer>(image, Op::ImageVariations, Delivery::Buffered, ModelType::IMAGE)->image_variations({});
    operation_adapter<IUpscaleServer>(image, Op::Upscale, Delivery::Buffered, ModelType::IMAGE)->upscale_via_cli("fixture", "fixture");
    operation_adapter<IModel3DServer>(mesh, Op::Model3DGeneration, Delivery::Buffered, ModelType::MESH)->model_3d_generations({}, sink);
    for (size_t op = 0; op < 18; ++op) {
        const int witnessed = llm.calls[op] + transcription.calls[op] + tts.calls[op] + audio.calls[op] +
            image.calls[op] + mesh.calls[op] + classification.calls[op];
        check(witnessed == (op <= 2 || op == 8 ? 2 : 1), "independent dispatch witness for operation " + std::to_string(op));
    }
    check(!operation_adapter<IEmbeddingsServer>(image, Op::Embeddings, Delivery::Buffered, ModelType::IMAGE), "inheritance alone does not enable embeddings");
    check(!operation_adapter<IImageServer>(llm, Op::ChatCompletion, Delivery::Buffered, ModelType::LLM), "typed adapter cannot serve a wrong operation");
    check(!operation_adapter<ITranscriptionServer>(transcription, Op::Transcription, Delivery::Streaming, ModelType::TRANSCRIPTION), "buffered transcription has no streaming alias");
    check(!operation_adapter<IStreamingTranscriptionServer>(image, Op::StreamingTranscription, Delivery::Realtime, ModelType::TRANSCRIPTION), "no native realtime capability from inheritance");
}

void check_validation() {
    WitnessServer server(LegacyOperationProfile::LlamaCpp);
    const OperationBinding embedding{{Op::Embeddings, Delivery::Buffered, k_all_operation_model_types,
                                      OperationProvenance::Implemented}, "embeddings", "json"};
    const auto adapters = server.operation_bindings()->adapters();
    auto rejects = [&](OperationAdapters candidate, std::vector<OperationBinding> bindings, bool compiled = false) {
        try { OperationBindings contract(candidate, std::move(bindings), compiled); }
        catch (const std::invalid_argument&) { return true; }
        return false;
    };
    auto missing = adapters;
    missing.embeddings = nullptr;
    check(rejects(missing, {embedding}), "positive declaration with null adapter rejected");
    auto invalid = embedding;
    invalid.handler = "unknown";
    check(rejects(adapters, {invalid}), "unknown handler rejected");
    invalid = embedding;
    invalid.handler = "chat.buffered";
    check(rejects(adapters, {invalid}), "wrong handler signature rejected");
    invalid = embedding;
    invalid.codec = "unknown";
    check(rejects(adapters, {invalid}), "unknown codec rejected");
    invalid.codec = "sse";
    check(rejects(adapters, {invalid}), "incompatible codec rejected");
    check(rejects(adapters, {embedding, embedding}), "duplicate declarations rejected");
    invalid = embedding;
    invalid.description.model_types = operation_model_type_mask(ModelType::LLM);
    check(rejects(adapters, {embedding, invalid}), "overlapping model masks rejected");
    invalid.description.model_types = 0;
    check(rejects(adapters, {invalid}), "empty model mask rejected");
    invalid.description.model_types = 1u << 14;
    check(rejects(adapters, {invalid}), "unknown model mask rejected");
    invalid = embedding;
    invalid.description.operation = static_cast<Op>(255);
    check(rejects(adapters, {invalid}), "invalid operation declaration rejected");
    invalid = embedding;
    invalid.description.delivery = Delivery::Streaming;
    check(rejects(adapters, {invalid}), "invalid delivery declaration rejected");
    invalid = {{Op::Responses, Delivery::Streaming, k_all_operation_model_types, OperationProvenance::LegacyForwarding}, "responses.streaming", "sse"};
    check(rejects(adapters, {invalid}), "external declarations cannot enable legacy forwarding");
    missing = adapters;
    missing.completion_implementations[0] = false;
    invalid = {{Op::ChatCompletion, Delivery::Buffered, k_all_operation_model_types, OperationProvenance::Implemented}, "chat.buffered", "json"};
    check(rejects(missing, {invalid}), "completion interface default is not implementation evidence");
    missing = adapters;
    missing.streaming_forwarder = nullptr;
    invalid = {{Op::Responses, Delivery::Streaming, k_all_operation_model_types, OperationProvenance::LegacyForwarding}, "responses.streaming", "sse"};
    check(rejects(missing, {invalid}, true), "legacy declaration requires an executable streaming adapter");
    missing = adapters;
    missing.responses = nullptr;
    invalid = {{Op::Responses, Delivery::Buffered, k_all_operation_model_types, OperationProvenance::Implemented}, "responses.buffered", "json"};
    check(rejects(missing, {invalid}), "responses require a direct virtual adapter");
    invalid = embedding;
    invalid.description.provenance = static_cast<OperationProvenance>(255);
    check(rejects(adapters, {invalid}), "unknown provenance rejected");
    for (const auto name : {"unknown", "", "embedding"}) {
        Op parsed = Op::Count;
        check(!parse_backend_operation(name, parsed) && parsed == Op::Count, "unknown JSON operation rejected");
    }
    const std::array<const char*, 18> names = {"chat_completion", "completion", "responses", "embeddings", "reranking",
        "classification", "transcription", "streaming_transcription", "text_to_speech", "audio_generation",
        "image_generation", "image_edits", "image_variations", "upscale", "model_3d_generation", "slots", "slot_action", "tokenize"};
    for (size_t i = 0; i < names.size(); ++i) {
        Op parsed = Op::Count;
        check(parse_backend_operation(names[i], parsed) && static_cast<size_t>(parsed) == i, "all JSON operation names pinned");
    }
}

void check_concurrent_queries() {
    WitnessServer server(LegacyOperationProfile::LlamaCpp);
    std::atomic<bool> ok{true};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&] {
            track_allocations = true;
            for (int i = 0; i < 10000; ++i) {
                if (!server.supports_operation(Op::ChatCompletion, Delivery::Streaming, ModelType::LLM) ||
                    server.supports_operation(Op::TextToSpeech, Delivery::Buffered, ModelType::TTS) ||
                    !operation_adapter<IEmbeddingsServer>(server, Op::Embeddings, Delivery::Buffered, ModelType::LLM)) {
                    ok.store(false);
                }
            }
            track_allocations = false;
        });
    }
    for (auto& worker : workers) worker.join();
    check(ok && query_allocations == 0 && server.probes == 0, "240000 concurrent queries allocate nothing and invoke no probes");
    check(std::all_of(server.calls.begin(), server.calls.end(), [](int count) { return count == 0; }), "query invokes no operation hook or transport");
}

void check_binding_boundaries() {
    WitnessServer server(LegacyOperationProfile::LlamaCpp);
    const auto adapters = server.operation_bindings()->adapters();
    const auto llm = operation_model_type_mask(ModelType::LLM);
    const auto tts = operation_model_type_mask(ModelType::TTS);
    const std::vector<OperationBinding> handlers = {
        {{Op::ChatCompletion, Delivery::Buffered, llm, OperationProvenance::Implemented}, "chat.buffered", "json"},
        {{Op::ChatCompletion, Delivery::Streaming, llm, OperationProvenance::Implemented}, "chat.streaming", "sse"},
        {{Op::Completion, Delivery::Buffered, llm, OperationProvenance::Implemented}, "completion.buffered", "json"},
        {{Op::Completion, Delivery::Streaming, llm, OperationProvenance::Implemented}, "completion.streaming", "sse"},
        {{Op::Responses, Delivery::Buffered, llm, OperationProvenance::Implemented}, "responses.buffered", "json"},
        {{Op::Responses, Delivery::Streaming, llm, OperationProvenance::Implemented}, "responses.streaming", "sse"},
        {{Op::Embeddings, Delivery::Buffered, llm, OperationProvenance::Implemented}, "embeddings", "json"},
        {{Op::Reranking, Delivery::Buffered, llm, OperationProvenance::Implemented}, "reranking", "json"},
        {{Op::Classification, Delivery::Buffered, llm, OperationProvenance::Implemented}, "classification", "json"},
        {{Op::Transcription, Delivery::Buffered, llm, OperationProvenance::Implemented}, "transcription", "multipart"},
        {{Op::StreamingTranscription, Delivery::Realtime, llm, OperationProvenance::Implemented}, "transcription.realtime", "jsonl"},
        {{Op::TextToSpeech, Delivery::Buffered, tts, OperationProvenance::Implemented}, "speech.buffered", "media"},
        {{Op::TextToSpeech, Delivery::Streaming, tts, OperationProvenance::Implemented}, "speech.streaming", "media"},
        {{Op::AudioGeneration, Delivery::Buffered, llm, OperationProvenance::Implemented}, "audio", "media"},
        {{Op::ImageGeneration, Delivery::Buffered, llm, OperationProvenance::Implemented}, "image", "json"},
        {{Op::ImageEdits, Delivery::Buffered, llm, OperationProvenance::Implemented}, "image.edits", "json"},
        {{Op::ImageVariations, Delivery::Buffered, llm, OperationProvenance::Implemented}, "image.variations", "json"},
        {{Op::Upscale, Delivery::Buffered, llm, OperationProvenance::Implemented}, "upscale", "base64"},
        {{Op::Model3DGeneration, Delivery::Buffered, llm, OperationProvenance::Implemented}, "mesh", "media"},
        {{Op::Slots, Delivery::Buffered, llm, OperationProvenance::Implemented}, "slots", "json"},
        {{Op::SlotAction, Delivery::Buffered, llm, OperationProvenance::Implemented}, "slots.action", "json"},
        {{Op::Tokenize, Delivery::Buffered, llm, OperationProvenance::Implemented}, "tokenize", "json"}
    };
    auto rejects = [&](OperationAdapters candidate, std::vector<OperationBinding> bindings,
                       bool compiled, std::vector<OperationDescription> rejections, const char* message) {
        try { OperationBindings contract(candidate, std::move(bindings), compiled, std::move(rejections)); }
        catch (const std::invalid_argument& error) { return std::string(error.what()) == message; }
        return false;
    };
    OperationBindings complete(adapters, handlers);
    check(!complete.compiled_compatibility() && complete.descriptions().size() == 22,
          "all 22 registered signatures accept independent executable bindings without compatibility");
    for (const auto& binding : handlers) {
        const auto mode = binding.description.operation == Op::TextToSpeech ? ModelType::TTS : ModelType::LLM;
        check(complete.supports({binding.description.operation, binding.description.delivery, mode}), "accepted handler cell is reachable");
        auto invalid = binding;
        invalid.codec = "unknown";
        check(rejects(adapters, {invalid}, false, {}, "Unknown or incompatible operation codec"), "each handler rejects an unknown codec");
        invalid = binding;
        invalid.handler = binding.description.operation == Op::Embeddings ? "reranking" : "embeddings";
        check(rejects(adapters, {invalid}, false, {}, "Operation handler signature mismatch"), "same-shaped methods cannot substitute for another operation");
        invalid = binding;
        invalid.description.provenance = OperationProvenance::LegacyForwarding;
        check(rejects(adapters, {invalid}, false, {}, "Legacy forwarding requires a compiled SSE compatibility binding"),
              "every external legacy-forwarding declaration rejects");
        auto missing = adapters;
        switch (binding.description.operation) {
            case Op::ChatCompletion:
            case Op::Completion:
            case Op::Responses:
                if (binding.description.delivery == Delivery::Streaming) missing.streaming_forwarder = nullptr;
                else missing.completion = nullptr;
                break;
            case Op::Embeddings: missing.embeddings = nullptr; break;
            case Op::Reranking: missing.reranking = nullptr; break;
            case Op::Classification: missing.classification = nullptr; break;
            case Op::Transcription: missing.transcription = nullptr; break;
            case Op::StreamingTranscription: missing.streaming_transcription = nullptr; break;
            case Op::TextToSpeech: missing.text_to_speech = nullptr; break;
            case Op::AudioGeneration: missing.audio_generation = nullptr; break;
            case Op::ImageGeneration:
            case Op::ImageEdits:
            case Op::ImageVariations: missing.image = nullptr; break;
            case Op::Upscale: missing.upscale = nullptr; break;
            case Op::Model3DGeneration: missing.model_3d = nullptr; break;
            case Op::Slots:
            case Op::SlotAction: missing.slots = nullptr; break;
            case Op::Tokenize: missing.tokenizer = nullptr; break;
            default: break;
        }
        check(rejects(missing, {binding}, false, {}, "Operation handler has no executable adapter"), "each registered signature requires its adapter");
    }
    auto embedding = handlers[6];
    auto disjoint = embedding;
    disjoint.description.model_types = tts;
    OperationBindings split(adapters, {embedding, disjoint});
    check(split.supports({Op::Embeddings, Delivery::Buffered, ModelType::LLM}) &&
          split.supports({Op::Embeddings, Delivery::Buffered, ModelType::TTS}) &&
          !split.supports({Op::Embeddings, Delivery::Buffered, ModelType::EMBEDDING}) && split.descriptions().size() == 2,
          "disjoint declarations merge only their declared cells");
    check(!complete.has_rejection({Op::Embeddings, Delivery::Buffered, ModelType::LLM}), "noncompatibility table has no implicit rejection");
    OperationBindings with_rejection(adapters, {embedding}, true, {disjoint.description});
    check(with_rejection.has_rejection({Op::Embeddings, Delivery::Buffered, ModelType::TTS}) &&
          !with_rejection.supports({Op::Embeddings, Delivery::Buffered, ModelType::TTS}) &&
          !with_rejection.has_rejection({Op::Embeddings, Delivery::Buffered, ModelType::LLM}), "disjoint compiled rejection remains separate from support");
    check(rejects(adapters, {}, false, {embedding.description}, "Rejection requires a compiled adapter"), "external rejection declarations reject");
    auto missing = adapters;
    missing.embeddings = nullptr;
    check(rejects(missing, {}, true, {embedding.description}, "Rejection requires a compiled adapter"), "compiled rejection needs an executable adapter");
    check(rejects(adapters, {embedding}, true, {embedding.description}, "Overlapping operation rejection"), "support and rejection cannot overlap");
    check(rejects(adapters, {}, true, {embedding.description, embedding.description}, "Overlapping operation rejection"), "duplicate rejection declarations reject");
    for (const auto operation : {Op::Count, static_cast<Op>(255)}) {
        auto invalid = embedding;
        invalid.description.operation = operation;
        check(rejects(adapters, {invalid}, false, {}, "Invalid operation, delivery or model types"), "operation sentinel and invalid enum reject");
    }
    for (const auto delivery : {Delivery::Count, Delivery::Realtime, static_cast<Delivery>(255)}) {
        auto invalid = embedding;
        invalid.description.delivery = delivery;
        check(rejects(adapters, {invalid}, false, {}, "Invalid operation, delivery or model types"), "delivery sentinel, realtime alias and invalid enum reject");
    }
    for (const ModelTypeMask mask : {static_cast<ModelTypeMask>(0), static_cast<ModelTypeMask>(llm | (1u << 15))}) {
        auto invalid = embedding;
        invalid.description.model_types = mask;
        check(rejects(adapters, {invalid}, false, {}, "Invalid operation, delivery or model types"), "empty and mixed valid-invalid masks reject");
    }
    auto legacy = handlers[5];
    legacy.description.provenance = OperationProvenance::LegacyForwarding;
    missing = adapters;
    missing.completion_implementations[2] = false;
    OperationBindings compatibility(missing, {legacy}, true);
    check(compatibility.supports({Op::Responses, Delivery::Streaming, ModelType::LLM}), "compiled legacy forwarding accepts the existing transport without buffered responses");
    for (auto invalid : {handlers[4], handlers[12]}) {
        invalid.description.provenance = OperationProvenance::LegacyForwarding;
        check(rejects(adapters, {invalid}, true, {}, "Legacy forwarding requires a compiled SSE compatibility binding"), "compiled compatibility cannot enable buffered or media legacy forwarding");
    }
    for (size_t i = 0; i < 6; ++i) {
        missing = adapters;
        missing.completion_implementations[i / 2] = false;
        check(rejects(missing, {handlers[i]}, false, {}, "Operation handler has no executable adapter"), "each implemented completion delivery requires implementation evidence");
    }
}

void check_queries_with_locked_state() {
    WitnessServer server(LegacyOperationProfile::LlamaCpp);
    auto state_lock = server.hold_state_mutex();
    auto process_lock = server.hold_process_mutex();
    auto worker = std::async(std::launch::async, [&] {
        bool ok = true;
        track_allocations = true;
        for (int i = 0; i < 10000; ++i) {
            ok &= server.supports_operation(Op::Embeddings, Delivery::Buffered, ModelType::LLM);
            ok &= !server.supports_operation(Op::TextToSpeech, Delivery::Streaming, ModelType::TTS);
            ok &= operation_adapter<IEmbeddingsServer>(server, Op::Embeddings, Delivery::Buffered, ModelType::LLM) != nullptr;
        }
        track_allocations = false;
        return ok;
    });
    const bool completed_while_locked = worker.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    process_lock.unlock();
    state_lock.unlock();
    const bool correct = worker.get();
    check(completed_while_locked && correct, "30000 queries finish while state and process mutexes are held; establishes no blocking on either mutex");
    check(query_allocations == 0 && server.probes == 0 &&
          std::all_of(server.calls.begin(), server.calls.end(), [](int count) { return count == 0; }),
          "locked-state queries allocate nothing and invoke no lifecycle, availability, operation or transport probe");
}

void check_removed_helpers() {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "src" / "cpp";
    const std::array<std::string, 6> removed = {"save_audio_to_temp", "cleanup_temp_file", "validate_audio_file",
        "build_transcription_request", "forward_multipart_audio_request", "RyzenAIServer::is_available"};
    std::array<size_t, 6> references{};
    size_t whisper_temp_references = 0;
    size_t ryzenai_declarations = 0;
    size_t files = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || (entry.path().extension() != ".cpp" && entry.path().extension() != ".h")) continue;
        std::ifstream input(entry.path());
        if (!input) throw std::runtime_error("Cannot read cleanup fixture source");
        const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        ++files;
        for (size_t i = 0; i < removed.size(); ++i) references[i] += source.find(removed[i]) != std::string::npos;
        if (entry.path().parent_path().filename() == "whispercpp") {
            whisper_temp_references += source.find("temp_dir_") != std::string::npos;
        }
        if (entry.path().filename() == "ryzenai_server.h") {
            ryzenai_declarations += source.find("is_available") != std::string::npos;
        }
    }
    check(files > 0, "cleanup audit reads the production source tree");
    for (size_t i = 0; i < removed.size(); ++i) check(references[i] == 0, "removed helper has no production references: " + removed[i]);
    check(whisper_temp_references == 0 && ryzenai_declarations == 0, "Whisper temp-directory member and RyzenAI availability declaration remain absent");
}

}

void* operator new(std::size_t size) {
    if (track_allocations) ++query_allocations;
    if (auto* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

int main() {
    try {
        check_complete_matrix();
        check_witnesses();
        check_validation();
        check_binding_boundaries();
        check_concurrent_queries();
        check_queries_with_locked_state();
        check_removed_helpers();
    } catch (const std::exception& error) {
        check(false, std::string("unexpected exception: ") + error.what());
    }
    std::printf("%zu checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
