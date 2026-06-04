#include "lemon/backends/cloud_server.h"
#include "lemon/backends/docker_utils.h"
#include "lemon/error_types.h"
#include "lemon/runtime_config.h"
#include "lemon/streaming_proxy.h"
#include "lemon/utils/http_client.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>
#include <lemon/utils/aixlog.hpp>

namespace lemon {
namespace backends {

namespace {

bool id_contains(const std::string& id, const std::string& needle) {
    return id.find(needle) != std::string::npos;
}

// Pattern-based fallback for /v1/models entries that don't publish any
// capability metadata (notably OpenAI, whose response is just
// {id, object, owned_by, created}). The patterns cover the model
// families we currently know about:
//   - Image/video: flux, stable-diffusion, sdxl, sd-, dall-e, gpt-image,
//                  chatgpt-image, sora
//   - Audio:       whisper, tts, *-transcribe, gpt-realtime, gpt-audio
//   - Reranking:   rerank
//   - Embeddings:  embed, bge-, nomic-
//   - Classifiers: moderation
// Anything else falls through to LLM. New providers that publish
// capability metadata (see is_chat_model below) bypass this entirely
// and don't need new patterns.
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
//
// Strategy: trust provider-supplied capability metadata when it exists,
// fall back to id pattern matching only when there is none. This keeps
// the substring list bounded — adding a new provider that publishes
// capabilities does not require adding new patterns.
//
// Signals checked, in priority order:
//   1. supports_chat: bool       — Fireworks
//   2. capabilities: [string]    — generic ("chat", "chat.completions",
//                                  "embeddings", "image_generation", ...)
//   3. architecture.modality     — OpenRouter ("text->text",
//                                  "text+image->text", "text->image", ...)
//                                  Anything that produces text via chat is
//                                  considered chat-capable.
//   4. infer_type(id) == LLM     — fallback for bare responses (OpenAI).
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
// /v1/models entry and normalise the divergent fields providers use into
// lemonade's shared label vocabulary, so cloud models gate inputs exactly
// like local ones (the UI offers image upload iff "vision" is present, etc.).
//
// Strategy mirrors is_chat_model: trust structured provider metadata first,
// fall back to id patterns only for providers that publish none (OpenAI).
// When a signal is absent the capability defaults OFF — under-offering an
// input is safer than letting the client attach an image the provider rejects
// (the per-model override exists for the cases auto-detection can't cover).
//
// Recognised signals:
//   vision — supports_image_input (Fireworks); supports_vision/vision bools;
//            architecture.input_modalities ⊇ "image" (OpenRouter);
//            modalities/input_modalities ⊇ "image".
//   tools  — supports_tools (Fireworks); supported_parameters ⊇ "tools"
//            (OpenRouter); capabilities ⊇ "tools"/"function_calling";
//            function_calling/supports_function_calling bools.
//   reason — supported_parameters ⊇ "reasoning"; reasoning/supports_reasoning.
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

    // ---- vision ----
    bool vision = flag("supports_image_input") || flag("supports_vision") ||
                  flag("vision") ||
                  array_has(m.value("modalities", json::array()), "image") ||
                  array_has(m.value("input_modalities", json::array()), "image");
    if (!vision && m.contains("architecture") && m["architecture"].is_object()) {
        vision = array_has(m["architecture"].value("input_modalities", json::array()),
                           "image");
    }

    // ---- tool-calling ----
    const json params = m.value("supported_parameters", json::array());
    const json caps = m.value("capabilities", json::array());
    bool tools = flag("supports_tools") || flag("function_calling") ||
                 flag("supports_function_calling") ||
                 array_has(params, "tools") || array_has(params, "tool_choice") ||
                 array_has(caps, "tools") || array_has(caps, "function_calling") ||
                 array_has(caps, "tool_calling");

    // ---- reasoning ----
    bool reasoning = flag("reasoning") || flag("supports_reasoning") ||
                     array_has(params, "reasoning") ||
                     array_has(params, "include_reasoning");

