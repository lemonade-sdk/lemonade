#include "lemon/backends/cloud_server.h"
#include "lemon/error_types.h"
#include "lemon/runtime_config.h"
#include "lemon/streaming_proxy.h"
#include "lemon/utils/http_client.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <lemon/utils/aixlog.hpp>

namespace lemon {
namespace backends {

namespace {

bool id_contains(const std::string& id, const std::string& needle) {
    return id.find(needle) != std::string::npos;
}

// Infer model type from a model id. Providers don't return a structured
// type field on /v1/models, so we deny-list non-chat substrings; anything
// that does not match falls through to LLM. Caller in discover_models()
// keeps only LLM and drops the rest, since CloudServer is chat-only.
//
// The patterns cover the major providers we care about:
//   - Fireworks/Together/OpenRouter: flux, stable-diffusion, sdxl, sd-,
//     bge-, nomic-, rerank, whisper, embed
//   - OpenAI: dall-e-*, gpt-image-*, chatgpt-image-*, sora-* (image/video);
//     tts-*, *-tts, *-transcribe, gpt-realtime-*, gpt-audio-* (audio);
//     omni-moderation-* / text-moderation-* (classifiers); text-embedding-*
ModelType infer_type(const std::string& id) {
    // Image / video generation — no common /v1/images shape across providers.
    if (id_contains(id, "flux") || id_contains(id, "stable-diffusion") ||
        id_contains(id, "sdxl") || id_contains(id, "sd-") ||
        id_contains(id, "dall-e") || id_contains(id, "gpt-image") ||
        id_contains(id, "chatgpt-image") || id_contains(id, "sora")) {
        return ModelType::IMAGE;
    }
    // Audio: ASR (whisper, *-transcribe), TTS (tts, *-tts), and the realtime
    // voice / audio-in-out variants that don't speak plain text chat.
    if (id_contains(id, "whisper") || id_contains(id, "tts") ||
        id_contains(id, "transcribe") || id_contains(id, "realtime") ||
        id_contains(id, "audio")) {
        return ModelType::AUDIO;
    }
    if (id_contains(id, "rerank")) {
        return ModelType::RERANKING;
    }
    // Embeddings and safety classifiers — both non-chat for filter purposes.
    if (id_contains(id, "embed") || id_contains(id, "bge-") ||
        id_contains(id, "nomic-") || id_contains(id, "moderation")) {
        return ModelType::EMBEDDING;
    }
    return ModelType::LLM;
}

std::vector<std::string> chat_labels() {
    return {"cloud"};
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
// Examples:
//   provider="fireworks", id="accounts/fireworks/models/deepseek-v4-pro"
//     -> "fireworks/deepseek-v4-pro"
//   provider="fireworks", id="accounts/trilogy/models/cogsci-..."
//     -> "fireworks/trilogy/cogsci-..."
//   provider="openai",    id="gpt-4o"
//     -> "openai/gpt-4o"
//   provider="together",  id="meta-llama/Llama-3.3-70B-Instruct-Turbo"
//     -> "together/meta-llama/Llama-3.3-70B-Instruct-Turbo"
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

    return provider + "/" + cleaned;
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

    upstream_model_ = model_info.checkpoint();

    auto* cfg = RuntimeConfig::global();
    if (!cfg || !cfg->cloud_offload_enabled()) {
        throw std::runtime_error(
            "Cloud offload is disabled. Set 'cloud_offload.enabled' to true in "
            "config.json to enable cloud-offloaded models.");
    }

    api_key_ = cfg->cloud_provider_api_key(provider_);
    if (api_key_.empty()) {
        std::string upper = provider_;
        for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        throw std::runtime_error(
            "No API key configured for cloud provider '" + provider_ + "'. Set "
            "the LEMONADE_" + upper + "_API_KEY environment variable, or "
            "cloud_offload.providers." + provider_ + ".api_key in config.json.");
    }

    base_url_ = cfg->cloud_provider_base_url(provider_);
    if (base_url_.empty()) {
        throw std::runtime_error(
            "No base_url configured for cloud provider '" + provider_ + "'. Set "
            "cloud_offload.providers." + provider_ + ".base_url in config.json "
            "(e.g., \"https://api.fireworks.ai/inference/v1\" for Fireworks).");
    }
    // Strip a trailing slash so that path concatenation ("base + /chat/...")
    // doesn't yield "//chat/...". Some providers (notably nginx-fronted ones)
    // 404 on the doubled slash.
    while (!base_url_.empty() && base_url_.back() == '/') {
        base_url_.pop_back();
    }

    LOG(INFO, "Cloud") << "Cloud provider: " << provider_
                       << ", upstream model: " << upstream_model_
                       << ", base_url: " << base_url_ << std::endl;
    loaded_ = true;
}

void CloudServer::unload() {
    if (loaded_) {
        LOG(INFO, "Cloud") << "Unloading cloud model: " << model_name_ << std::endl;
    }
    loaded_ = false;
    api_key_.clear();
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

json CloudServer::post_with_auth(const std::string& path, const json& request, long timeout_seconds) {
    if (!loaded_) {
        return ErrorResponse::from_exception(ModelNotLoadedException(server_name_));
    }
    std::string url = base_url_ + path;
    std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + api_key_}
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
    return post_with_auth("/chat/completions", rewrite_model_field(request));
}

json CloudServer::completion(const json& request) {
    return post_with_auth("/completions", rewrite_model_field(request));
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
    std::string url = base_url_ + suffix;

    // Rewrite the model field in the body so we forward the upstream model id.
    std::string forwarded_body = request_body;
    try {
        json req = json::parse(request_body);
        req["model"] = upstream_model_;
        if (req.contains("max_completion_tokens") && !req.contains("max_tokens")) {
            req["max_tokens"] = req["max_completion_tokens"];
        }
        forwarded_body = req.dump();
    } catch (const json::exception&) {
        // If parsing fails, forward unchanged — the provider will surface the error.
    }

    std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + api_key_}
    };

