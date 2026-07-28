#pragma once

#include <nlohmann/json.hpp>
#include <httplib.h>

namespace lemon {

using json = nlohmann::json;

class ICapability {
public:
    virtual ~ICapability() = default;
};

class ICompletionServer : public virtual ICapability {
public:
    virtual ~ICompletionServer() = default;
    virtual json chat_completion(const json& request) = 0;
    virtual json completion(const json& request) = 0;
};

class IEmbeddingsServer : public virtual ICapability {
public:
    virtual ~IEmbeddingsServer() = default;
    virtual json embeddings(const json& request) = 0;
};

class IRerankingServer : public virtual ICapability {
public:
    virtual ~IRerankingServer() = default;
    virtual json reranking(const json& request) = 0;
};

class ITranscriptionServer : public virtual ICapability {
public:
    virtual ~ITranscriptionServer() = default;
    virtual json audio_transcriptions(const json& request) = 0;
};

// Optional streaming transcription capability (realtime STT)
class IStreamingTranscriptionServer : public virtual ICapability {
public:
    virtual ~IStreamingTranscriptionServer() = default;

    // Returns the address for connecting to the backend's streaming server.
    // Format is backend-defined; callers should check the protocol with
    // the backend implementation. For moonshine-server this is a
    // line-delimited JSON-over-TCP endpoint: "tcp://127.0.0.1:<port>".
    virtual std::string get_streaming_address() = 0;
};

// Optional audio capability (text-to-speech)
class ITextToSpeechServer : public virtual ICapability {
public:
    virtual ~ITextToSpeechServer() = default;
    virtual void audio_speech(const json& request, httplib::DataSink& sink) = 0;
    virtual std::vector<std::string> supported_audio_formats() const { return {}; }
};

// Text-classification capability (encoder classifiers: PII, prompt-safety,
// domain, etc.). Input text -> {label: score}. Serves the router's `classifier`
// condition type (issue #2592).
class IClassificationServer : public virtual ICapability {
public:
    virtual ~IClassificationServer() = default;
    virtual json classify(const json& request) = 0;
};

class IImageServer : public virtual ICapability {
public:
    virtual ~IImageServer() = default;
    virtual json image_generations(const json& request) = 0;
    virtual json image_edits(const json& request) = 0;
    virtual json image_variations(const json& request) = 0;
};

// Generative audio capability (text -> audio clip). Serves both music and
// sound-effect models; the loaded model decides which. Streams the encoded
// audio bytes to the sink, like ITextToSpeechServer.
class IAudioGenerationServer : public virtual ICapability {
public:
    virtual ~IAudioGenerationServer() = default;
    virtual void audio_generations(const json& request, httplib::DataSink& sink) = 0;
    virtual std::vector<std::string> supported_audio_formats() const { return {"wav"}; }
};

class IModel3DServer : public virtual ICapability {
public:
    virtual ~IModel3DServer() = default;
    virtual void model_3d_generations(const json& request, httplib::DataSink& sink) = 0;
};

class ISlotsServer : public virtual ICapability {
public:
    virtual ~ISlotsServer() = default;
    virtual json get_slots() = 0;
    virtual json slots_action(int slot_id, const std::string& action, const json& request_body) = 0;
};

class ITokenizerServer : public virtual ICapability {
public:
    virtual ~ITokenizerServer() = default;
    virtual json tokenize(const json& request_body) = 0;
};

template<typename T>
bool supports_capability(ICapability* server) {
    return dynamic_cast<T*>(server) != nullptr;
}

} // namespace lemon
