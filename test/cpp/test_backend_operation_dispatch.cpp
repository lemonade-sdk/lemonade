#include "lemon/backends/cloud/cloud_server.h"
#include "lemon/backends/fastflowlm/fastflowlm_server.h"
#include "lemon/backends/hrx/hrx_server.h"
#include "lemon/backends/kokoro/kokoro_server.h"
#include "lemon/backends/llamacpp/llamacpp_server.h"
#include "lemon/backends/moonshine/moonshine_server.h"
#include "lemon/backends/openmoss/openmoss_server.h"
#include "lemon/backends/sdcpp/sdcpp_server.h"
#include "lemon/backends/thenoise/thenoise_server.h"
#include "lemon/backends/whispercpp/whispercpp_server.h"
#include "lemon/cloud_provider_registry.h"
#include "lemon/router.h"
#include "lemon/wrapped_server.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace lemon {

struct RoutingHelperTestHook {
    static std::unique_ptr<WrappedServer> create_backend(Router& router, const ModelInfo& model) {
        return router.create_backend_server(model);
    }
};

}

namespace {

using namespace lemon;
using Op = BackendOperation;
using Delivery = OperationDelivery;
int failures = 0;
int checks = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

struct Capture {
    std::string bytes;
    int done = 0;
    httplib::DataSink sink;
    Capture() {
        sink.write = [this](const char* data, size_t size) { bytes.append(data, size); return true; };
        sink.is_writable = [] { return true; };
        sink.done = [this] { ++done; };
        sink.done_with_trailer = [this](const httplib::Headers&) { ++done; };
    }
};

class FakeHttpBackend {
public:
    FakeHttpBackend() {
        server_.set_pre_routing_handler([this](const httplib::Request&, httplib::Response&) {
            ++requests_;
            return httplib::Server::HandlerResponse::Unhandled;
        });
        auto post = [this](const httplib::Request& request, httplib::Response& response) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                paths_.push_back(request.path);
                bodies_.push_back(request.body);
                form_ = request.form;
            }
            const auto body = json::parse(request.body, nullptr, false);
            if (request.path == "/v1/audio/speech") {
                response.set_content("fixture-pcm", "audio/pcm");
            } else if (body.is_object() && body.value("stream", false)) {
                response.set_content("data: {\"choices\":[{\"delta\":{\"content\":\"fixture\"}}]}\n\ndata: [DONE]\n\n", "text/event-stream");
            } else if (request.path == "/inference") {
                response.set_content("{\"text\":\"fixture transcription\"}", "application/json");
            } else {
                response.set_content("{\"model\":\"upstream\",\"witness\":true}", "application/json");
            }
        };
        for (const auto path : {"/v1/chat/completions", "/v1/completions", "/v1/responses", "/v1/embeddings", "/v1/rerank", "/tokenize",
                                "/slots/2", "/inference", "/v1/audio/transcriptions", "/v1/audio/speech"}) {
            server_.Post(path, post);
        }
        server_.Get("/slots", [](const httplib::Request&, httplib::Response& response) {
            response.set_content("{\"slots\":[2]}", "application/json");
        });
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) throw std::runtime_error("Could not bind CPU-only fake HTTP backend");
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }
    ~FakeHttpBackend() { server_.stop(); thread_.join(); }
    int port() const { return port_; }
    size_t request_count() const { return requests_.load(); }
    json last_body() {
        std::lock_guard<std::mutex> lock(mutex_);
        return json::parse(bodies_.back(), nullptr, false);
    }
    std::string last_path() {
        std::lock_guard<std::mutex> lock(mutex_);
        return paths_.back();
    }
    std::string field(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (form_.has_file(name)) return form_.get_file(name).content;
        if (form_.has_field(name)) return form_.get_field(name);
        return "";
    }
    std::string filename() {
        std::lock_guard<std::mutex> lock(mutex_);
        return form_.get_file("file").filename;
    }
    std::string content_type() {
        std::lock_guard<std::mutex> lock(mutex_);
        return form_.get_file("file").content_type;
    }

private:
    httplib::Server server_;
    int port_ = 0;
    std::thread thread_;
    std::mutex mutex_;
    std::vector<std::string> paths_;
    std::vector<std::string> bodies_;
    httplib::MultipartFormData form_;
    std::atomic<size_t> requests_{0};
};