    try {
        if (sse) {
            // Buffer the body until the status code is known: providers return
            // 200 with SSE events on success, but JSON (not SSE) with 4xx/5xx
            // on auth/quota/format errors. Forwarding chunks straight through
            // and then appending an SSE error on top would produce garbled
            // output to the client. Hold the bytes; only flush them on 200.
            std::string body_buffer;
            bool has_done_marker = false;
            auto result = utils::HttpClient::post_stream(
                url,
                forwarded_body,
                [&body_buffer, &has_done_marker](const char* data, size_t length) {
                    body_buffer.append(data, length);
                    if (std::string(data, length).find("[DONE]") != std::string::npos) {
                        has_done_marker = true;
                    }
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

    if (!body.contains("data") || !body["data"].is_array()) {
        LOG(WARNING, "Cloud") << "/v1/models response from provider '" << provider
                              << "' missing 'data' array" << std::endl;
        return models;
    }

    for (const auto& m : body["data"]) {
        if (!m.is_object() || !m.contains("id") || !m["id"].is_string()) {
            continue;
        }
        std::string upstream_id = m["id"].get<std::string>();

        // Chat-only by design. CloudServer implements chat_completion /
        // completion against OpenAI v1; embeddings, audio, reranking, and
        // image use diverging wire formats across providers and belong in
        // sibling backends. infer_type() classifies upstream ids by name
        // pattern; anything that is not LLM is dropped here so the router
        // never sees a cloud model it cannot dispatch.
        if (infer_type(upstream_id) != ModelType::LLM) {
            continue;
        }

        ModelInfo info;
        // Public name = "<provider>/<cleaned_upstream_id>". The cleanup
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
        models.push_back(std::move(info));
    }

    LOG(INFO, "Cloud") << "Discovered " << models.size()
                       << " model(s) from provider '" << provider
                       << "' via " << url << std::endl;
    return models;
}

} // namespace backends
} // namespace lemon