    // ---- id-pattern fallback for metadata-barren providers (OpenAI) ----
    // Only consulted when the entry carries no structured capability hints at
    // all, so an authoritative "false" from a provider is never overridden.
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

// Build the user-facing model name from a provider's upstream id, applying
// two universal cleanup rules (no provider-specific code):
//
//   1. Collapse "accounts/<x>/models/<y>" -> "<x>/<y>". This is a
//      content-pattern match (the GCP-style resource-path convention used
//      by Fireworks). Any provider that adopts the same shape benefits
//      automatically; providers using flat ids ("gpt-4o") or other
//      namespaces ("meta-llama/Llama-3.3-70B-Instruct-Turbo") pass through
//      untouched.
//
//   2. If the cleaned id leads with "<provider>/", strip it before adding
//      the wrapping "<provider>/" prefix — otherwise Fireworks's first-
//      party models ("fireworks/...") would render as
//      "fireworks/fireworks/...".
//
// The provider namespace is joined with a "." separator (matching the
// "user."/"extra." namespacing used elsewhere); the cleaned upstream id keeps
// its own native "/" separators.
//
// Examples:
//   provider="fireworks", id="accounts/fireworks/models/deepseek-v4-pro"
//     -> "fireworks.deepseek-v4-pro"
//   provider="fireworks", id="accounts/trilogy/models/cogsci-..."
//     -> "fireworks.trilogy/cogsci-..."
//   provider="openai",    id="gpt-4o"
//     -> "openai.gpt-4o"
//   provider="together",  id="meta-llama/Llama-3.3-70B-Instruct-Turbo"
//     -> "together.meta-llama/Llama-3.3-70B-Instruct-Turbo"
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
                         BackendManager* backend_manager)
    : WrappedServer("cloud", log_level, model_manager, backend_manager),
      provider_(provider) {}

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

    // No credential resolution at load time — keys and base URLs are per-
    // request now (see PerRequestCreds in the header). We just record the
    // upstream model id so the per-request handlers know what to rewrite
    // "model" to before forwarding.
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

CloudServer::PerRequestCreds CloudServer::extract_creds(json& request) const {
    PerRequestCreds creds;

    // 1. Per-request creds injected by server.cpp from X-Lemonade-Cloud-*
    //    headers. Strip the field before we forward upstream — providers
    //    that strict-validate (Fireworks) will 400 on unknown keys.
    if (request.contains("_lemonade_cloud_creds") &&
        request["_lemonade_cloud_creds"].is_object()) {
        const auto& injected = request["_lemonade_cloud_creds"];
        if (injected.contains("api_key") && injected["api_key"].is_string()) {
            creds.api_key = injected["api_key"].get<std::string>();
        }
        if (injected.contains("base_url") && injected["base_url"].is_string()) {
            creds.base_url = injected["base_url"].get<std::string>();
        }
        request.erase("_lemonade_cloud_creds");
    }

    // 2. Env-var fallback for any field the client did not supply. This
    //    is the operator-pre-provisioned path — a self-hosted lemond can
    //    expose a "house" cloud key to all clients without each client
    //    configuring its own.
    std::string upper = provider_;
    for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (creds.api_key.empty()) {
        std::string env_name = "LEMONADE_" + upper + "_API_KEY";
        if (const char* v = std::getenv(env_name.c_str()); v && *v) {
            creds.api_key = v;
        }
    }
    if (creds.base_url.empty()) {
        std::string env_name = "LEMONADE_" + upper + "_BASE_URL";
        if (const char* v = std::getenv(env_name.c_str()); v && *v) {
            creds.base_url = v;
        }
    }

    // 3. Docker-managed SGLang: when lemond hosts the container, external
    //    clients (CLI tools, curl, etc.) may not send X-Lemonade-Cloud-*
    //    headers. Use the live container endpoint and local API key.
    if (provider_ == docker_cloud_provider_name()) {
        const auto docker_status = get_docker_runtime_status();
        if (docker_status.running) {
            if (creds.api_key.empty()) {
                creds.api_key = docker_cloud_api_key();
            }
            if (creds.base_url.empty()) {
                creds.base_url = docker_status.base_url.empty()
                                     ? sglang_base_url()
                                     : docker_status.base_url;
            }
        }
    }

    // Strip a trailing slash so path concatenation ("base + /chat/...")
    // doesn't yield "//chat/...". Some providers (notably nginx-fronted
    // ones) 404 on the doubled slash.
    while (!creds.base_url.empty() && creds.base_url.back() == '/') {
        creds.base_url.pop_back();
    }
    return creds;
}

json CloudServer::rewrite_model_field(const json& request) const {
    json modified = request;
    modified["model"] = upstream_model_;
    // Map OpenAI's max_completion_tokens to max_tokens for providers that
    // haven't migrated yet (most accept both, but be safe).
    if (modified.contains("max_completion_tokens") && !modified.contains("max_tokens")) {
        modified["max_tokens"] = modified["max_completion_tokens"];
    }
    return modified;
}

json CloudServer::post_with_auth(const std::string& path, const json& request,
                                  const PerRequestCreds& creds, long timeout_seconds) {
    if (!loaded_) {
        return ErrorResponse::from_exception(ModelNotLoadedException(server_name_));
    }
    if (creds.api_key.empty() || creds.base_url.empty()) {
        std::string upper = provider_;
        for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return ErrorResponse::create(
            "Missing cloud credentials for provider '" + provider_ + "'. Set "
            "X-Lemonade-Cloud-Key and X-Lemonade-Cloud-Base-Url request headers, "
            "or the LEMONADE_" + upper + "_API_KEY and LEMONADE_" + upper +
            "_BASE_URL env vars on the server.",
            ErrorType::BACKEND_ERROR,
            {{"provider", provider_}}
        );
    }
    std::string base_url = creds.base_url;
    if (provider_ == docker_cloud_provider_name()) {
        base_url = normalize_sglang_base_url(base_url);
    }
    while (!base_url.empty() && base_url.back() == '/') {
        base_url.pop_back();
    }
    std::string url = base_url + path;
    std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + creds.api_key}
    };