class RunningFakeProcess : public ServerProcess {
public:
    RunningFakeProcess() : ServerProcess(ProcessOutput{}) {}
    std::string start(const ServerCommand&) override { return "127.0.0.1"; }
    void stop() override {}
    bool running() const override { return true; }
};

template<class Server>
class LocalServer : public Server {
public:
    LocalServer(int port, ModelType mode = ModelType::LLM)
        : Server("error", nullptr, nullptr) {
        this->port_ = port;
        this->set_model_metadata("client-fixture", "upstream-fixture", mode, DEVICE_CPU, RecipeOptions{});
        this->set_state(ModelState::READY);
    }
    bool is_backend_alive() const override { return true; }
    // The factory fallback keeps its concrete type; attach a fake process without loading a model.
    static void attach_transport(WrappedServer& server, int port, ModelType mode = ModelType::LLM) {
        server.*(&LocalServer::port_) = port;
        server.*(&LocalServer::process_) = std::make_unique<RunningFakeProcess>();
        server.set_model_metadata("client-fixture", "upstream-fixture", mode, DEVICE_CPU, RecipeOptions{});
        server.set_state(ModelState::READY);
    }
};

std::unique_ptr<WrappedServer> configured(std::unique_ptr<WrappedServer> server, int port = 0, ModelType mode = ModelType::LLM) {
    if (port) LocalServer<backends::LlamaCppServer>::attach_transport(*server, port, mode);
    return server;
}

void check_llama_dispatch(FakeHttpBackend& upstream) {
    LocalServer<backends::LlamaCppServer> server(upstream.port());
    const json request = {{"model", "client-fixture"}, {"max_completion_tokens", 7}};
    const std::vector<std::pair<Op, std::string>> paths = {
        {Op::ChatCompletion, "/v1/chat/completions"}, {Op::Completion, "/v1/completions"}, {Op::Responses, "/v1/responses"}
    };
    for (const auto& [operation, path] : paths) {
        const auto response = server.dispatch_completion_operation(operation, request);
        check(response.value("model", "") == "client-fixture" && upstream.last_path() == path, "buffered completion preserves route and model normalization");
        const auto body = upstream.last_body();
        check((operation == Op::Responses && !body.contains("max_tokens")) ||
              (operation != Op::Responses && body.value("max_tokens", 0) == 7), "legacy token alias remains operation-specific");
        Capture capture;
        server.dispatch_streaming_operation(operation, json{{"model", "client-fixture"}, {"stream", true}}.dump(), capture.sink);
        check(upstream.last_path() == path && capture.bytes == "data: {\"choices\":[{\"delta\":{\"content\":\"fixture\"}}]}\n\ndata: [DONE]\n\n",
              "streaming completion preserves route and exact SSE bytes");
    }
    operation_adapter<IEmbeddingsServer>(server, Op::Embeddings, Delivery::Buffered, ModelType::LLM)->embeddings(request);
    check(upstream.last_path() == "/v1/embeddings" && upstream.last_body() == request, "embeddings reaches fake HTTP transport unchanged");
    operation_adapter<IRerankingServer>(server, Op::Reranking, Delivery::Buffered, ModelType::LLM)->reranking(request);
    check(upstream.last_path() == "/v1/rerank", "reranking uses the existing path");
    check(operation_adapter<ISlotsServer>(server, Op::Slots, Delivery::Buffered, ModelType::LLM)->get_slots() == json{{"slots", {2}}}, "slots retains GET dispatch");
    operation_adapter<ISlotsServer>(server, Op::SlotAction, Delivery::Buffered, ModelType::LLM)->slots_action(2, "save", request);
    check(upstream.last_path() == "/slots/2", "slot action retains POST dispatch");
    operation_adapter<ITokenizerServer>(server, Op::Tokenize, Delivery::Buffered, ModelType::LLM)->tokenize(request);
    check(upstream.last_path() == "/tokenize", "tokenize retains POST dispatch");
    auto hrx = configured(backends::hrx::create(backends::BackendContext{"error"}), upstream.port());
    operation_adapter<IEmbeddingsServer>(*hrx, Op::Embeddings, Delivery::Buffered, ModelType::LLM)->embeddings(request);
    check(upstream.last_path() == "/v1/embeddings", "HRX keeps callable embeddings despite advertised mask");
    operation_adapter<IRerankingServer>(*hrx, Op::Reranking, Delivery::Buffered, ModelType::LLM)->reranking(request);
    check(upstream.last_path() == "/v1/rerank", "HRX keeps callable reranking despite advertised mask");
}

