#include "lemon/backends/sherpa_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backend_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/audio_types.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/process_manager.h"
#include "lemon/error_types.h"
#include <cstring>
#include <iostream>
#include <chrono>
#include <thread>
#include <filesystem>
#include <set>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <nlohmann/json.hpp>
#include <libwebsockets.h>
#include <lemon/utils/aixlog.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

InstallParams SherpaServer::get_install_params(const std::string& backend, const std::string& version) {
    InstallParams params;

    if (backend == "rocm") {
        // sherpa-onnx PR #1110 adds the ROCm execution provider. Upstream does
        // not yet publish a prebuilt ROCm asset, so lemonade hosts a ROCm build
        // (-DSHERPA_ONNX_ENABLE_ROCM=ON -DBUILD_SHARED_LIBS=ON) in the same
        // style as the whisper.cpp builds repo. Linux x64 only, per PR #1110.
        //
        // ALL-AMD-GPU build: this single asset is a multi-arch ("fat") binary
        // compiled for a broad AMD GPU target list (RDNA2/3/3.5/4 + CDNA), so
        // ONE download runs on every AMD GPU. It is intentionally NOT per-gfx
        // (unlike llamacpp/sd, which call SystemInfo::get_rocm_arch() and fetch
        // an arch-specific asset). There is no gfx pin here. The build is
        // produced by .github/workflows/build-sherpa-onnx-rocm.yml. Any future
        // unlisted arch can still be forced at runtime via HSA_OVERRIDE_GFX_VERSION
        // (inherited from the parent process env).
#if defined(__linux__)
        params.repo = "lemonade-sdk/sherpa-onnx-builds";
        params.filename = "sherpa-onnx-" + version + "-linux-x64-rocm-shared.tar.bz2";
#else
        throw std::runtime_error("sherpa-onnx ROCm backend is only supported on Linux x64");
#endif
    } else if (backend == "cpu") {
        // Upstream k2-fsa shared CPU build (CPU execution provider).
        params.repo = "k2-fsa/sherpa-onnx";
#if defined(__linux__)
        params.filename = "sherpa-onnx-" + version + "-linux-x64-shared.tar.bz2";
#elif defined(_WIN32)
        params.filename = "sherpa-onnx-" + version + "-win-x64-shared.tar.bz2";
#elif defined(__APPLE__)
        params.filename = "sherpa-onnx-" + version + "-osx-universal2-shared.tar.bz2";
#else
        throw std::runtime_error("Unsupported platform for sherpa-onnx");
#endif
    } else {
        throw std::runtime_error("[SherpaServer] Unknown sherpa-onnx backend: " + backend);
    }

    return params;
}

SherpaServer::SherpaServer(const std::string& log_level, ModelManager* model_manager, BackendManager* backend_manager)
    : WrappedServer("sherpa-onnx-server", log_level, model_manager, backend_manager) {
}

SherpaServer::~SherpaServer() {
    unload();
}

