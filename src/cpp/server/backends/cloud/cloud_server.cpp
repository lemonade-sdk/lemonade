#include "lemon/backends/cloud/cloud_server.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/model_manager.h"
#include "lemon/cloud_provider_registry.h"
#include "lemon/error_types.h"
#include "lemon/runtime_config.h"
#include "lemon/streaming_proxy.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/json_utils.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <string_view>
#include <utility>
#include <lemon/utils/aixlog.hpp>

namespace lemon {
namespace backends {

namespace {

bool id_contains(const std::string& id, const std::string& needle) {
    return id.find(needle) != std::string::npos;
}

// Id-pattern fallback for /v1/models entries that don't publish capability
// metadata (notably OpenAI). Anything unmatched falls through to LLM.
ModelType infer_type(const std::string& id) {
    if (id_contains(id, "flux") || id_contains(id, "stable-diffusion") ||
        id_contains(id, "sdxl") || id_contains(id, "sd-") ||
        id_contains(id, "dall-e") || id_contains(id, "gpt-image") ||
        id_contains(id, "chatgpt-image") || id_contains(id, "sora")) {
        return ModelType::IMAGE;
    }
    if (id_contains(id, "tts")) {
        return ModelType::TTS;
    }
    if (id_contains(id, "whisper") || id_contains(id, "transcribe") ||
        id_contains(id, "realtime") || id_contains(id, "audio")) {
        return ModelType::TRANSCRIPTION;
    }
    if (id_contains(id, "rerank")) {
        return ModelType::RERANKING;
    }
    if (id_contains(id, "embed") || id_contains(id, "bge-") ||
        id_contains(id, "nomic-") || id_contains(id, "moderation")) {
        return ModelType::EMBEDDING;
    }
    return ModelType::LLM;
}

// Decide whether a /v1/models entry should be surfaced as a chat model.
// Trust provider capability metadata first, in priority order, falling back
// to infer_type(id) for bare responses:
//   1. supports_chat: bool       — Fireworks
//   2. capabilities: [string]    — generic
//   3. architecture.modality     — OpenRouter
//   4. type: string              — Together AI
bool is_chat_model(const json& m) {
    if (!m.is_object() || !m.contains("id") || !m["id"].is_string()) {
        return false;
    }

    // Output-shape veto: providers sometimes flag non-text generators with
    // supports_chat=true because they accept chat-shaped requests (Fireworks
    // does this for FLUMINA image-editing models — chat-shape input, image
    // output). Reject those before trusting supports_chat.
    if (m.contains("kind") && m["kind"].is_string()) {
        const std::string kind = m["kind"].get<std::string>();
        if (kind == "FLUMINA_BASE_MODEL" ||
            kind.find("IMAGE") != std::string::npos ||
            kind.find("AUDIO") != std::string::npos ||
            kind.find("VIDEO") != std::string::npos ||
            kind.find("EMBED") != std::string::npos) {
            return false;
        }
    }

    if (m.contains("supports_chat") && m["supports_chat"].is_boolean()) {
        return m["supports_chat"].get<bool>();
    }

    if (m.contains("capabilities") && m["capabilities"].is_array()) {
        for (const auto& cap : m["capabilities"]) {
            if (!cap.is_string()) continue;
            std::string s = cap.get<std::string>();
            if (s == "chat" || s == "chat.completions" || s == "completion") {
                return true;
            }
        }
        return false;
    }

    if (m.contains("architecture") && m["architecture"].is_object()) {
        const auto& arch = m["architecture"];
        if (arch.contains("modality") && arch["modality"].is_string()) {
            const std::string mod = arch["modality"].get<std::string>();
            // OpenRouter encodes modality as "<inputs>-><outputs>", e.g.
            // "text->text", "text+image->text", "text->image". Anything
            // that emits text from a chat-style call is fine; image/audio/
            // embedding outputs are not.
            return mod.find("->text") != std::string::npos;
        }
    }

    // Together AI doesn't use any of the fields above; it tags each model
    // with a "type" (chat / language / code / image / embedding / rerank /
    // moderation / audio). Trust it: text-generating types are chat/completion
    // capable, everything else is not.
    if (m.contains("type") && m["type"].is_string()) {
        const std::string t = m["type"].get<std::string>();
        if (t == "chat" || t == "language" || t == "code") return true;
        if (t == "image" || t == "embedding" || t == "rerank" ||
            t == "moderation" || t == "audio") return false;
    }

    return infer_type(m["id"].get<std::string>()) == ModelType::LLM;
}

std::vector<std::string> chat_labels() {
    return {"cloud"};
}

// Detect capability labels (vision / tool-calling / reasoning) from a
// /v1/models entry, normalising the divergent fields providers use into
// lemonade's shared label vocabulary so cloud models gate inputs like local
// ones. When a signal is absent the capability defaults OFF — under-offering
// an input is safer than letting the client attach an image the provider
// rejects (the per-model override covers cases auto-detection can't).
std::vector<std::string> capability_labels(const json& m) {
    std::vector<std::string> labels;
    if (!m.is_object()) return labels;

    auto flag = [&](const char* key) -> bool {
        return m.contains(key) && m[key].is_boolean() && m[key].get<bool>();
    };
    auto array_has = [](const json& arr, const char* needle) -> bool {
        if (!arr.is_array()) return false;
        for (const auto& e : arr) {
            if (e.is_string() && e.get<std::string>() == needle) return true;
        }
        return false;
    };

    bool vision = flag("supports_image_input") || flag("supports_vision") ||
                  flag("vision") ||
                  array_has(m.value("modalities", json::array()), "image") ||
                  array_has(m.value("input_modalities", json::array()), "image");
    if (!vision && m.contains("architecture") && m["architecture"].is_object()) {
        vision = array_has(m["architecture"].value("input_modalities", json::array()),
                           "image");
    }

    const json params = m.value("supported_parameters", json::array());
    const json caps = m.value("capabilities", json::array());
    bool tools = flag("supports_tools") || flag("function_calling") ||
                 flag("supports_function_calling") ||
                 array_has(params, "tools") || array_has(params, "tool_choice") ||
                 array_has(caps, "tools") || array_has(caps, "function_calling") ||
                 array_has(caps, "tool_calling");

    bool reasoning = flag("reasoning") || flag("supports_reasoning") ||
                     array_has(params, "reasoning") ||
                     array_has(params, "include_reasoning");

    // Id-pattern fallback, consulted only when the entry carries no structured
    // capability hints, so an authoritative "false" from a provider stands.
    const bool has_meta = m.contains("supports_image_input") ||
                          m.contains("supports_vision") || m.contains("vision") ||
                          m.contains("supports_tools") ||
                          m.contains("function_calling") ||
                          m.contains("architecture") || m.contains("capabilities") ||
                          m.contains("supported_parameters") ||
                          m.contains("modalities") || m.contains("input_modalities");
    if (!has_meta && m.contains("id") && m["id"].is_string()) {
        // Lowercase for matching: providers without metadata (Together) use
        // mixed case in ids, e.g. "Qwen/Qwen2.5-VL-72B-Instruct" — a
        // case-sensitive "-vl" would miss it.
        std::string id = m["id"].get<std::string>();
        std::transform(id.begin(), id.end(), id.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (id_contains(id, "gpt-4o") || id_contains(id, "gpt-4.1") ||
            id_contains(id, "gpt-5") || id_contains(id, "-vl") ||
            id_contains(id, "vision") || id_contains(id, "llava") ||
            id_contains(id, "pixtral")) {
            vision = true;
        }
        // Modern OpenAI chat / reasoning models all support tool calling.
        if (id_contains(id, "gpt-4") || id_contains(id, "gpt-5") ||
            id_contains(id, "o1") || id_contains(id, "o3") || id_contains(id, "o4")) {
            tools = true;
        }
        if (id_contains(id, "o1") || id_contains(id, "o3") || id_contains(id, "o4") ||
            id_contains(id, "reason") || id_contains(id, "-thinking")) {
            reasoning = true;
        }
    }

    if (vision) labels.push_back("vision");
    if (tools) labels.push_back("tool-calling");
    if (reasoning) labels.push_back("reasoning");
    return labels;
}

// Normalise a model's pricing to USD per 1,000,000 tokens. Returns
// {input, output}; a component is -1 when the provider doesn't report it
// (or reports 0, which is ambiguous across providers and not worth showing).
//   OpenRouter: pricing.prompt / pricing.completion as USD-per-token strings.
//   Together:   pricing.input / pricing.output as USD-per-million numbers.
//   Fireworks:  no pricing field -> {-1, -1}.
std::pair<double, double> parse_cloud_cost(const json& m) {
    std::pair<double, double> cost{-1.0, -1.0};
    if (!m.contains("pricing") || !m["pricing"].is_object()) {
        return cost;
    }
    const auto& p = m["pricing"];
    auto to_num = [](const json& v) -> double {
        if (v.is_number()) return v.get<double>();
        if (v.is_string()) {
            try { return std::stod(v.get<std::string>()); } catch (...) {}
        }
        return -1.0;
    };
    if (p.contains("prompt") || p.contains("completion")) {
        // OpenRouter: per-token -> per-million.
        const double in = to_num(p.value("prompt", json(nullptr)));
        const double out = to_num(p.value("completion", json(nullptr)));
        if (in > 0) cost.first = in * 1e6;
        if (out > 0) cost.second = out * 1e6;
    } else if (p.contains("input") || p.contains("output")) {
        // Together: already per-million.
        const double in = to_num(p.value("input", json(nullptr)));
        const double out = to_num(p.value("output", json(nullptr)));
        if (in > 0) cost.first = in;
        if (out > 0) cost.second = out;
    }
    return cost;
}

// Build the user-facing model name "<provider>.<cleaned_upstream_id>" by
// applying two content-pattern cleanup rules (no provider-specific code).
// Example: provider="fireworks", id="accounts/fireworks/models/deepseek-v4-pro"
// -> "fireworks.deepseek-v4-pro".
std::string build_public_name(const std::string& provider, const std::string& upstream_id) {
    std::string cleaned = upstream_id;

    // Rule 1: strip the leading "accounts/<x>/models/" wrapper if present.
    const std::string accounts_prefix = "accounts/";
    if (cleaned.rfind(accounts_prefix, 0) == 0) {
        std::string after_accounts = cleaned.substr(accounts_prefix.size());
        auto slash = after_accounts.find('/');
        if (slash != std::string::npos) {
            std::string account = after_accounts.substr(0, slash);
            std::string after_account = after_accounts.substr(slash + 1);
            const std::string models_prefix = "models/";
            if (after_account.rfind(models_prefix, 0) == 0) {
                cleaned = account + "/" + after_account.substr(models_prefix.size());
            }
        }
    }

    // Rule 2: dedup the leading provider segment so we don't double it up.
    std::string lead_dedup = provider + "/";
    if (cleaned.rfind(lead_dedup, 0) == 0) {
        cleaned = cleaned.substr(lead_dedup.size());
    }

    return provider + "." + cleaned;
}

} // namespace

CloudServer::CloudServer(const std::string& provider,
                         const std::string& log_level,
                         ModelManager* model_manager,
                         BackendManager* backend_manager,
                         CloudProviderRegistry* registry)
    : WrappedServer("cloud", log_level, model_manager, backend_manager),
      provider_(provider),
      registry_(registry) {}

CloudServer::~CloudServer() {
    unload();
}

void CloudServer::load(const std::string& model_name,
                       const ModelInfo& model_info,
                       const RecipeOptions& options,
                       bool /*do_not_upgrade*/) {
    (void) options;
    LOG(INFO, "Cloud") << "Loading cloud model: " << model_name << std::endl;

    if (model_info.cloud_provider.empty()) {
        throw std::runtime_error(
            "Cloud model '" + model_name + "' is missing the 'cloud_provider' field "
            "in its registry entry");
    }
    if (model_info.checkpoint().empty()) {
        throw std::runtime_error(
            "Cloud model '" + model_name + "' is missing the 'checkpoint' field "
            "(provider's upstream model id)");
    }

    // No credential resolution at load time — creds are resolved per request
    // via the registry so a runtime key supplied after load still works for
    // already-loaded models. We just record the upstream model id so the
    // per-request handlers know what to rewrite "model" to before forwarding.
    upstream_model_ = model_info.checkpoint();
    LOG(INFO, "Cloud") << "Cloud provider: " << provider_
                       << ", upstream model: " << upstream_model_ << std::endl;
    loaded_ = true;
}

void CloudServer::unload() {
    if (loaded_) {
        LOG(INFO, "Cloud") << "Unloading cloud model: " << model_name_ << std::endl;
    }
    loaded_ = false;
}

bool CloudServer::is_backend_alive() const {
    return loaded_;
}

std::string CloudServer::get_backend_health_state() const {
    return loaded_ ? "ready" : "stopped";
}

CloudServer::ResolvedCreds CloudServer::resolve_creds() const {
    ResolvedCreds creds;
    if (registry_ == nullptr) {
        return creds;
    }
    creds.api_key = registry_->resolve_key(provider_);
    creds.base_url = registry_->base_url_for(provider_);
    if (!creds.api_key.empty() &&
        CloudProviderRegistry::is_http_base_url(creds.base_url) &&
        !registry_->allow_insecure_http_for(provider_)) {
        creds.insecure_http_blocked = true;
    }
    // The registry already normalizes base_url on install, but a defensive
    // strip here keeps the contract local — anyone tracing post_with_auth
    // can see the joined URL can't double-slash.
    while (!creds.base_url.empty() && creds.base_url.back() == '/') {
        creds.base_url.pop_back();
    }
    return creds;
}

json CloudServer::missing_creds_error() const {
    const std::string env_name = CloudProviderRegistry::env_var_name(provider_);
    bool installed = registry_ != nullptr && registry_->is_installed(provider_);
    std::string msg;
    if (!installed) {
        msg = "Cloud provider '" + provider_ + "' is not installed. "
              "POST /v1/install with {\"backend\":\"cloud\",\"provider\":\"" +
              provider_ + "\",\"base_url\":\"https://...\"} first.";
    } else {
        msg = "No API key for cloud provider '" + provider_ + "'. Set the " +
              env_name + " env var or POST /v1/cloud/auth with "
              "{\"provider\":\"" + provider_ + "\",\"api_key\":\"...\"}.";
    }
    return ErrorResponse::create(
        msg,
        ErrorType::BACKEND_ERROR,
        {{"provider", provider_}}
    );
}

std::string CloudServer::missing_creds_sse() const {
    const std::string env_name = CloudProviderRegistry::env_var_name(provider_);
    bool installed = registry_ != nullptr && registry_->is_installed(provider_);
    std::string msg;
    if (!installed) {
        msg = "Cloud provider '" + provider_ + "' is not installed.";
    } else {
        msg = "No API key for cloud provider '" + provider_ + "'. Set " +
              env_name + " or POST /v1/cloud/auth.";
    }
    json err = {{"error", {
        {"message", msg},
        {"type", "backend_error"},
        {"provider", provider_}
    }}};
    return "data: " + err.dump() + "\n\n";
}

json CloudServer::insecure_http_error() const {
    const std::string msg =
        "Cloud provider '" + provider_ + "' uses http:// with an API key. "
        "Reinstall or re-authenticate with allow_insecure_http=true to opt in.";
    return ErrorResponse::create(
        msg,
        ErrorType::BACKEND_ERROR,
        {{"provider", provider_}, {"code", "insecure_http_requires_opt_in"}}
    );
}

std::string CloudServer::insecure_http_sse() const {
    const std::string msg =
        "Cloud provider '" + provider_ + "' uses http:// with an API key. "
        "Reinstall or re-authenticate with allow_insecure_http=true to opt in.";
    json err = {{"error", {
        {"message", msg},
        {"type", "backend_error"},
        {"provider", provider_},
        {"code", "insecure_http_requires_opt_in"}
    }}};
    return "data: " + err.dump() + "\n\n";
}

json CloudServer::rewrite_model_field(const json& request) const {
    json modified = request;
    modified["model"] = upstream_model_;
    utils::JsonUtils::add_legacy_max_tokens_alias(modified);
    return modified;
}

json CloudServer::post_with_auth(const std::string& path, const json& request,
                                  const ResolvedCreds& creds, long timeout_seconds) {
    if (!loaded_) {
        return ErrorResponse::from_exception(ModelNotLoadedException(server_name_));
    }
    if (creds.insecure_http_blocked) {
        return insecure_http_error();
    }
    if (creds.api_key.empty() || creds.base_url.empty()) {
        return missing_creds_error();
    }
    std::string url = creds.base_url + path;
    std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + creds.api_key}
    };