json unsupported(const std::string& message, bool code = false) {
    json error = {{"error", {{"message", message}, {"type", "unsupported_operation"}}}};
    if (code) error["error"]["code"] = "model_not_applicable";
    return error;
}

class FallbackSpeechServer : public WrappedServer, public ITextToSpeechServer {
public:
    FallbackSpeechServer() : WrappedServer("fixture", "error") {
        const auto tts = operation_model_type_mask(ModelType::TTS);
        bind_operations(OperationBindings(operation_adapters_for(*this), {
            {{Op::TextToSpeech, Delivery::Buffered, tts, OperationProvenance::Implemented}, "speech.buffered", "media"},
            {{Op::TextToSpeech, Delivery::Streaming, tts, OperationProvenance::Implemented}, "speech.streaming", "media"}}, true));
    }
    void load(const std::string&, const ModelInfo&, const RecipeOptions&, bool) override {}
    void audio_speech(const json&, httplib::DataSink&) override {}
    std::vector<std::string> supported_audio_formats() const override { return {"mp3", "wav", "opus", "pcm"}; }
    std::vector<std::string> supported_streaming_audio_formats() const override { return {}; }
    AudioFormatMetadata audio_format_metadata(const std::string& format) const override {
        return format == "pcm" ? AudioFormatMetadata{"audio/pcm", {{"X-Sample-Rate", "24000"}, {"X-Channels", "1"}}}
                               : AudioFormatMetadata{};
    }
};