namespace {

// The sherpa online websocket server has no HTTP health endpoint, so the
// base-class HTTP readiness probe (which requires a 200) never succeeds. Poll a
// raw TCP connect instead: once the port accepts connections the server is up.
bool tcp_port_open(int port, int timeout_ms) {
#ifdef _WIN32
    SOCKET sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return false;
#else
    int sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return false;
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    bool ok = ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    (void)timeout_ms;  // blocking connect with loopback resolves immediately
#ifdef _WIN32
    ::closesocket(sock);
#else
    ::close(sock);
#endif
    return ok;
}

// --- Minimal WAV (RIFF/PCM) decoder -------------------------------------
// The realtime path and the documented streaming contract supply 16 kHz mono
// PCM16 WAV. We parse the canonical RIFF header, downmix to mono, convert
// PCM16 -> float32 in [-1, 1], and (if needed) linearly resample to the
// recognizer's target rate.
struct WavData {
    std::vector<float> samples;  // mono float32
    int sample_rate = 0;
};

uint32_t rd_u32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t rd_u16(const unsigned char* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

WavData decode_wav(const std::string& data) {
    WavData out;
    const auto* buf = reinterpret_cast<const unsigned char*>(data.data());
    const size_t n = data.size();
    if (n < 44 || std::memcmp(buf, "RIFF", 4) != 0 || std::memcmp(buf + 8, "WAVE", 4) != 0) {
        throw std::runtime_error("Unsupported audio: expected mono 16 kHz PCM16 WAV");
    }

    uint16_t channels = 1;
    uint16_t bits_per_sample = 16;
    int sample_rate = 0;
    size_t pos = 12;
    const unsigned char* data_chunk = nullptr;
    uint32_t data_size = 0;

    while (pos + 8 <= n) {
        const unsigned char* chunk_id = buf + pos;
        uint32_t chunk_size = rd_u32(buf + pos + 4);
        const unsigned char* chunk_body = buf + pos + 8;
        if (std::memcmp(chunk_id, "fmt ", 4) == 0 && pos + 8 + 16 <= n) {
            channels = rd_u16(chunk_body + 2);
            sample_rate = static_cast<int>(rd_u32(chunk_body + 4));
            bits_per_sample = rd_u16(chunk_body + 14);
        } else if (std::memcmp(chunk_id, "data", 4) == 0) {
            data_chunk = chunk_body;
            data_size = chunk_size;
            if (pos + 8 + data_size > n) {
                data_size = static_cast<uint32_t>(n - (pos + 8));  // tolerate truncated size
            }
            break;
        }
        pos += 8 + chunk_size + (chunk_size & 1);  // chunks are word-aligned
    }

    if (!data_chunk || sample_rate == 0 || bits_per_sample != 16 || channels == 0) {
        throw std::runtime_error("Unsupported WAV format: only 16-bit PCM is supported");
    }

    const auto* pcm = reinterpret_cast<const int16_t*>(data_chunk);
    size_t total_samples = data_size / sizeof(int16_t);
    size_t frames = total_samples / channels;
    out.samples.reserve(frames);
    for (size_t i = 0; i < frames; ++i) {
        int32_t acc = 0;
        for (uint16_t c = 0; c < channels; ++c) {
            acc += pcm[i * channels + c];
        }
        float v = static_cast<float>(acc) / channels / 32768.0f;
        out.samples.push_back(v);
    }
    out.sample_rate = sample_rate;
    return out;
}

std::vector<float> resample_linear(const std::vector<float>& in, int in_rate, int out_rate) {
    if (in_rate == out_rate || in.empty()) return in;
    double ratio = static_cast<double>(out_rate) / in_rate;
    size_t out_n = static_cast<size_t>(in.size() * ratio);
    std::vector<float> out;
    out.reserve(out_n);
    for (size_t i = 0; i < out_n; ++i) {
        double src = i / ratio;
        size_t i0 = static_cast<size_t>(src);
        size_t i1 = std::min(i0 + 1, in.size() - 1);
        double frac = src - i0;
        out.push_back(static_cast<float>(in[i0] * (1.0 - frac) + in[i1] * frac));
    }
    return out;
}

// --- libwebsockets client for the sherpa online websocket protocol ------
// Protocol (from sherpa-onnx online-websocket-server-impl.cc):
//   * client sends binary frames of raw float32 PCM samples
//   * client sends a text frame "Done" to mark end of stream
//   * server replies with JSON results ({ "text": ..., ... }); on completion it
//     sends the text sentinel "Done!".
struct SherpaWsClient {
    std::string host = "127.0.0.1";
    int port = 0;
    const std::vector<float>* samples = nullptr;
    size_t offset = 0;
    bool sent_done = false;

    std::string last_text;     // most recent non-empty transcript JSON text
    bool finished = false;
    std::string error;

    std::mutex mtx;
    std::condition_variable cv;
};

// Send up to ~3200 samples (200 ms @ 16 kHz) per writeable callback.
constexpr size_t kSamplesPerMessage = 3200;

int sherpa_ws_callback(struct lws* wsi, enum lws_callback_reasons reason,
                       void* /*user*/, void* in, size_t len) {
    struct lws_context* ctx = lws_get_context(wsi);
    auto* client = static_cast<SherpaWsClient*>(lws_context_user(ctx));
    if (!client) return 0;

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            lws_callback_on_writable(wsi);
            break;

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
            std::lock_guard<std::mutex> lk(client->mtx);
            client->error = in ? std::string(static_cast<char*>(in), len)
                                : "sherpa-onnx websocket connection error";
            client->finished = true;
            client->cv.notify_all();
            break;
        }

        case LWS_CALLBACK_CLIENT_RECEIVE: {
            std::string msg(static_cast<char*>(in), len);
            if (msg == "Done!") {
                std::lock_guard<std::mutex> lk(client->mtx);
                client->finished = true;
                client->cv.notify_all();
                return -1;  // close the connection
            }
            // Otherwise it's a JSON result; keep the latest non-empty text.
            try {
                auto j = nlohmann::json::parse(msg);
                if (j.contains("text") && j["text"].is_string()) {
                    std::string t = j["text"].get<std::string>();
                    if (!t.empty()) {
                        std::lock_guard<std::mutex> lk(client->mtx);
                        client->last_text = t;
                    }
                }
            } catch (const std::exception&) {
                // Non-JSON frame; ignore.
            }
            break;
        }

        case LWS_CALLBACK_CLIENT_WRITEABLE: {
            if (client->samples && client->offset < client->samples->size()) {
                size_t remaining = client->samples->size() - client->offset;
                size_t count = std::min(remaining, kSamplesPerMessage);
                size_t bytes = count * sizeof(float);
                std::vector<unsigned char> frame(LWS_PRE + bytes);
                std::memcpy(frame.data() + LWS_PRE,
                            client->samples->data() + client->offset, bytes);
                int written = lws_write(wsi, frame.data() + LWS_PRE, bytes, LWS_WRITE_BINARY);
                if (written < static_cast<int>(bytes)) {
                    std::lock_guard<std::mutex> lk(client->mtx);
                    client->error = "Failed to write audio frame to sherpa-onnx";
                    client->finished = true;
                    client->cv.notify_all();
                    return -1;
                }
                client->offset += count;
                lws_callback_on_writable(wsi);
            } else if (!client->sent_done) {
                // All samples sent; send the "Done" sentinel as a text frame.
                static const std::string done = "Done";
                std::vector<unsigned char> frame(LWS_PRE + done.size());
                std::memcpy(frame.data() + LWS_PRE, done.data(), done.size());
                lws_write(wsi, frame.data() + LWS_PRE, done.size(), LWS_WRITE_TEXT);
                client->sent_done = true;
            }
            break;
        }

        case LWS_CALLBACK_CLIENT_CLOSED: {
            std::lock_guard<std::mutex> lk(client->mtx);
            client->finished = true;
            client->cv.notify_all();
            break;
        }

        default:
            break;
    }
    return 0;
}