    try {
        auto response = utils::HttpClient::post(url, request.dump(), headers, timeout_seconds);
        if (response.status_code == 200) {
            // Return the body unchanged so the server.cpp handler picks up the
            // `usage` telemetry like every other backend.
            return json::parse(response.body);
        }

        json error_details;
        try {
            error_details = json::parse(response.body);
        } catch (...) {
            error_details = response.body;
        }
        return ErrorResponse::create(
            "cloud (" + provider_ + ") request failed",
            ErrorType::BACKEND_ERROR,
            {
                {"status_code", response.status_code},
                {"response", error_details}
            }
        );
    } catch (const std::exception& e) {
        return ErrorResponse::from_exception(NetworkException(e.what()));
    }
}

json CloudServer::chat_completion(const json& request) {
    json modified = rewrite_model_field(request);
    return post_with_auth("/chat/completions", modified, resolve_creds());
}

json CloudServer::completion(const json& request) {
    json modified = rewrite_model_field(request);
    return post_with_auth("/completions", modified, resolve_creds());
}

json CloudServer::responses(const json& /*request*/) {
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Responses API", "cloud (" + provider_ + ")")
    );
}

void CloudServer::forward_streaming_request(const std::string& endpoint,
                                            const std::string& request_body,
                                            httplib::DataSink& sink,
                                            bool sse,
                                            long timeout_seconds,
                                            TelemetryCallback telemetry_callback) {
    // Telemetry from cloud streaming responses: OpenAI-shape SSE puts the
    // usage block in the final pre-[DONE] chunk. We don't parse it here —
    // the Router-level streaming path delivers cleaner numbers than we can
    // reconstruct from chunked output, and matching local backends here
    // would only diverge subtly. Passing the callback through preserves the
    // contract for callers that pass one in.
    auto sse_error = [](const std::string& message, const std::string& type,
                        const json& extra = json::object()) {
        json err = {{"error", {{"message", message}, {"type", type}}}};
        for (auto& [k, v] : extra.items()) {
            err["error"][k] = v;
        }
        return "data: " + err.dump() + "\n\n";
    };

    if (!loaded_) {
        std::string error_msg = sse_error("Cloud model not loaded", "model_not_loaded");
        sink.write(error_msg.c_str(), error_msg.size());
        sink.done();
        if (telemetry_callback) {
            telemetry_callback(0, 0, 0.0, 0.0, "Cloud model not loaded");
        }
        return;
    }

    // The router calls this with endpoints like "/v1/chat/completions"; strip
    // the local /v1 prefix and join with the provider's base URL.
    std::string suffix = endpoint;
    const std::string v1_prefix = "/v1";
    if (suffix.rfind(v1_prefix, 0) == 0) {
        suffix = suffix.substr(v1_prefix.size());
    }

    // Rewrite the "model" field to the provider's upstream id. If parsing
    // fails the body forwards verbatim — most providers will then 400 with
    // a body the client can interpret, which is more informative than
    // refusing locally.
    std::string forwarded_body = request_body;
    try {
        json req = json::parse(request_body);
        req["model"] = upstream_model_;
        utils::JsonUtils::add_legacy_max_tokens_alias(req);
        forwarded_body = req.dump();
    } catch (const json::exception&) {
        // Best-effort: forward whatever we got.
    }

    ResolvedCreds creds = resolve_creds();
    if (creds.insecure_http_blocked) {
        std::string error_msg = insecure_http_sse();
        sink.write(error_msg.c_str(), error_msg.size());
        sink.done();
        return;
    }
    if (creds.api_key.empty() || creds.base_url.empty()) {
        std::string error_msg = missing_creds_sse();
        sink.write(error_msg.c_str(), error_msg.size());
        sink.done();
        if (telemetry_callback) {
            telemetry_callback(0, 0, 0.0, 0.0, "Missing API credentials");
        }
        return;
    }

    std::string url = creds.base_url + suffix;

    std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + creds.api_key}
    };

    try {
        if (sse) {
            // Providers return 200 with SSE events on success, and JSON (not
            // SSE) with 4xx/5xx on auth/quota/format errors. We need clean SSE
            // output in both cases — but post_stream only surfaces the status
            // code at the end, so we discriminate by peeking at the first
            // chunk: SSE bodies start with "data:" or ":" (comment/heartbeat),
            // JSON errors start with "{" or whitespace. Stream-through if SSE;
            // buffer if it looks like an error, then emit a clean SSE error
            // envelope on the non-200 path. Holding the whole body before
            // flushing (the previous behavior) defeats streaming.
            std::string body_buffer;
            bool has_done_marker = false;
            bool streaming_mode = false;
            bool first_chunk = true;

            int input_tokens = 0;
            int output_tokens = 0;
            double time_to_first_token = 0.0;
            double tokens_per_second = 0.0;
            bool has_first_token = false;
            const auto start_time = std::chrono::steady_clock::now();
            std::string sse_line_buffer;

            auto process_cloud_line = [&](const std::string& line) {
                std::string json_str;
                if (line.find("data: ") == 0) {
                    json_str = line.substr(6);
                }
                if (!json_str.empty() && json_str != "[DONE]") {
                    try {
                        auto chunk = json::parse(json_str);
                        if (chunk.contains("usage") && !chunk["usage"].is_null()) {
                            auto usage = chunk["usage"];
                            if (usage.contains("prompt_tokens") && usage["prompt_tokens"].is_number()) {
                                input_tokens = usage["prompt_tokens"].get<int>();
                            }
                            if (usage.contains("completion_tokens") && usage["completion_tokens"].is_number()) {
                                output_tokens = usage["completion_tokens"].get<int>();
                            }
                        }
                    } catch (...) {}
                }
            };

            auto result = utils::HttpClient::post_stream(
                url,
                forwarded_body,
                [&](const char* data, size_t length) -> bool {
                    if (length == 0) return true;
                    if (first_chunk) {
                        first_chunk = false;
                        size_t i = 0;
                        while (i < length && std::isspace(static_cast<unsigned char>(data[i]))) ++i;
                        if (i < length && (data[i] == 'd' || data[i] == ':')) {
                            streaming_mode = true;
                        }
                    }
                    if (streaming_mode) {
                        if (std::string_view(data, length).find("[DONE]") != std::string_view::npos) {
                            has_done_marker = true;
                        }

                        // Parse SSE lines
                        sse_line_buffer.append(data, length);
                        StreamingProxy::process_sse_lines(sse_line_buffer, process_cloud_line);

                        if (!has_first_token && std::string_view(data, length).find("data: ") != std::string_view::npos) {
                            has_first_token = true;
                            time_to_first_token = std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - start_time).count();
                        }

                        return sink.write(data, length);
                    }
                    body_buffer.append(data, length);
                    return true;
                },
                headers,
                timeout_seconds
            );

            if (result.curl_code != CURLE_OK) {
                if (result.curl_code == CURLE_WRITE_ERROR) {
                    LOG(WARNING, "Cloud") << "Client disconnected during stream: CURL error: " << result.curl_error << std::endl;
                    if (telemetry_callback) {
                        telemetry_callback(0, 0, 0.0, 0.0, "Client disconnected during stream");
                    }
                    return;
                } else if (result.curl_code == CURLE_PARTIAL_FILE || result.curl_code == CURLE_RECV_ERROR) {
                    if (!has_done_marker) {
                        throw std::runtime_error("backend connection failed during SSE stream before DONE: CURL error: " + result.curl_error);
                    }
                } else {
                    throw std::runtime_error("SSE stream failed: CURL error: " + result.curl_error);
                }
            }

            if (result.status_code != 200) {
                LOG(ERROR, "Cloud") << "Provider returned status " << result.status_code
                                    << ", body: " << body_buffer.substr(0, 200) << std::endl;
                json extra = {{"status_code", result.status_code}};
                std::string error_msg = sse_error(
                    "cloud (" + provider_ + ") request failed", "backend_error", extra);
                sink.write(error_msg.c_str(), error_msg.size());
                sink.done();
                if (telemetry_callback) {
                    telemetry_callback(0, 0, 0.0, 0.0, "cloud (" + provider_ + ") request failed with status " + std::to_string(result.status_code));
                }
                return;
            }

            // 200 OK: if streaming_mode is true we've already flushed everything.
            // If we somehow buffered on a 200 (provider sent non-SSE success),
            // flush the buffer now so the client at least sees the payload.
            if (!body_buffer.empty()) {
                sink.write(body_buffer.data(), body_buffer.size());
            }
            if (!has_done_marker) {
                const char* done_marker = "data: [DONE]\n\n";
                sink.write(done_marker, std::strlen(done_marker));
            }
            sink.done();

            if (telemetry_callback) {
                if (output_tokens > 0 && time_to_first_token > 0.0) {
                    double duration = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
                    double generation_duration = duration - time_to_first_token;
                    if (generation_duration > 0.0) {
                        tokens_per_second = output_tokens / generation_duration;
                    }
                }
                telemetry_callback(input_tokens, output_tokens, time_to_first_token, tokens_per_second, "");
            }
        } else {
            utils::HttpResponse result = utils::HttpClient::post_stream(
                url,
                forwarded_body,
                [&sink](const char* data, size_t length) {
                    return sink.write(data, length);
                },
                headers,
                timeout_seconds
            );
            if (result.curl_code != CURLE_OK) {
                if (result.curl_code == CURLE_WRITE_ERROR) {
                    LOG(WARNING, "Cloud") << "Client disconnected during stream: CURL error: " << result.curl_error << std::endl;
                    if (telemetry_callback) {
                        telemetry_callback(0, 0, 0.0, 0.0, "Client disconnected during stream");
                    }
                    return;
                } else {
                    throw std::runtime_error("Request failed: CURL error: " + result.curl_error);
                }
            }
            if (result.status_code != 200) {
                LOG(ERROR, "Cloud") << "Provider returned status " << result.status_code << std::endl;
                if (telemetry_callback) {
                    telemetry_callback(0, 0, 0.0, 0.0, "status_code " + std::to_string(result.status_code));
                }
            } else {
                if (telemetry_callback) {
                    telemetry_callback(0, 0, 0.0, 0.0, "");
                }
            }
            sink.done();
        }
    } catch (const std::exception& e) {
        LOG(ERROR, "Cloud") << "Streaming request failed: " << e.what() << std::endl;
        if (telemetry_callback) {
            telemetry_callback(0, 0, 0.0, 0.0, e.what());
        }
        try {
            std::string error_msg = sse_error(e.what(), "streaming_error");
            sink.write(error_msg.c_str(), error_msg.size());
            sink.done();
        } catch (...) {
            // Sink may already be closed.
        }
    }
}