void check_rejections() {
    auto kokoro = backends::kokoro::create(backends::BackendContext{"error"});
    backends::WhisperServer whisper("error", nullptr, nullptr);
    backends::MoonshineServer moonshine("error", nullptr, nullptr);
    backends::SDServer sd("error", nullptr, nullptr);
    backends::TheNoiseServer noise("error", nullptr, nullptr);
    for (const auto operation : {Op::ChatCompletion, Op::Completion, Op::Responses}) {
        const std::string label = operation == Op::ChatCompletion ? "chat completion" : operation == Op::Completion ? "text completion" : "responses";
        check(kokoro->dispatch_completion_operation(operation, {}) == unsupported("Kokoro does not support text completion. Use audio speech endpoints instead.", true), "exact Kokoro exceptional rejection");
        check(whisper.dispatch_completion_operation(operation, {}) == unsupported("Whisper models do not support " + label + ". Use audio transcription endpoints instead.", true), "exact Whisper exceptional rejection");
        check(moonshine.dispatch_completion_operation(operation, {}) == unsupported("Moonshine models do not support " + label + ". Use audio transcription endpoints instead.", true), "exact Moonshine exceptional rejection");
        const auto title = operation == Op::ChatCompletion ? "Chat completion" : operation == Op::Completion ? "Text completion" : "Responses";
        check(sd.dispatch_completion_operation(operation, {}) == unsupported(std::string(title) + " not supported by sd-cpp (image generation model)"), "SD rejection retains absence of code field");
        check(noise.dispatch_completion_operation(operation, {}) == unsupported(std::string(title) + " not supported by thenoise (image generation model)"), "TheNoise rejection retains absence of code field");
    }
    check(!operation_adapter<IImageServer>(noise, Op::ImageVariations, Delivery::Buffered, ModelType::IMAGE), "TheNoise variations are not a positive binding");
    auto* rejected = operation_rejection_adapter<IImageServer>(noise, Op::ImageVariations, Delivery::Buffered, ModelType::IMAGE);
    check(rejected && rejected->image_variations({}) == unsupported("Image variations not supported by thenoise"), "TheNoise rejection adapter preserves exact payload");
    backends::OpenMossServer openmoss("error", nullptr, nullptr);
    check(!operation_adapter<ITextToSpeechServer>(openmoss, Op::TextToSpeech, Delivery::Buffered, ModelType::AUDIO_GENERATION), "OpenMoss audio mode cannot dispatch speech");
    check(!operation_adapter<IAudioGenerationServer>(openmoss, Op::AudioGeneration, Delivery::Buffered, ModelType::TTS), "OpenMoss speech mode cannot dispatch audio generation");
    check(operation_metadata_adapter<ITextToSpeechServer>(kokoro.get(), Op::TextToSpeech, Delivery::Buffered)->supported_audio_formats() ==
          std::vector<std::string>({"mp3", "wav", "opus", "pcm"}), "buffered Kokoro formats preserve pre-mode-check metadata");
    check(operation_metadata_adapter<ITextToSpeechServer>(kokoro.get(), Op::TextToSpeech, Delivery::Streaming)->supported_streaming_audio_formats() ==
          std::vector<std::string>({"pcm"}), "Kokoro streaming remains PCM-only");
    const auto pcm = openmoss.audio_format_metadata("pcm");
    check(pcm.content_type == "audio/pcm" && pcm.headers.empty(), "pre-load PCM MIME is exact without invented sample rate or channels");
    FallbackSpeechServer fallback;
    auto* metadata = operation_metadata_adapter<ITextToSpeechServer>(&fallback, Op::TextToSpeech, Delivery::Streaming);
    check(metadata && metadata->supported_streaming_audio_formats().empty() &&
          metadata->supported_audio_formats() == std::vector<std::string>({"mp3", "wav", "opus", "pcm"}),
          "empty streaming format list retains the buffered-format fallback contract");
    const auto raw_pcm = metadata->audio_format_metadata("pcm");
    check(raw_pcm.content_type == "audio/pcm" && raw_pcm.headers == std::map<std::string, std::string>({{"X-Sample-Rate", "24000"}, {"X-Channels", "1"}}),
          "typed metadata adapter preserves exact PCM rate and channel headers");
    check(!operation_metadata_adapter<ITextToSpeechServer>(&fallback, Op::TextToSpeech, Delivery::Realtime),
          "invalid metadata delivery cannot use compatibility fallback");
    const auto formats = operation_metadata_adapter<ITextToSpeechServer>(kokoro.get(), Op::TextToSpeech, Delivery::Streaming)->supported_streaming_audio_formats();
    check(std::find(formats.begin(), formats.end(), "mp3") == formats.end(), "explicit MP3 is unavailable in Kokoro streaming formats");
}