    try {
        auto response = utils::HttpClient::post(url, request.dump(), headers, timeout_seconds);
        if (response.status_code == 200) {
            json body = json::parse(response.body);
            // Best-effort telemetry from OpenAI-shape usage.
            if (body.contains("usage") && body["usage"].is_object()) {
                const auto& usage = body["usage"];
                int prompt_tokens = usage.value("prompt_tokens", 0);
                int completion_tokens = usage.value("completion_tokens", 0);
                set_telemetry(prompt_tokens, completion_tokens, 0.0, 0.0);
                set_prompt_tokens(prompt_tokens);
            }
            return body;
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
    PerRequestCreds creds = extract_creds(modified);
    return post_with_auth("/chat/completions", modified, creds);
}

json CloudServer::completion(const json& request) {
    json modified = rewrite_model_field(request);
    PerRequestCreds creds = extract_creds(modified);
    return post_with_auth("/completions", modified, creds);
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
                                            long timeout_seconds) {
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
        return;
    }

    // The router calls this with endpoints like "/v1/chat/completions"; strip
    // the local /v1 prefix and join with the provider's base URL.
    std::string suffix = endpoint;
    const std::string v1_prefix = "/v1";
    if (suffix.rfind(v1_prefix, 0) == 0) {
        suffix = suffix.substr(v1_prefix.size());
    }

    // Parse the body once so we can both extract per-request credentials
    // (injected by server.cpp from X-Lemonade-Cloud-* headers) and rewrite
    // the "model" field. If parsing fails, fall back to env-var-only creds.
    PerRequestCreds creds;
    std::string forwarded_body = request_body;
    try {
        json req = json::parse(request_body);
        creds = extract_creds(req);
        req["model"] = upstream_model_;
        if (req.contains("max_completion_tokens") && !req.contains("max_tokens")) {
            req["max_tokens"] = req["max_completion_tokens"];
        }
        forwarded_body = req.dump();
    } catch (const json::exception&) {
        // Parse failure means no injected creds — try env vars only.
        json empty = json::object();
        creds = extract_creds(empty);
    }

    if (creds.api_key.empty() || creds.base_url.empty()) {
        std::string upper = provider_;
        for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        json extra = {{"provider", provider_}};
        std::string error_msg = sse_error(
            "Missing cloud credentials for provider '" + provider_ + "'. Set "
            "X-Lemonade-Cloud-Key and X-Lemonade-Cloud-Base-Url request headers, "
            "or the LEMONADE_" + upper + "_API_KEY and LEMONADE_" + upper +
            "_BASE_URL env vars on the server.",
            "backend_error", extra);
        sink.write(error_msg.c_str(), error_msg.size());
        sink.done();
        return;
    }

    std::string base_url = creds.base_url;
    if (provider_ == docker_cloud_provider_name()) {
        base_url = normalize_sglang_base_url(base_url);
    }
    while (!base_url.empty() && base_url.back() == '/') {
        base_url.pop_back();
    }
    std::string url = base_url + suffix;

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
            auto result = utils::HttpClient::post_stream(
                url,
                forwarded_body,
                [&](const char* data, size_t length) -> bool {
                    if (length == 0) return true;
                    if (first_chunk) {
                        first_chunk = false;
                        // Skip leading whitespace before classifying.
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
                        return sink.write(data, length);
                    }
                    body_buffer.append(data, length);
                    return true;
                },
                headers,
                timeout_seconds
            );

            if (result.status_code != 200) {
                LOG(ERROR, "Cloud") << "Provider returned status " << result.status_code
                                    << ", body: " << body_buffer.substr(0, 200) << std::endl;
                json extra = {{"status_code", result.status_code}};
                std::string error_msg = sse_error(
                    "cloud (" + provider_ + ") request failed", "backend_error", extra);
                sink.write(error_msg.c_str(), error_msg.size());
                sink.done();
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
        } else {
            auto result = utils::HttpClient::post_stream(
                url,
                forwarded_body,
                [&sink](const char* data, size_t length) {
                    return sink.write(data, length);
                },
                headers,
                timeout_seconds
            );
            if (result.status_code != 200) {
                LOG(ERROR, "Cloud") << "Provider returned status " << result.status_code << std::endl;
            }
            sink.done();
        }
    } catch (const std::exception& e) {
        LOG(ERROR, "Cloud") << "Streaming request failed: " << e.what() << std::endl;
        try {
            std::string error_msg = sse_error(e.what(), "streaming_error");
            sink.write(error_msg.c_str(), error_msg.size());
            sink.done();
        } catch (...) {
            // Sink may already be closed.
        }
    }
}

