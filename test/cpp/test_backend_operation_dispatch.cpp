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
#include "lemon/wrapped_server.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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
        auto post = [this](const httplib::Request& request, httplib::Response& response) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                paths_.push_back(request.path);
                bodies_.push_back(request.body);
                form_ = request.form;
            }
            const auto body = json::parse(request.body, nullptr, false);
            if (body.is_object() && body.value("stream", false)) {
                response.set_content("data: {\"choices\":[{\"delta\":{\"content\":\"fixture\"}}]}\n\ndata: [DONE]\n\n", "text/event-stream");
            } else if (request.path == "/inference") {
                response.set_content("{\"text\":\"fixture transcription\"}", "application/json");
            } else if (request.path == "/v1/audio/speech") {
                response.set_content("fixture-pcm", "audio/pcm");
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

private:
    httplib::Server server_;
    int port_ = 0;
    std::thread thread_;
    std::mutex mutex_;
    std::vector<std::string> paths_;
    std::vector<std::string> bodies_;
    httplib::MultipartFormData form_;
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
};

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
    LocalServer<backends::HrxServer> hrx(upstream.port());
    operation_adapter<IEmbeddingsServer>(hrx, Op::Embeddings, Delivery::Buffered, ModelType::LLM)->embeddings(request);
    check(upstream.last_path() == "/v1/embeddings", "HRX inherits callable embeddings despite advertised mask");
    operation_adapter<IRerankingServer>(hrx, Op::Reranking, Delivery::Buffered, ModelType::LLM)->reranking(request);
    check(upstream.last_path() == "/v1/rerank", "HRX inherits callable reranking despite advertised mask");
}

json unsupported(const std::string& message, bool code = false) {
    json error = {{"error", {{"message", message}, {"type", "unsupported_operation"}}}};
    if (code) error["error"]["code"] = "model_not_applicable";
    return error;
}

class FallbackSpeechServer : public backends::KokoroServer {
public:
    FallbackSpeechServer() : backends::KokoroServer("error", nullptr, nullptr) {}
    std::vector<std::string> supported_streaming_audio_formats() const override { return {}; }
    AudioFormatMetadata audio_format_metadata(const std::string& format) const override {
        return format == "pcm" ? AudioFormatMetadata{"audio/pcm", {{"X-Sample-Rate", "24000"}, {"X-Channels", "1"}}}
                               : AudioFormatMetadata{};
    }
};

