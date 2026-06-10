#pragma once

#include "../wrapped_server.h"
#include "../server_capabilities.h"
#include "backend_utils.h"
#include <string>
#include <filesystem>
#include <vector>
#include <cstdint>

namespace lemon {
namespace backends {

// Streaming speech-to-text backend wrapping the sherpa-onnx online (streaming)
// transducer recognizer.
//
// Unlike whisper.cpp, sherpa-onnx ships a true streaming recognizer served by
// `sherpa-onnx-online-websocket-server`, which speaks a small custom WebSocket
// protocol (binary float32 PCM frames + a "Done" text sentinel, JSON results
// back). This class launches that server as a subprocess and bridges
// lemonade's OpenAI-shaped `audio/transcriptions` (and therefore the realtime
// WebSocket path, which feeds WAV chunks through the same router method) onto
// it: decode the incoming audio to mono 16 kHz PCM, stream it to the sherpa
// server, and return the aggregated transcript as `{ "text": ... }`.
//
// ROCm: sherpa-onnx PR #1110 adds an AMD ROCm execution provider, built with
// -DSHERPA_ONNX_ENABLE_ROCM=ON and selected at runtime with --provider=rocm.
// The "rocm" backend selects that prebuilt and launches with --provider=rocm;
// the "cpu" backend uses the upstream shared build and --provider=cpu. ROCm is
// Linux x64 only (per PR #1110); other platforms throw.
class SherpaServer : public WrappedServer, public ITranscriptionServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    inline static const BackendSpec SPEC = BackendSpec(
        "sherpa-onnx",
#ifdef _WIN32
        "sherpa-onnx-online-websocket-server.exe"
#else
        "sherpa-onnx-online-websocket-server"
#endif
        , get_install_params
    );

    explicit SherpaServer(const std::string& log_level,
                          ModelManager* model_manager,
                          BackendManager* backend_manager);

    ~SherpaServer() override;

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;

    void unload() override;

    // ICompletionServer implementation (not supported - return errors)
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    // ITranscriptionServer implementation
    json audio_transcriptions(const json& request) override;

private:
    // Resolve the transducer triple (encoder/decoder/joiner ONNX + tokens.txt)
    // from the resolved model path. The model path may point at any one of the
    // four files or at the directory containing them.
    struct TransducerPaths {
        std::string encoder;
        std::string decoder;
        std::string joiner;
        std::string tokens;
    };
    TransducerPaths resolve_transducer_paths(const std::string& model_path);

    // Decode arbitrary input audio (request "file_data") to mono 16 kHz PCM16
    // little-endian samples. Only WAV is decoded natively; the realtime path
    // (and the documented streaming contract) supplies 16 kHz mono PCM16 WAV.
    std::vector<float> decode_to_mono_f32(const std::string& audio_data,
                                          const std::string& filename,
                                          int target_sample_rate);

    // Stream float32 samples to the running sherpa websocket server and collect
    // the final transcript. Returns the recognized text.
    std::string stream_to_websocket(const std::vector<float>& samples);

    std::string model_path_;
    std::string sherpa_backend_;   // "rocm" or "cpu" (provider passed to server)
    int num_threads_ = 2;
    std::string decoding_method_ = "greedy_search";
};

} // namespace backends
} // namespace lemon