utils::HttpSecurityPolicy CloudServer::discovery_policy(const std::string& base_url,
                                                        bool allow_insecure_http) {
    // The AllowInsecureHttp opt-in only applies to plaintext http:// providers.
    // An https:// provider stays HTTPS-only even if allow_insecure_http is stale
    // or accidentally set, so a redirect can never downgrade the
    // Bearer-carrying request to http.
    return allow_insecure_http && CloudProviderRegistry::is_http_base_url(base_url)
               ? utils::HttpSecurityPolicy::AllowInsecureHttp
               : utils::HttpSecurityPolicy::ExternalHttpsOnly;
}

std::vector<ModelInfo> CloudServer::discover_models(const std::string& provider,
                                                     const std::string& api_key,
                                                     const std::string& base_url,
                                                     bool allow_insecure_http) {
    std::vector<ModelInfo> models;
    if (api_key.empty()) {
        return models;
    }
    if (base_url.empty()) {
        LOG(WARNING, "Cloud") << "Skipping discovery for provider '" << provider
                              << "': no base_url configured" << std::endl;
        return models;
    }

    // Mirror the trailing-slash normalization done in load() so a config
    // entry like "https://.../v1/" doesn't produce "/v1//models".
    std::string normalized_base = base_url;
    while (!normalized_base.empty() && normalized_base.back() == '/') {
        normalized_base.pop_back();
    }
    std::string url = normalized_base + "/models";
    std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + api_key}
    };

    const auto policy = discovery_policy(normalized_base, allow_insecure_http);

    utils::HttpResponse response;
    try {
        // Short timeout: this runs synchronously inside cache build, once per
        // configured provider. The 300 s default would block model listing
        // for minutes if a provider's API is unreachable. 15 s is plenty for
        // a /v1/models response under normal conditions.
        response = utils::HttpClient::get(url, headers, /*timeout_seconds=*/15, policy);
    } catch (const std::exception& e) {
        LOG(WARNING, "Cloud") << "Model discovery failed for provider '" << provider
                              << "': " << e.what() << std::endl;
        return models;
    }

    if (response.status_code != 200) {
        LOG(WARNING, "Cloud") << "GET " << url << " returned HTTP "
                              << response.status_code
                              << " — no models discovered for provider '" << provider
                              << "'. Body: " << response.body.substr(0, 200) << std::endl;
        return models;
    }

    json body;
    try {
        body = json::parse(response.body);
    } catch (const std::exception& e) {
        LOG(WARNING, "Cloud") << "Failed to parse /v1/models response from provider '"
                              << provider << "': " << e.what() << std::endl;
        return models;
    }

    // Provider responses come in two shapes: the OpenAI envelope
    // {"object":"list","data":[...]} (OpenAI, Fireworks, OpenRouter) and a
    // bare top-level array [...] (Together AI). Accept both.
    const json* model_array = nullptr;
    if (body.is_array()) {
        model_array = &body;
    } else if (body.contains("data") && body["data"].is_array()) {
        model_array = &body["data"];
    } else {
        LOG(WARNING, "Cloud") << "/v1/models response from provider '" << provider
                              << "' is neither a JSON array nor an object with a 'data' array"
                              << std::endl;
        return models;
    }

    for (const auto& m : *model_array) {
        // Chat-only by design; embeddings/audio/reranking/image belong in
        // sibling backends with diverging wire formats.
        if (!is_chat_model(m)) {
            continue;
        }
        std::string upstream_id = m["id"].get<std::string>();

        ModelInfo info;
        info.model_name = build_public_name(provider, upstream_id);
        info.checkpoints["main"] = upstream_id;
        info.recipe = "cloud";
        info.cloud_provider = provider;
        // Mark suggested so the Model Manager's default suggested-only filter
        // doesn't hide every cloud model the user explicitly configured.
        info.suggested = true;
        info.downloaded = true;  // Cloud models have no local artifacts.
        info.size = 0.0;
        info.type = ModelType::LLM;
        info.device = DEVICE_NONE;
        info.labels = chat_labels();
        for (auto& cap : capability_labels(m)) {
            info.labels.push_back(std::move(cap));
        }
        // Display-only metadata; never affects routing.
        if (m.contains("context_length") && m["context_length"].is_number_integer()) {
            info.max_context_window = m["context_length"].get<int64_t>();
        }
        const auto cost = parse_cloud_cost(m);
        info.cost_input_per_million = cost.first;
        info.cost_output_per_million = cost.second;
        models.push_back(std::move(info));
    }

    LOG(INFO, "Cloud") << "Discovered " << models.size()
                       << " model(s) from provider '" << provider
                       << "' via " << url << std::endl;
    return models;
}

} // namespace backends
} // namespace lemon