void check_rejections() {
    backends::KokoroServer kokoro("error", nullptr, nullptr);
    backends::WhisperServer whisper("error", nullptr, nullptr);
    backends::MoonshineServer moonshine("error", nullptr, nullptr);
    backends::SDServer sd("error", nullptr, nullptr);
    backends::TheNoiseServer noise("error", nullptr, nullptr);
    for (const auto operation : {Op::ChatCompletion, Op::Completion, Op::Responses}) {
        const std::string label = operation == Op::ChatCompletion ? "chat completion" : operation == Op::Completion ? "text completion" : "responses";
        check(kokoro.dispatch_completion_operation(operation, {}) == unsupported("Kokoro does not support text completion. Use audio speech endpoints instead.", true), "exact Kokoro exceptional rejection");
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
    check(operation_metadata_adapter<ITextToSpeechServer>(&kokoro, Op::TextToSpeech, Delivery::Buffered)->supported_audio_formats() ==
          std::vector<std::string>({"mp3", "wav", "opus", "pcm"}), "buffered Kokoro formats preserve pre-mode-check metadata");
    check(operation_metadata_adapter<ITextToSpeechServer>(&kokoro, Op::TextToSpeech, Delivery::Streaming)->supported_streaming_audio_formats() ==
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
    const auto formats = kokoro.supported_streaming_audio_formats();
    check(std::find(formats.begin(), formats.end(), "mp3") == formats.end(), "explicit MP3 is unavailable in Kokoro streaming formats");
}

void check_flm_dispatch(FakeHttpBackend& upstream) {
    LocalServer<backends::FastFlowLMServer> server(upstream.port());
    check(!server.supports_operation(Op::Transcription, Delivery::Buffered, ModelType::LLM), "FLM rejects transcription in chat mode");
    auto* transcription_rejection = operation_rejection_adapter<ITranscriptionServer>(server, Op::Transcription, Delivery::Buffered, ModelType::LLM);
    check(transcription_rejection && transcription_rejection->audio_transcriptions({}) == unsupported("Audio transcription not supported by FLM llm model"), "FLM transcription rejection retains model-specific body");
    check(server.supports_operation(Op::Embeddings, Delivery::Buffered, ModelType::LLM), "FLM embeddings remain available in chat mode");
    operation_adapter<IEmbeddingsServer>(server, Op::Embeddings, Delivery::Buffered, ModelType::LLM)->embeddings({});
    check(upstream.last_path() == "/v1/embeddings", "FLM embeddings in chat mode reach transport");
    check(server.dispatch_completion_operation(Op::Responses, {}) == unsupported("Responses API not supported by flm"), "FLM buffered responses retain rejection");
    Capture legacy;
    server.dispatch_streaming_operation(Op::Responses, "{\"stream\":true}", legacy.sink);
    check(upstream.last_path() == "/v1/responses" && upstream.last_body().value("model", "") == "upstream-fixture" &&
          legacy.bytes.find("fixture") != std::string::npos && legacy.bytes.find("[DONE]") != std::string::npos,
          "FLM legacy streaming responses reach fake upstream with checkpoint rewrite");
    for (const auto mode : {ModelType::EMBEDDING, ModelType::TRANSCRIPTION}) {
        LocalServer<backends::FastFlowLMServer> other(upstream.port(), mode);
        for (const auto operation : {Op::ChatCompletion, Op::Completion, Op::Responses}) {
            Capture rejection;
            other.dispatch_streaming_operation(operation, "{}", rejection.sink);
            check(!other.supports_operation(operation, Delivery::Streaming, mode) &&
                  rejection.bytes == "data: {\"error\":{\"message\":\"Streaming not supported for FLM " + model_type_to_string(mode) + " model\",\"type\":\"unsupported_operation\"}}\n\n" &&
                  rejection.done == 1, "exact FLM mode-specific SSE rejection and termination");
        }
        check(other.dispatch_completion_operation(Op::ChatCompletion, {}) == unsupported("Chat completion not supported by FLM " + model_type_to_string(mode) + " model"), "FLM buffered chat preserves mode rejection");
        check(other.dispatch_completion_operation(Op::Completion, {}) == unsupported("Text completion not supported by FLM " + model_type_to_string(mode) + " model"), "FLM buffered completion preserves mode rejection");
    }
    LocalServer<backends::FastFlowLMServer> asr(upstream.port(), ModelType::TRANSCRIPTION);
    auto* embeddings_rejection = operation_rejection_adapter<IEmbeddingsServer>(asr, Op::Embeddings, Delivery::Buffered, ModelType::TRANSCRIPTION);
    check(embeddings_rejection && embeddings_rejection->embeddings({}) == unsupported("Embeddings not supported by FLM transcription model"), "FLM embeddings preserve ASR rejection body");
}

void check_whisper_wire(FakeHttpBackend& upstream) {
    LocalServer<backends::WhisperServer> whisper(upstream.port(), ModelType::TRANSCRIPTION);
    auto* adapter = operation_adapter<ITranscriptionServer>(whisper, Op::Transcription, Delivery::Buffered, ModelType::TRANSCRIPTION);
    const auto response = adapter->audio_transcriptions({{"file_data", "fixture-audio"}, {"filename", "fixture.mp3"}, {"language", "pl"}});
    check(response == json{{"text", "fixture transcription"}} && upstream.last_path() == "/inference" &&
          upstream.field("file") == "fixture-audio" && upstream.field("language") == "pl", "Whisper live in-memory multipart path survives and sends exact fields");
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
    check(cloud.dispatch_completion_operation(Op::Responses, {}) == unsupported("Responses API not supported by cloud (operation-contract-fixture)"), "Cloud buffered responses still reject");
    Capture capture;
    cloud.dispatch_streaming_operation(Op::Responses, "{\"stream\":true}", capture.sink);
    check(upstream.last_path() == "/v1/responses" && upstream.last_body().value("model", "") == "fixture-upstream" &&
          capture.bytes.find("fixture") != std::string::npos && capture.bytes.find("[DONE]") != std::string::npos,
          "Cloud legacy streaming responses reach only fake loopback upstream");
    cloud.unload();
    check(cloud.supports_operation(Op::Responses, Delivery::Streaming, ModelType::LLM), "Cloud unload does not mutate operation support");
}

} // namespace

int main() {
    try {
        check_rejections();
        FakeHttpBackend upstream;
        check_llama_dispatch(upstream);
        check_flm_dispatch(upstream);
        check_whisper_wire(upstream);
        check_cloud_wire(upstream);
    } catch (const std::exception& error) {
        check(false, std::string("unexpected exception: ") + error.what());
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
