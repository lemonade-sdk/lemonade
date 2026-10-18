#pragma once

#include <exception>
#include <functional>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "lemon/server/api_route.h"

namespace lemon {

class ModelLoader;
class ModelManager;

// The Ollama and Anthropic gateways answer with Lemonade's OpenAI-shaped inference,
// converted to their own wire formats. The conversions more than one gateway route
// needs live here, including the stream conversions.

using NdjsonChunkConverter = std::function<nlohmann::json(const nlohmann::json& openai_chunk)>;
using NdjsonDoneBuilder = std::function<nlohmann::json(int prompt_eval_count, int eval_count)>;

void stream_sse_to_ndjson(const std::string& openai_body, httplib::DataSink& client_sink,
                          NdjsonChunkConverter convert_chunk, NdjsonDoneBuilder build_done,
                          ApiRoute::StreamFn call_router);

void stream_openai_sse_to_anthropic_sse(const std::string& openai_body,
                                        httplib::DataSink& client_sink,
                                        const std::string& model,
                                        const std::vector<std::string>& warnings,
                                        ApiRoute::StreamFn call_router);

// The load-level options a gateway request may carry: Ollama's options.num_ctx, or a
// top-level ctx_size, which wins. Either may be null when the request does not send it.
nlohmann::json gateway_load_options(const nlohmann::json& options, const nlohmann::json& ctx_size);

// Ollama's "details" object, inferred from the model name, recipe and checkpoint.
nlohmann::json ollama_details(const std::string& model_name, const std::string& recipe,
                              const std::string& checkpoint);
nlohmann::ordered_json ollama_details_schema();

// Copies Ollama sampling options, from the request's options argument or its top level, to
// OpenAI names.
void map_ollama_options(const nlohmann::json& options, const nlohmann::json& ollama_request,
                        nlohmann::json& openai_request);

extern const RouteError kGatewayModelNotLoaded;

// Inference gateway routes load their model on first use, with the request's
// gateway_load_options(). Writes the residency conflict or kGatewayModelNotLoaded in format
// and returns false when the load fails.
bool gateway_ensure_loaded(ModelLoader& loader, const std::string& model,
                           const nlohmann::json& load_options, ErrorFormat format,
                           httplib::Response& res);

// What gateway_ensure_loaded() writes.
std::vector<RouteError> gateway_load_errors();

extern const RouteError kOllamaModelNotFound;

// Writes kOllamaModelNotFound and returns false when name is not in the registry.
bool ollama_model_registered(ModelManager& model_manager, const std::string& name,
                             httplib::Response& res);

extern const RouteError kOllamaBackendError;

// Writes an Ollama-shaped 500 when the router returned an error. Returns true if it did.
bool write_ollama_backend_error(const nlohmann::json& response, httplib::Response& res);

std::string anthropic_message_id();
std::string anthropic_stop_reason(const nlohmann::json& openai_choice);

} // namespace lemon