std::vector<ModelInfo> CloudServer::discover_models(const std::string& provider,
                                                     const std::string& api_key,
                                                     const std::string& base_url) {
    std::vector<ModelInfo> models;
    if (api_key.empty()) {
        return models;
    }
    if (base_url.empty()) {
        LOG(WARNING, "Cloud") << "Skipping discovery for provider '" << provider
                              << "': no base_url configured" << std::endl;
        return models;
    }

    std::string normalized_base = base_url;
    if (provider == docker_cloud_provider_name()) {
        normalized_base = normalize_sglang_base_url(base_url);
    }
    while (!normalized_base.empty() && normalized_base.back() == '/') {
        normalized_base.pop_back();
    }
    std::string url = normalized_base + "/models";
    std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + api_key}
    };

    utils::HttpResponse response;
    try {
        // Short timeout: this runs synchronously inside cache build, once per
        // configured provider. The 300 s default would block model listing
        // for minutes if a provider's API is unreachable. 15 s is plenty for
        // a /v1/models response under normal conditions.
        response = utils::HttpClient::get(url, headers, /*timeout_seconds=*/15);
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
        // Chat-only by design. CloudServer implements chat_completion /
        // completion against OpenAI v1; embeddings, audio, reranking, and
        // image use diverging wire formats across providers and belong in
        // sibling backends. is_chat_model() trusts provider-supplied
        // capability metadata first (supports_chat, capabilities,
        // architecture.modality) and falls back to id pattern matching for
        // bare responses, so the router never sees a cloud model it cannot
        // dispatch.
        if (!is_chat_model(m)) {
            continue;
        }
        std::string upstream_id = m["id"].get<std::string>();

        ModelInfo info;
        // Public name = "<provider>.<cleaned_upstream_id>". The cleanup
        // rules in build_public_name() are content-pattern based and apply
        // universally to any provider — see the function comment for the
        // examples and rationale.
        info.model_name = build_public_name(provider, upstream_id);
        info.checkpoints["main"] = upstream_id;
        info.recipe = "cloud";
        info.cloud_provider = provider;
        // Discovered models are "suggested" because the user explicitly
        // configured this provider — they wouldn't have a working API key
        // otherwise. Without this, the Model Manager UI's default
        // suggested-only filter hides every cloud model.
        info.suggested = true;
        info.downloaded = true;  // Cloud models have no local artifacts.
        info.size = 0.0;
        info.type = ModelType::LLM;
        info.device = DEVICE_NONE;
        info.labels = chat_labels();
        for (auto& cap : capability_labels(m)) {
            info.labels.push_back(std::move(cap));
        }
        // Static metadata the providers publish (all three give context_length;
        // OpenRouter/Together also give pricing). Surfaced in /models, /health
        // and the discover response — display only, never affects routing.
        if (provider == docker_cloud_provider_name()) {
            info.max_context_window = sglang_max_context_window();
        } else if (m.contains("context_length") && m["context_length"].is_number_integer()) {
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