const struct lws_protocols kSherpaProtocols[] = {
    {"sherpa-online", sherpa_ws_callback, 0, 1 << 16, 0, nullptr, 0},
    LWS_PROTOCOL_LIST_TERM
};

}  // namespace

SherpaServer::TransducerPaths SherpaServer::resolve_transducer_paths(const std::string& model_path) {
    TransducerPaths paths;

    // model_path may be a directory or any of the four model files. Search the
    // containing directory (recursively) for the transducer triple + tokens.
    fs::path p(model_path);
    fs::path dir = fs::is_directory(p) ? p : p.parent_path();

    if (!fs::exists(dir)) {
        throw std::runtime_error("sherpa-onnx model directory not found: " + dir.string());
    }

    auto matches = [](const std::string& name, const std::string& key) {
        return name.find(key) != std::string::npos;
    };

    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        std::string fname = entry.path().filename().string();
        std::string full = entry.path().string();
        if (fname == "tokens.txt") {
            paths.tokens = full;
        } else if (entry.path().extension() == ".onnx") {
            if (matches(fname, "encoder") && paths.encoder.empty()) paths.encoder = full;
            else if (matches(fname, "decoder") && paths.decoder.empty()) paths.decoder = full;
            else if (matches(fname, "joiner") && paths.joiner.empty()) paths.joiner = full;
        }
    }

    if (paths.encoder.empty() || paths.decoder.empty() ||
        paths.joiner.empty() || paths.tokens.empty()) {
        throw std::runtime_error(
            "sherpa-onnx model is incomplete: expected encoder/decoder/joiner .onnx "
            "and tokens.txt in " + dir.string());
    }

    return paths;
}