void check_flm_dispatch(FakeHttpBackend& upstream) {
    LocalServer<backends::FastFlowLMServer> server(upstream.port());
    check(!server.supports_operation(Op::Transcription, Delivery::Buffered, ModelType::LLM), "FLM rejects transcription in chat mode");
    auto* transcription_rejection = operation_rejection_adapter<ITranscriptionServer>(server, Op::Transcription, Delivery::Buffered, ModelType::LLM);
    const auto before_transcription = upstream.request_count();
    check(transcription_rejection && transcription_rejection->audio_transcriptions({}) == unsupported("Audio transcription not supported by FLM llm model"), "FLM transcription rejection retains model-specific body");
    check(upstream.request_count() == before_transcription, "FLM transcription mode rejection never reaches transport");
    check(server.supports_operation(Op::Embeddings, Delivery::Buffered, ModelType::LLM), "FLM embeddings remain available in chat mode");
    operation_adapter<IEmbeddingsServer>(server, Op::Embeddings, Delivery::Buffered, ModelType::LLM)->embeddings({});
    check(upstream.last_path() == "/v1/embeddings", "FLM embeddings in chat mode reach transport");
    const auto before_responses = upstream.request_count();
    check(server.dispatch_completion_operation(Op::Responses, {}) == unsupported("Responses API not supported by flm"), "FLM buffered responses retain rejection");
    check(upstream.request_count() == before_responses, "FLM buffered responses never reach transport");
    Capture legacy;
    server.dispatch_streaming_operation(Op::Responses, "{\"stream\":true}", legacy.sink);
    check(upstream.last_path() == "/v1/responses" && upstream.last_body().value("model", "") == "upstream-fixture" &&
          legacy.bytes.find("fixture") != std::string::npos && legacy.bytes.find("[DONE]") != std::string::npos,
          "FLM legacy streaming responses reach fake upstream with checkpoint rewrite");
    for (const auto mode : {ModelType::EMBEDDING, ModelType::TRANSCRIPTION}) {
        LocalServer<backends::FastFlowLMServer> other(upstream.port(), mode);
        for (const auto operation : {Op::ChatCompletion, Op::Completion, Op::Responses}) {
            const auto before = upstream.request_count();
            Capture rejection;
            other.dispatch_streaming_operation(operation, "{}", rejection.sink);
            check(!other.supports_operation(operation, Delivery::Streaming, mode) &&
                  rejection.bytes == "data: {\"error\":{\"message\":\"Streaming not supported for FLM " + model_type_to_string(mode) + " model\",\"type\":\"unsupported_operation\"}}\n\n" &&
                  rejection.done == 1, "exact FLM mode-specific SSE rejection and termination");
            check(upstream.request_count() == before, "FLM rejected streaming operation never reaches transport");
        }
        const auto before = upstream.request_count();
        check(other.dispatch_completion_operation(Op::ChatCompletion, {}) == unsupported("Chat completion not supported by FLM " + model_type_to_string(mode) + " model"), "FLM buffered chat preserves mode rejection");
        check(other.dispatch_completion_operation(Op::Completion, {}) == unsupported("Text completion not supported by FLM " + model_type_to_string(mode) + " model"), "FLM buffered completion preserves mode rejection");
        check(upstream.request_count() == before, "FLM rejected buffered completion operations never reach transport");
    }
    LocalServer<backends::FastFlowLMServer> asr(upstream.port(), ModelType::TRANSCRIPTION);
    auto* embeddings_rejection = operation_rejection_adapter<IEmbeddingsServer>(asr, Op::Embeddings, Delivery::Buffered, ModelType::TRANSCRIPTION);
    const auto before_embeddings = upstream.request_count();
    check(embeddings_rejection && embeddings_rejection->embeddings({}) == unsupported("Embeddings not supported by FLM transcription model"), "FLM embeddings preserve ASR rejection body");
    check(upstream.request_count() == before_embeddings, "FLM rejected ASR embeddings never reach transport");
}

void check_whisper_wire(FakeHttpBackend& upstream) {
    LocalServer<backends::WhisperServer> whisper(upstream.port(), ModelType::TRANSCRIPTION);
    auto* adapter = operation_adapter<ITranscriptionServer>(whisper, Op::Transcription, Delivery::Buffered, ModelType::TRANSCRIPTION);
    const auto response = adapter->audio_transcriptions({{"file_data", "fixture-audio"}, {"filename", "fixture.mp3"}, {"language", "pl"}});
    check(response == json{{"text", "fixture transcription"}} && upstream.last_path() == "/inference" &&
          upstream.field("file") == "fixture-audio" && upstream.field("language") == "pl", "Whisper live in-memory multipart path survives and sends exact fields");
    const std::string binary_audio("RIFF\0\xff\r\nWAVE", 12);
    const std::vector<std::pair<std::string, std::string>> formats = {
        {"mp3", "audio/mpeg"}, {"wav", "audio/wav"}, {"m4a", "audio/mp4"},
        {"ogg", "audio/ogg"}, {"flac", "audio/flac"}, {"webm", "audio/webm"}, {"unknown", "audio/wav"}
    };
    for (const auto& [extension, mime] : formats) {
        const auto before = upstream.request_count();
        const auto result = adapter->audio_transcriptions({{"file_data", binary_audio}, {"filename", "uploads/fixture." + extension}});
        check(result == json{{"text", "fixture transcription"}} && upstream.request_count() == before + 1 &&
              upstream.field("file") == binary_audio && upstream.filename() == "fixture." + extension && upstream.content_type() == mime,
              "Whisper preserves binary bytes, basename and MIME for " + extension);
        check(upstream.field("language") == "auto" && upstream.field("response_format") == "json" &&
              upstream.field("temperature") == "0.0" && upstream.field("translate").empty(), "Whisper live wire preserves omitted defaults without translation");
    }
    adapter->audio_transcriptions({{"file_data", binary_audio}, {"language", "de"}, {"prompt", "Noch einmal"},
                                   {"response_format", "verbose_json"}, {"temperature", 0.25}});
    check(upstream.filename() == "audio.wav" && upstream.field("language") == "de" && upstream.field("prompt") == "Noch einmal" &&
          upstream.field("response_format") == "verbose_json" && upstream.field("temperature") == "0.250000",
          "Whisper live multipart retains explicit parameters and default filename");
    for (const auto request : {json::object(), json{{"file_data", ""}}}) {
        const auto before = upstream.request_count();
        const auto message = request.empty() ? "Transcription failed: Missing 'file_data' in request" : "Transcription failed: Empty audio data";
        check(adapter->audio_transcriptions(request) == json{{"error", {{"message", message}, {"type", "audio_processing_error"}}}},
              "Whisper invalid input retains exact error payload");
        check(upstream.request_count() == before, "Whisper invalid input never reaches multipart transport");
    }
}