namespace lemon {
namespace backends {
namespace cloud {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return std::make_unique<CloudServer>(
        ctx.model_info->cloud_provider, ctx.log_level,
        ctx.model_manager, ctx.backend_manager, ctx.cloud_registry);
}


namespace {
class CloudOps : public BackendOps {
public:
    std::string resolve_checkpoint_path(const ModelInfo&,
                                        const CheckpointResolveContext&) const override {
        // Cloud models have no local artifacts; the checkpoint is the upstream
        // provider's model id, used directly when forwarding requests.
        return "";
    }

    bool is_downloaded(const ModelInfo&, const BackendOpsContext&) const override {
        return true;
    }

    void download_model(const ModelInfo&, bool, DownloadProgressCallback,
                        const BackendOpsContext&) const override {}

    // Discover models from each installed cloud provider with a resolvable
    // credential. Failures are logged, never propagated, so one offline
    // provider can't block discovery.
    std::vector<ModelInfo> discover_models(const BackendOpsContext& ctx) const override {
        std::vector<ModelInfo> out;
        if (ctx.cloud_registry == nullptr) {
            return out;
        }
        for (const auto& rec : ctx.cloud_registry->list_installed()) {
            const std::string api_key = ctx.cloud_registry->resolve_key(rec.name);
            if (api_key.empty() || rec.base_url.empty()) {
                LOG(INFO, "CloudOps") << "Skipping cloud discovery for '" << rec.name
                                      << "': no API key resolvable (set "
                                      << CloudProviderRegistry::env_var_name(rec.name)
                                      << " or POST /v1/cloud/auth)" << std::endl;
                continue;
            }
            // Don't send the API key to a plaintext http:// endpoint unless the
            // provider explicitly opted in (AGENTS.md invariant #11).
            if (CloudProviderRegistry::is_http_base_url(rec.base_url) && !rec.allow_insecure_http) {
                LOG(WARNING, "CloudOps") << "Skipping cloud discovery for '" << rec.name
                                         << "': http:// with API key requires allow_insecure_http=true"
                                         << std::endl;
                continue;
            }
            try {
                for (auto& m : CloudServer::discover_models(rec.name, api_key, rec.base_url,
                                                            rec.allow_insecure_http)) {
                    if (m.recipe == "cloud" && !m.model_name.empty()) {
                        out.push_back(std::move(m));
                    }
                }
            } catch (const std::exception& e) {
                LOG(WARNING, "CloudOps") << "Cloud discovery threw for '" << rec.name
                                         << "': " << e.what() << std::endl;
            }
        }
        return out;
    }
};
}  // namespace

const BackendSpec* spec() { return nullptr; }
const BackendOps* ops() { return single_ops<CloudOps>(); }
}  // namespace cloud
}  // namespace backends
}  // namespace lemon