void SherpaServer::load(const std::string& model_name,
                        const ModelInfo& model_info,
                        const RecipeOptions& options,
                        bool do_not_upgrade) {
    LOG(INFO, "SherpaServer") << "Loading model: " << model_name << std::endl;
    LOG(INFO, "SherpaServer") << "Per-model settings: " << options.to_log_string() << std::endl;

    std::string sherpa_backend = options.get_option("sherpa-onnx_backend").get<std::string>();
    std::string sherpa_args = options.get_option("sherpa-onnx_args").get<std::string>();

    RuntimeConfig::validate_backend_choice("sherpa-onnx", sherpa_backend);

    // Device reporting: ROCm runs on the GPU, CPU provider on the CPU.
    if (sherpa_backend == "rocm") {
        device_type_ = DEVICE_GPU;
    } else {
        device_type_ = DEVICE_CPU;
    }
    sherpa_backend_ = sherpa_backend;

    backend_manager_->install_backend(SPEC.recipe, sherpa_backend);

    std::string model_path = model_info.resolved_path();
    if (model_path.empty()) {
        throw std::runtime_error("Model file not found for checkpoint: " + model_info.checkpoint());
    }
    model_path_ = model_path;

    TransducerPaths tp = resolve_transducer_paths(model_path);

    LOG(INFO, "SherpaServer") << "Using backend: " << sherpa_backend << std::endl;
    LOG(INFO, "SherpaServer") << "  encoder: " << tp.encoder << std::endl;
    LOG(INFO, "SherpaServer") << "  decoder: " << tp.decoder << std::endl;
    LOG(INFO, "SherpaServer") << "  joiner:  " << tp.joiner << std::endl;
    LOG(INFO, "SherpaServer") << "  tokens:  " << tp.tokens << std::endl;

    std::string exe_path = BackendUtils::get_backend_binary_path(SPEC, sherpa_backend);

    port_ = choose_port();
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port");
    }

    LOG(INFO, "SherpaServer") << "Starting server on port " << port_ << std::endl;

    // The "rocm" backend selects the ROCm execution provider (PR #1110); the
    // "cpu" backend uses the default cpu provider.
    std::string provider = (sherpa_backend == "rocm") ? "rocm" : "cpu";

    // Lemonade manages the model triple, port, and provider; users may add
    // sherpa flags (e.g. --num-threads, --decoding-method) via sherpa-onnx_args.
    std::vector<std::string> args = {
        "--encoder", tp.encoder,
        "--decoder", tp.decoder,
        "--joiner", tp.joiner,
        "--tokens", tp.tokens,
        "--provider", provider,
        "--port", std::to_string(port_)
    };

    std::set<std::string> reserved_flags = {
        "--encoder", "--decoder", "--joiner", "--tokens", "--provider", "--port"
    };

    if (!sherpa_args.empty()) {
        std::string validation_error = validate_custom_args(sherpa_args, reserved_flags);
        if (!validation_error.empty()) {
            throw std::invalid_argument(
                "Invalid custom sherpa-onnx-online-websocket-server arguments:\n" + validation_error
            );
        }

        LOG(DEBUG, "SherpaServer") << "Adding custom arguments: " << sherpa_args << std::endl;
        std::vector<std::string> custom_args_vec = parse_custom_args(sherpa_args);
        args.insert(args.end(), custom_args_vec.begin(), custom_args_vec.end());
    }

    // Set up environment variables for shared library loading.
    std::vector<std::pair<std::string, std::string>> env_vars;
    fs::path exe_dir = fs::path(exe_path).parent_path();

#ifndef _WIN32
    std::string lib_path = exe_dir.string();
    // sherpa-onnx ships its shared libs alongside the binary and (often) in a
    // sibling lib/ directory.
    fs::path sibling_lib = exe_dir.parent_path() / "lib";
    if (fs::exists(sibling_lib)) {
        lib_path = lib_path + ":" + sibling_lib.string();
    }

    const char* existing_ld_path = std::getenv("LD_LIBRARY_PATH");
    if (existing_ld_path && strlen(existing_ld_path) > 0) {
        lib_path = lib_path + ":" + std::string(existing_ld_path);
    }

    env_vars.push_back({"LD_LIBRARY_PATH", lib_path});
    if (is_debug()) {
        std::cout << "[SherpaServer] Setting LD_LIBRARY_PATH=" << lib_path << std::endl;
    }
#endif

    process_handle_ = utils::ProcessManager::start_process(
        exe_path,
        args,
        "",          // working_dir (empty = current)
        is_debug(),  // inherit_output
        false,       // filter_health_logs
        env_vars
    );

    if (process_handle_.pid == 0) {
        throw std::runtime_error("Failed to start sherpa-onnx-online-websocket-server process");
    }

    LOG(INFO, "SherpaServer") << "Process started with PID: " << process_handle_.pid << std::endl;

    // sherpa's websocket server has no HTTP health endpoint, so poll for the
    // listening TCP port instead of the base-class HTTP readiness probe.
    const int max_attempts = 6000;  // ~600s at 100ms
    bool ready = false;
    for (int i = 0; i < max_attempts; ++i) {
        if (!utils::ProcessManager::is_running(process_handle_)) {
            int exit_code = utils::ProcessManager::get_exit_code(process_handle_);
            LOG(ERROR, "SherpaServer") << "sherpa-onnx server terminated with exit code: "
                                       << exit_code << std::endl;
            break;
        }
        if (tcp_port_open(port_, 1000)) {
            ready = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!ready) {
        unload();
        throw std::runtime_error("sherpa-onnx server failed to start or become ready");
    }

    LOG(INFO, "SherpaServer") << "Server is ready!" << std::endl;
}