void check_flm_and_moonshine_wire(FakeHttpBackend& upstream) {
    LocalServer<backends::FastFlowLMServer> flm(upstream.port(), ModelType::TRANSCRIPTION);
    LocalServer<backends::MoonshineServer> moonshine(upstream.port(), ModelType::TRANSCRIPTION);
    const std::string binary_audio("RIFF\0\xff\r\nWAVE", 12);
    const std::vector<std::pair<std::string, std::string>> formats = {
        {"fixture.mp3", "audio/mpeg"}, {"fixture.wav", "audio/wav"}, {"fixture.m4a", "audio/mp4"},
        {"fixture.ogg", "audio/ogg"}, {"fixture.flac", "audio/flac"}, {"fixture.webm", "audio/webm"},
        {"fixture.unknown", "audio/wav"}, {"fixture.MP3", "audio/wav"}, {"fixture.FlAc", "audio/wav"},
        {"fixture", "audio/wav"}
    };
    for (auto* server : std::vector<WrappedServer*>{&flm, &moonshine}) {
        auto* adapter = operation_adapter<ITranscriptionServer>(*server, Op::Transcription, Delivery::Buffered, ModelType::TRANSCRIPTION);
        check(adapter != nullptr, "FLM/Moonshine transcription adapter available on CPU fixture");
        if (!adapter) continue;
        const bool is_flm = server == &flm;
        for (const auto& [filename, mime] : formats) {
            const auto before = upstream.request_count();
            const auto response = adapter->audio_transcriptions({{"file_data", binary_audio}, {"filename", "uploads/" + filename}});
            const json expected = is_flm ? json{{"model", "upstream"}, {"witness", true}} : json{{"text", "fixture transcription"}};
            check(response == expected && upstream.request_count() == before + 1 &&
                  upstream.last_path() == (is_flm ? "/v1/audio/transcriptions" : "/inference") &&
                  upstream.field("file") == binary_audio && upstream.filename() == filename && upstream.content_type() == mime,
                  std::string(is_flm ? "FLM" : "Moonshine") + " preserves multipart MIME, basename and binary bytes for " + filename);
            if (is_flm) {
                check(upstream.field("model") == "upstream-fixture", "FLM retains required upstream model multipart field");
            } else {
                check(upstream.field("response_format") == "json", "Moonshine retains default response format multipart field");
            }
        }
    }
}

void check_cloud_wire(FakeHttpBackend& upstream) {
    CloudProviderRegistry registry;
    registry.install("operation-contract-fixture", "http://127.0.0.1:" + std::to_string(upstream.port()) + "/v1");
    registry.set_allow_insecure_http("operation-contract-fixture", true);
    registry.set_runtime_key("operation-contract-fixture", "fixture-key");
    backends::CloudServer cloud("operation-contract-fixture", "error", nullptr, nullptr, &registry);
    check(cloud.supports_operation(Op::Responses, Delivery::Streaming, ModelType::LLM), "Cloud support query before credential-backed setup");
    ModelInfo model;
    model.cloud_provider = "operation-contract-fixture";
    model.checkpoints["main"] = "fixture-upstream";
    cloud.load("fixture", model, RecipeOptions{}, false);
    const auto before_responses = upstream.request_count();
    check(cloud.dispatch_completion_operation(Op::Responses, {}) == unsupported("Responses API not supported by cloud (operation-contract-fixture)"), "Cloud buffered responses still reject");
    check(upstream.request_count() == before_responses, "Cloud buffered responses never reach transport");
    Capture capture;
    cloud.dispatch_streaming_operation(Op::Responses, "{\"stream\":true}", capture.sink);
    check(upstream.last_path() == "/v1/responses" && upstream.last_body().value("model", "") == "fixture-upstream" &&
          capture.bytes.find("fixture") != std::string::npos && capture.bytes.find("[DONE]") != std::string::npos,
          "Cloud legacy streaming responses reach only fake loopback upstream");
    cloud.unload();
    check(cloud.supports_operation(Op::Responses, Delivery::Streaming, ModelType::LLM), "Cloud unload does not mutate operation support");
}

