#include "lemon/backends/backend_registry.h"
#include "lemon/backends/hrx/hrx_server.h"
#include "lemon/backends/llamacpp/llamacpp_server.h"
#include "lemon/wrapped_server.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
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

} // namespace

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
        check_concurrent_queries();
    } catch (const std::exception& error) {
        check(false, std::string("unexpected exception: ") + error.what());
    }
    std::printf("%zu checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