void SherpaServer::unload() {
    if (process_handle_.pid != 0) {
        LOG(INFO, "SherpaServer") << "Stopping server (PID: " << process_handle_.pid << ")" << std::endl;
        utils::ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        port_ = 0;
    }
}

// ICompletionServer implementation - not supported for sherpa-onnx STT
json SherpaServer::chat_completion(const json& request) {
    return json{
        {"error", {
            {"message", "sherpa-onnx models do not support chat completion. Use audio transcription endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

json SherpaServer::completion(const json& request) {
    return json{
        {"error", {
            {"message", "sherpa-onnx models do not support text completion. Use audio transcription endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

json SherpaServer::responses(const json& request) {
    return json{
        {"error", {
            {"message", "sherpa-onnx models do not support responses. Use audio transcription endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

std::vector<float> SherpaServer::decode_to_mono_f32(const std::string& audio_data,
                                                    const std::string& filename,
                                                    int target_sample_rate) {
    fs::path fp(filename);
    std::string ext = fp.extension().string();
    for (auto& c : ext) c = static_cast<char>(::tolower(c));

    // Only WAV is decoded natively. The realtime path always sends 16 kHz mono
    // PCM16 WAV; the batch path documents WAV as the streaming-backend input.
    if (!ext.empty() && ext != ".wav") {
        throw std::runtime_error(
            "sherpa-onnx streaming backend requires PCM16 WAV input (got '" + ext +
            "'); convert to WAV or use a whisper.cpp model for compressed formats");
    }

    WavData wav = decode_wav(audio_data);
    return resample_linear(wav.samples, wav.sample_rate, target_sample_rate);
}

std::string SherpaServer::stream_to_websocket(const std::vector<float>& samples) {
    SherpaWsClient client;
    client.port = port_;
    client.samples = &samples;

    struct lws_context_creation_info info;
    std::memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;  // client-only context
    info.protocols = kSherpaProtocols;
    info.gid = -1;
    info.uid = -1;
    info.user = &client;

    struct lws_context* context = lws_create_context(&info);
    if (!context) {
        throw std::runtime_error("Failed to create libwebsockets context for sherpa-onnx client");
    }

    struct lws_client_connect_info ccinfo;
    std::memset(&ccinfo, 0, sizeof(ccinfo));
    ccinfo.context = context;
    ccinfo.address = "127.0.0.1";
    ccinfo.port = port_;
    ccinfo.path = "/";
    ccinfo.host = "127.0.0.1";
    ccinfo.origin = "127.0.0.1";
    ccinfo.protocol = kSherpaProtocols[0].name;

    if (!lws_client_connect_via_info(&ccinfo)) {
        lws_context_destroy(context);
        throw std::runtime_error("Failed to connect to sherpa-onnx websocket server");
    }

    // Service the event loop until the server signals completion ("Done!") or
    // the connection closes. Bounded by a generous timeout.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(600);
    while (true) {
        {
            std::lock_guard<std::mutex> lk(client.mtx);
            if (client.finished) break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            std::lock_guard<std::mutex> lk(client.mtx);
            client.error = "Timed out waiting for sherpa-onnx transcription";
            break;
        }
        lws_service(context, 50);
    }

    lws_context_destroy(context);

    std::lock_guard<std::mutex> lk(client.mtx);
    if (!client.error.empty()) {
        throw std::runtime_error(client.error);
    }
    return client.last_text;
}

// ITranscriptionServer implementation
json SherpaServer::audio_transcriptions(const json& request) {
    try {
        if (!request.contains("file_data")) {
            throw std::runtime_error("Missing 'file_data' in request");
        }

        std::string audio_data = request["file_data"].get<std::string>();
        std::string filename = request.value("filename", "audio.wav");

        if (audio_data.empty()) {
            throw std::runtime_error("Empty audio data");
        }

        // Standardized optional carrier-audio params (see audio_types.h
        // RequestParam). sherpa transducer models run at a fixed internal rate;
        // we resample the supplied audio to that rate. The recognizer rate is
        // 16 kHz mono (AudioDefaults). sample_rate, if provided, is only used as
        // a fallback when the WAV header is absent/unreliable.
        const int target_rate = audio::AudioDefaults::SAMPLE_RATE_HZ;

        std::vector<float> samples = decode_to_mono_f32(audio_data, filename, target_rate);
        if (samples.empty()) {
            return json{{"text", ""}};
        }

        std::string text = stream_to_websocket(samples);
        return json{{"text", text}};

    } catch (const std::exception& e) {
        return json{
            {"error", {
                {"message", std::string("Transcription failed: ") + e.what()},
                {"type", "audio_processing_error"}
            }}
        };
    }
}

} // namespace backends
} // namespace lemon