class UnboundServer : public WrappedServer {
public:
    UnboundServer() : WrappedServer("fixture", "error") {}
    void load(const std::string&, const ModelInfo&, const RecipeOptions&, bool) override {}
    int transfers = 0;
    void forward_streaming_request(const std::string&, const std::string&, httplib::DataSink&,
                                   bool, long, TelemetryCallback) override { ++transfers; }
};

void check_default_rejection_framing() {
    UnboundServer server;
    for (const auto operation : {Op::ChatCompletion, Op::Completion, Op::Responses}) {
        const auto label = operation == Op::ChatCompletion ? "chat completion" : operation == Op::Completion ? "text completion" : "responses";
        check(server.dispatch_completion_operation(operation, {}) == unsupported(std::string("fixture does not support ") + label +
              ". Use the appropriate endpoint for this model type instead.", true), "exact base buffered rejection payload");
        Capture rejection;
        server.dispatch_streaming_operation(operation, "{}", rejection.sink);
        check(rejection.bytes == "data: {\"error\":{\"code\":\"model_not_applicable\",\"message\":\"fixture does not support streaming completion. Use the appropriate endpoint for this model type instead.\",\"type\":\"unsupported_operation\"}}\n\n" &&
              rejection.done == 1 && server.transfers == 0, "base rejection preserves exact SSE frame, terminates once and invokes no transfer");
    }
}

void check_rejection_transport_isolation(FakeHttpBackend& upstream) {
    auto kokoro = configured(backends::kokoro::create(backends::BackendContext{"error"}), upstream.port(), ModelType::TTS);
    LocalServer<backends::WhisperServer> whisper(upstream.port(), ModelType::TRANSCRIPTION);
    LocalServer<backends::MoonshineServer> moonshine(upstream.port(), ModelType::TRANSCRIPTION);
    LocalServer<backends::SDServer> sd(upstream.port(), ModelType::IMAGE);
    LocalServer<backends::TheNoiseServer> noise(upstream.port(), ModelType::IMAGE);
    for (auto* server : std::vector<WrappedServer*>{kokoro.get(), &whisper, &moonshine, &sd, &noise}) {
        const auto before = upstream.request_count();
        for (const auto operation : {Op::ChatCompletion, Op::Completion, Op::Responses}) {
            check(!server->supports_operation(operation, Delivery::Buffered, server->get_model_type()) &&
                  server->dispatch_completion_operation(operation, {}).contains("error"), "exceptional buffered rejection remains selected on a transport-ready instance");
        }
        check(upstream.request_count() == before, "exceptional buffered rejections never reach fake HTTP transport");
    }
    const auto before = upstream.request_count();
    auto* rejection = operation_rejection_adapter<IImageServer>(noise, Op::ImageVariations, Delivery::Buffered, ModelType::IMAGE);
    check(rejection && rejection->image_variations({}) == unsupported("Image variations not supported by thenoise") &&
          upstream.request_count() == before, "TheNoise rejection adapter never reaches transport on a ready image server");
}

void check_tts_delivery_wire(FakeHttpBackend& upstream) {
    auto server = configured(backends::kokoro::create(backends::BackendContext{"error"}), upstream.port(), ModelType::TTS);
    auto& kokoro = *server;
    for (const auto delivery : {Delivery::Buffered, Delivery::Streaming}) {
        auto* adapter = operation_adapter<ITextToSpeechServer>(kokoro, Op::TextToSpeech, delivery, ModelType::TTS);
        check(adapter != nullptr, "Kokoro binds both public speech deliveries independently");
        if (!adapter) continue;
        const auto formats = delivery == Delivery::Buffered ? adapter->supported_audio_formats() : adapter->supported_streaming_audio_formats();
        for (const auto& format : formats) {
            Capture audio;
            json request = {{"model", "client-fixture"}, {"input", "fixture"}, {"response_format", format}};
            if (delivery == Delivery::Streaming) request["stream_format"] = "audio";
            const auto before = upstream.request_count();
            adapter->audio_speech(request, audio.sink);
            auto expected = request;
            expected["model"] = "kokoro";
            if (delivery == Delivery::Streaming) expected["stream"] = true;
            check(upstream.request_count() == before + 1 && upstream.last_path() == "/v1/audio/speech" && upstream.last_body() == expected &&
                  audio.bytes == "fixture-pcm" && audio.done == 1, "speech adapter preserves delivery and selected format on wire: " + format);
        }
    }
    check(!operation_adapter<ITextToSpeechServer>(kokoro, Op::TextToSpeech, Delivery::Realtime, ModelType::TTS), "Kokoro never enables realtime speech");
    for (const auto mode : {ModelType::LLM, ModelType::AUDIO_GENERATION}) {
        check(!operation_adapter<ITextToSpeechServer>(kokoro, Op::TextToSpeech, Delivery::Buffered, mode) &&
              !operation_adapter<ITextToSpeechServer>(kokoro, Op::TextToSpeech, Delivery::Streaming, mode), "wrong mode cannot acquire either speech delivery");
    }
}

void check_router_fallback_dispatch(FakeHttpBackend& upstream) {
    RuntimeConfig config(json{{"auto_evict", false}, {"auto_evict_threshold_pct", 0.9}, {"inhibit_suspend", false},
                              {"log_level", "error"}, {"max_loaded_models", 4}});
    struct RestoreConfig {
        RuntimeConfig* previous;
        ~RestoreConfig() { RuntimeConfig::set_global(previous); }
    } restore{RuntimeConfig::global()};
    RuntimeConfig::set_global(&config);
    Router router(&config, nullptr, nullptr);
    ModelInfo model;
    model.recipe = "unknown-operation-fixture";
    auto fallback = RoutingHelperTestHook::create_backend(router, model);
    check(dynamic_cast<backends::LlamaCppServer*>(fallback.get()) != nullptr && fallback->get_descriptor() == nullptr,
          "actual router factory retains the direct llama.cpp fallback for an unknown recipe");
    LocalServer<backends::LlamaCppServer>::attach_transport(*fallback, upstream.port());
    for (const auto operation : {Op::ChatCompletion, Op::Completion, Op::Responses}) {
        const auto before = upstream.request_count();
        const auto response = fallback->dispatch_completion_operation(operation, json{{"model", "client-fixture"}});
        const auto path = operation == Op::ChatCompletion ? "/v1/chat/completions" : operation == Op::Completion ? "/v1/completions" : "/v1/responses";
        check(response == json{{"model", "client-fixture"}, {"witness", true}} && upstream.request_count() == before + 1 && upstream.last_path() == path,
              "router fallback buffered adapter dispatches to the expected fake HTTP path");
        Capture stream;
        fallback->dispatch_streaming_operation(operation, "{\"stream\":true}", stream.sink);
        check(upstream.request_count() == before + 2 && upstream.last_path() == path &&
              stream.bytes == "data: {\"choices\":[{\"delta\":{\"content\":\"fixture\"}}]}\n\ndata: [DONE]\n\n",
              "router fallback streaming adapter preserves the expected path and SSE bytes");
    }
}

}

int main() {
    try {
        check_rejections();
        check_default_rejection_framing();
        FakeHttpBackend upstream;
        check_rejection_transport_isolation(upstream);
        check_llama_dispatch(upstream);
        check_flm_dispatch(upstream);
        check_whisper_wire(upstream);
        check_flm_and_moonshine_wire(upstream);
        check_cloud_wire(upstream);
        check_tts_delivery_wire(upstream);
        check_router_fallback_dispatch(upstream);
    } catch (const std::exception& error) {
        check(false, std::string("unexpected exception: ") + error.what());
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
