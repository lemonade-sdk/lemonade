#include "lemon/ollama_api.h"
#include "lemon/model_types.h"
#include "lemon/version.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <thread>

namespace lemon {

// ============================================================================
// Helper: extract parameter size from model name
// e.g. "Qwen3-0.6B-GGUF" → "0.6B", "Gemma-3-4b-it-GGUF" → "4B"
// ============================================================================
static std::string extract_parameter_size(const std::string& model_name) {
    std::string result;
    size_t i = 0;
    while (i < model_name.size()) {
        size_t seg_start = i;
        while (i < model_name.size() && model_name[i] != '-') i++;
        std::string segment = model_name.substr(seg_start, i - seg_start);
        if (i < model_name.size()) i++;  // skip hyphen

        // Match pattern: [0-9.]+[BbMm]
        if (segment.size() >= 2) {
            char last = segment.back();
            if (last == 'B' || last == 'b' || last == 'M' || last == 'm') {
                bool valid = true;
                for (size_t j = 0; j < segment.size() - 1; j++) {
                    if (!std::isdigit(segment[j]) && segment[j] != '.') {
                        valid = false;
                        break;
                    }
                }
                if (valid) {
                    // Normalize suffix to uppercase (e.g. "4b" → "4B")
                    result = segment.substr(0, segment.size() - 1) +
                             static_cast<char>(std::toupper(last));
                }
            }
        }
    }
    return result;  // returns last match (param size typically follows version numbers)
}

// ============================================================================
// Helper: extract quantization level from checkpoint string
// e.g. "unsloth/Qwen3-0.6B-GGUF:Q4_0" → "Q4_0"
//      "ggml-org/gemma-3-4b-it-GGUF:Q4_K_M" → "Q4_K_M"
//      "unsloth/gemma-3-270m-it-GGUF:gemma-3-270m-it-UD-IQ2_M.gguf" → "IQ2_M"
// ============================================================================
static std::string extract_quantization_level(const std::string& checkpoint) {
    // Search in the filename part (after colon) first, fallback to full string
    std::string search_str = checkpoint;
    size_t colon_pos = checkpoint.find(':');
    if (colon_pos != std::string::npos) {
        search_str = checkpoint.substr(colon_pos + 1);
    }

    // Look for [I]?Q[0-9][A-Za-z0-9_]* at a word boundary
    for (size_t i = 0; i < search_str.size(); i++) {
        size_t start = i;
        // Optional 'I' prefix (for IQ patterns)
        if (search_str[i] == 'I' && i + 1 < search_str.size() && search_str[i + 1] == 'Q') {
            i++;
        }
        if (search_str[i] == 'Q' && i + 1 < search_str.size() && std::isdigit(search_str[i + 1])) {
            // Check word boundary before start
            if (start > 0 && std::isalnum(search_str[start - 1])) {
                i = start;  // not a boundary, skip
                continue;
            }
            // Scan to end of quant token
            size_t end = i + 1;
            while (end < search_str.size() &&
                   (std::isdigit(search_str[end]) || std::isalpha(search_str[end]) ||
                    search_str[end] == '_')) {
                end++;
            }
            return search_str.substr(start, end - start);
        }
        i = start;  // reset to start for next iteration's i++
    }
    return "";
}

// ============================================================================
// Ollama → OpenAI option name mapping (ollama_key → openai_key)
// Options where both names are the same use identical strings.
// ============================================================================
struct OptionMapping { const char* ollama_key; const char* openai_key; };

static const OptionMapping OPTION_MAPPINGS[] = {
    {"temperature",    "temperature"},
    {"top_p",          "top_p"},
    {"seed",           "seed"},
    {"stop",           "stop"},
    {"num_predict",    "max_tokens"},
    {"repeat_penalty", "frequency_penalty"},
};

// Apply the option mappings from an Ollama request to an OpenAI request.
// Checks the "options" sub-object first, then top-level keys (top-level wins).
static void map_ollama_options(const json& ollama_request, json& openai_req) {
    // From options sub-object
    if (ollama_request.contains("options") && ollama_request["options"].is_object()) {
        const auto& opts = ollama_request["options"];
        for (const auto& m : OPTION_MAPPINGS) {
            if (opts.contains(m.ollama_key)) {
                openai_req[m.openai_key] = opts[m.ollama_key];
            }
        }
    }

    // Top-level overrides (Ollama also accepts these at the top level)
    for (const auto& m : OPTION_MAPPINGS) {
        if (ollama_request.contains(m.ollama_key)) {
            openai_req[m.openai_key] = ollama_request[m.ollama_key];
        }
    }
}

OllamaApi::OllamaApi(Router* router, ModelManager* model_manager)
    : router_(router), model_manager_(model_manager) {
}

void OllamaApi::register_routes(httplib::Server& server) {
    // Capture shared_ptr to keep OllamaApi alive as long as route handlers exist
    auto self = shared_from_this();

    // Chat completion
    server.Post("/api/chat", [self](const httplib::Request& req, httplib::Response& res) {
        self->handle_chat(req, res);
    });

    // Text generation (completion)
    server.Post("/api/generate", [self](const httplib::Request& req, httplib::Response& res) {
        self->handle_generate(req, res);
    });

    // List models
    server.Get("/api/tags", [self](const httplib::Request& req, httplib::Response& res) {
        self->handle_tags(req, res);
    });

    // Show model info
    server.Post("/api/show", [self](const httplib::Request& req, httplib::Response& res) {
        self->handle_show(req, res);
    });

    // Delete model
    server.Delete("/api/delete", [self](const httplib::Request& req, httplib::Response& res) {
        self->handle_delete(req, res);
    });

    // Pull (download) model
    server.Post("/api/pull", [self](const httplib::Request& req, httplib::Response& res) {
        self->handle_pull(req, res);
    });

    // Embeddings (new format)
    server.Post("/api/embed", [self](const httplib::Request& req, httplib::Response& res) {
        self->handle_embed(req, res);
    });

    // Embeddings (legacy format)
    server.Post("/api/embeddings", [self](const httplib::Request& req, httplib::Response& res) {
        self->handle_embeddings(req, res);
    });

    // Running models
    server.Get("/api/ps", [self](const httplib::Request& req, httplib::Response& res) {
        self->handle_ps(req, res);
    });

    // Version
    server.Get("/api/version", [self](const httplib::Request& req, httplib::Response& res) {
        self->handle_version(req, res);
    });

    // Also support HEAD on /api/version and / for Ollama client discovery
    server.Get("/", [](const httplib::Request& req, httplib::Response& res) {
        res.set_content("Ollama is running", "text/plain");
    });

    // 501 stubs for unsupported endpoints
    auto not_supported = [](const httplib::Request&, httplib::Response& res) {
        res.status = 501;
        res.set_content(R"({"error":"not supported by Lemonade"})", "application/json");
    };
    server.Post("/api/create", not_supported);
    server.Post("/api/copy", not_supported);
    server.Post("/api/push", not_supported);
    server.Post(R"(/api/blobs/(.+))", not_supported);

    std::cout << "[OllamaApi] Ollama-compatible routes registered" << std::endl;
}

// ============================================================================
// Helper: normalize model name (strip ":latest" suffix)
// ============================================================================
std::string OllamaApi::normalize_model_name(const std::string& name) {
    const std::string suffix = ":latest";
    if (name.size() > suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return name.substr(0, name.size() - suffix.size());
    }
    return name;
}

// ============================================================================
// Helper: auto-load model if needed (mirrors Server::auto_load_model_if_needed)
// ============================================================================
void OllamaApi::auto_load_model(const std::string& model) {
    std::string name = normalize_model_name(model);

    if (router_->is_model_loaded(name)) {
        return;
    }

    std::cout << "[OllamaApi] Auto-loading model: " << name << std::endl;

    if (!model_manager_->model_exists(name)) {
        throw std::runtime_error("model '" + name + "' not found");
    }

    auto info = model_manager_->get_model_info(name);

    // Download if not cached
    if (info.recipe != "flm" && !model_manager_->is_model_downloaded(name)) {
        std::cout << "[OllamaApi] Model not cached, downloading..." << std::endl;
        model_manager_->download_registered_model(info, true);
        info = model_manager_->get_model_info(name);
    }

    router_->load_model(name, info, RecipeOptions(info.recipe, json::object()), true);
    std::cout << "[OllamaApi] Model loaded: " << name << std::endl;
}

// ============================================================================
// Helper: build Ollama model entry from ModelInfo
// ============================================================================
// ============================================================================
// Helper: build Ollama "details" object from model name, recipe, and checkpoint
// ============================================================================
static json build_ollama_details(const std::string& model_name,
                                  const std::string& recipe,
                                  const std::string& checkpoint) {
    return {
        {"parent_model", ""},
        {"format", "gguf"},
        {"family", recipe},
        {"families", json::array({recipe})},
        {"parameter_size", extract_parameter_size(model_name)},
        {"quantization_level", extract_quantization_level(checkpoint)}
    };
}

json OllamaApi::build_ollama_model_entry(const std::string& id, const ModelInfo& info) {
    // Compute size in bytes (info.size is in GB)
    int64_t size_bytes = static_cast<int64_t>(info.size * 1073741824.0);  // 1 GB = 2^30 bytes

    json entry = {
        {"name", id + ":latest"},
        {"model", id + ":latest"},
        {"modified_at", "2024-01-01T00:00:00Z"},
        {"size", size_bytes},
        {"digest", "sha256:0000000000000000000000000000000000000000000000000000000000000000"},
        {"details", build_ollama_details(id, info.recipe, info.checkpoint())}
    };

    return entry;
}

// ============================================================================
// Request conversion: Ollama chat → OpenAI chat
// ============================================================================
json OllamaApi::convert_ollama_to_openai_chat(const json& ollama_request) {
    json openai_req;

    // Map model (normalize name)
    std::string model = normalize_model_name(ollama_request.value("model", ""));
    openai_req["model"] = model;

    // Map messages
    if (ollama_request.contains("messages")) {
        json messages = json::array();
        for (const auto& msg : ollama_request["messages"]) {
            json openai_msg;
            openai_msg["role"] = msg.value("role", "user");

            // Handle content - could be string or have images
            if (msg.contains("images") && msg["images"].is_array() && !msg["images"].empty()) {
                // Multimodal: convert to OpenAI content array format
                json content_parts = json::array();
                if (msg.contains("content") && !msg["content"].get<std::string>().empty()) {
                    content_parts.push_back({{"type", "text"}, {"text", msg["content"]}});
                }
                for (const auto& img : msg["images"]) {
                    content_parts.push_back({
                        {"type", "image_url"},
                        {"image_url", {{"url", "data:image/png;base64," + img.get<std::string>()}}}
                    });
                }
                openai_msg["content"] = content_parts;
            } else {
                openai_msg["content"] = msg.value("content", "");
            }

            // Forward tool_calls if present
            if (msg.contains("tool_calls")) {
                openai_msg["tool_calls"] = msg["tool_calls"];
            }

            messages.push_back(openai_msg);
        }
        openai_req["messages"] = messages;
    }

    // Map options (from "options" sub-object and top-level)
    map_ollama_options(ollama_request, openai_req);

    // Map tools if present
    if (ollama_request.contains("tools")) {
        openai_req["tools"] = ollama_request["tools"];
    }

    // Map format: "json" → response_format
    if (ollama_request.contains("format") && ollama_request["format"].is_string() &&
        ollama_request["format"].get<std::string>() == "json") {
        openai_req["response_format"] = {{"type", "json_object"}};
    }

    // Map think parameter → enable_thinking (controls reasoning output)
    if (ollama_request.contains("think")) {
        openai_req["enable_thinking"] = ollama_request["think"];
    }

    // Stream flag is handled by the caller
    openai_req["stream"] = false;

    return openai_req;
}

// ============================================================================
// Request conversion: Ollama generate → OpenAI completion
// ============================================================================
json OllamaApi::convert_ollama_to_openai_completion(const json& ollama_request) {
    json openai_req;

    std::string model = normalize_model_name(ollama_request.value("model", ""));
    openai_req["model"] = model;

    // For /api/generate, if there's a "system" field and "prompt", combine into messages
    // for chat completion (since many backends don't support raw completion)
    if (ollama_request.contains("prompt")) {
        openai_req["prompt"] = ollama_request["prompt"];
    }

    // Map options (from "options" sub-object and top-level)
    map_ollama_options(ollama_request, openai_req);

    openai_req["stream"] = false;

    return openai_req;
}

// ============================================================================
// Response conversion: OpenAI chat response → Ollama chat response
// ============================================================================
json OllamaApi::convert_openai_chat_to_ollama(const json& openai_response, const std::string& model) {
    json ollama_res;
    ollama_res["model"] = model;
    ollama_res["created_at"] = "2024-01-01T00:00:00Z";

    // Extract message from first choice
    if (openai_response.contains("choices") && !openai_response["choices"].empty()) {
        const auto& choice = openai_response["choices"][0];
        if (choice.contains("message")) {
            const auto& message = choice["message"];
            json msg;
            msg["role"] = (message.contains("role") && message["role"].is_string())
                          ? message["role"].get<std::string>() : "assistant";
            msg["content"] = (message.contains("content") && message["content"].is_string())
                             ? message["content"].get<std::string>() : "";

            // Map reasoning_content → thinking (Ollama uses "thinking" for reasoning models)
            if (message.contains("reasoning_content") && message["reasoning_content"].is_string()) {
                msg["thinking"] = message["reasoning_content"].get<std::string>();
            }

            // Forward tool_calls if present
            if (message.contains("tool_calls")) {
                msg["tool_calls"] = message["tool_calls"];
            }

            ollama_res["message"] = msg;
        }

        ollama_res["done_reason"] = (choice.contains("finish_reason") && choice["finish_reason"].is_string())
                                    ? choice["finish_reason"].get<std::string>() : "stop";
    }

    ollama_res["done"] = true;

    // Extract usage → Ollama duration/count fields
    if (openai_response.contains("usage")) {
        const auto& usage = openai_response["usage"];
        int prompt_tokens = usage.value("prompt_tokens", 0);
        int completion_tokens = usage.value("completion_tokens", 0);

        ollama_res["prompt_eval_count"] = prompt_tokens;
        ollama_res["eval_count"] = completion_tokens;

        // Provide reasonable timing estimates (nanoseconds)
        // These are approximations since the OpenAI response doesn't include timings
        ollama_res["total_duration"] = 0;
        ollama_res["load_duration"] = 0;
        ollama_res["prompt_eval_duration"] = 0;
        ollama_res["eval_duration"] = 0;
    }

    // If timings are available (from llama.cpp backends), use them
    if (openai_response.contains("timings")) {
        const auto& timings = openai_response["timings"];
        if (timings.contains("prompt_n"))
            ollama_res["prompt_eval_count"] = timings["prompt_n"];
        if (timings.contains("predicted_n"))
            ollama_res["eval_count"] = timings["predicted_n"];
        if (timings.contains("prompt_ms"))
            ollama_res["prompt_eval_duration"] = static_cast<int64_t>(timings["prompt_ms"].get<double>() * 1000000);
        if (timings.contains("predicted_ms"))
            ollama_res["eval_duration"] = static_cast<int64_t>(timings["predicted_ms"].get<double>() * 1000000);
    }

    return ollama_res;
}

// ============================================================================
// Response conversion: OpenAI streaming delta → Ollama streaming chunk
// ============================================================================
json OllamaApi::convert_openai_delta_to_ollama(const json& openai_chunk, const std::string& model) {
    json ollama_chunk;
    ollama_chunk["model"] = model;
    ollama_chunk["created_at"] = "2024-01-01T00:00:00Z";
    ollama_chunk["done"] = false;

    // Extract delta content
    if (openai_chunk.contains("choices") && !openai_chunk["choices"].empty()) {
        const auto& choice = openai_chunk["choices"][0];
        if (choice.contains("delta")) {
            const auto& delta = choice["delta"];
            json msg;
            msg["role"] = (delta.contains("role") && delta["role"].is_string())
                          ? delta["role"].get<std::string>() : "assistant";
            msg["content"] = (delta.contains("content") && delta["content"].is_string())
                             ? delta["content"].get<std::string>() : "";

            // Map reasoning_content → thinking (Ollama uses "thinking" for reasoning models)
            if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
                msg["thinking"] = delta["reasoning_content"].get<std::string>();
            }

            if (delta.contains("tool_calls")) {
                msg["tool_calls"] = delta["tool_calls"];
            }

            ollama_chunk["message"] = msg;
        }

        // Check for finish_reason
        if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
            ollama_chunk["done"] = true;
            ollama_chunk["done_reason"] = choice["finish_reason"];
        }
    }

    return ollama_chunk;
}

// ============================================================================
// Common streaming adapter: SSE → NDJSON
// Parses OpenAI SSE stream, converts each chunk via convert_chunk, writes
// NDJSON to the client, and sends a final done message via build_done.
// ============================================================================
void OllamaApi::stream_sse_to_ndjson(const std::string& openai_body,
                                      httplib::DataSink& client_sink,
                                      ChunkConverter convert_chunk,
                                      DoneBuilder build_done,
                                      StreamFn call_router) {
    httplib::DataSink adapter_sink;
    std::string sse_buffer;
    int eval_count = 0;
    int prompt_eval_count = 0;

    adapter_sink.is_writable = client_sink.is_writable;

    adapter_sink.write = [&client_sink, &sse_buffer, &eval_count,
                          &prompt_eval_count, &convert_chunk](const char* data, size_t len) -> bool {
        sse_buffer.append(data, len);

        size_t pos;
        while ((pos = sse_buffer.find('\n')) != std::string::npos) {
            std::string line = sse_buffer.substr(0, pos);
            sse_buffer.erase(0, pos + 1);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (line.empty() || line.find("data: ") != 0) {
                continue;
            }

            std::string json_str = line.substr(6);
            if (json_str == "[DONE]") {
                continue;
            }

            try {
                auto openai_chunk = json::parse(json_str);

                // Track token counts from usage in final chunk
                if (openai_chunk.contains("usage")) {
                    const auto& usage = openai_chunk["usage"];
                    if (usage.contains("prompt_tokens"))
                        prompt_eval_count = usage["prompt_tokens"].get<int>();
                    if (usage.contains("completion_tokens"))
                        eval_count = usage["completion_tokens"].get<int>();
                }

                auto ollama_chunk = convert_chunk(openai_chunk);
                std::string ndjson = ollama_chunk.dump() + "\n";
                if (!client_sink.write(ndjson.c_str(), ndjson.size())) {
                    return false;
                }
            } catch (const std::exception& e) {
                std::cerr << "[OllamaApi] Failed to parse SSE chunk: " << e.what() << std::endl;
            }
        }
        return true;
    };

    adapter_sink.done = [&client_sink, &eval_count, &prompt_eval_count, &build_done]() {
        json done_msg = build_done(prompt_eval_count, eval_count);
        std::string ndjson = done_msg.dump() + "\n";
        client_sink.write(ndjson.c_str(), ndjson.size());
        client_sink.done();
    };

    call_router(openai_body, adapter_sink);
}

// ============================================================================
// POST /api/chat — Ollama chat completion
// ============================================================================
void OllamaApi::handle_chat(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = json::parse(req.body);

        std::string model = normalize_model_name(request_json.value("model", ""));
        if (model.empty()) {
            res.status = 400;
            res.set_content(R"({"error":"model is required"})", "application/json");
            return;
        }

        // Unload model if empty messages + keep_alive=0 (Ollama unload convention)
        auto messages = request_json.value("messages", json::array());
        if (messages.empty() && request_json.contains("keep_alive") &&
            request_json["keep_alive"] == 0) {
            std::cout << "[OllamaApi] POST /api/chat - Unloading model: " << model << std::endl;
            try {
                router_->unload_model(model);
            } catch (...) {
                // Model may not be loaded, that's fine
            }
            json ollama_res = {
                {"model", model},
                {"created_at", "2024-01-01T00:00:00Z"},
                {"message", {{"role", "assistant"}, {"content", ""}}},
                {"done", true},
                {"done_reason", "unload"}
            };
            res.set_content(ollama_res.dump(), "application/json");
            return;
        }

        // Auto-load the model
        try {
            auto_load_model(model);
        } catch (const std::exception& e) {
            res.status = 404;
            json error = {{"error", "model '" + model + "' not found, try pulling it first"}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        // Determine streaming
        bool stream = request_json.value("stream", true);  // Ollama defaults to streaming

        // Convert to OpenAI format
        auto openai_req = convert_ollama_to_openai_chat(request_json);

        if (stream) {
            std::cout << "[OllamaApi] POST /api/chat - Streaming (model: " << model << ")" << std::endl;

            // Set streaming body as OpenAI format with stream=true
            openai_req["stream"] = true;
            std::string openai_body = openai_req.dump();

            res.set_chunked_content_provider(
                "application/x-ndjson",
                [this, openai_body, model](size_t offset, httplib::DataSink& sink) {
                    if (offset > 0) return false;
                    stream_sse_to_ndjson(openai_body, sink,
                        // Convert each SSE chunk to Ollama chat format
                        [this, &model](const json& chunk) {
                            return convert_openai_delta_to_ollama(chunk, model);
                        },
                        // Build final done message
                        [&model](int prompt_eval_count, int eval_count) -> json {
                            return {
                                {"model", model}, {"created_at", "2024-01-01T00:00:00Z"},
                                {"message", {{"role", "assistant"}, {"content", ""}}},
                                {"done", true}, {"done_reason", "stop"},
                                {"total_duration", 0}, {"load_duration", 0},
                                {"prompt_eval_count", prompt_eval_count}, {"prompt_eval_duration", 0},
                                {"eval_count", eval_count}, {"eval_duration", 0}
                            };
                        },
                        // Router stream function
                        [this](const std::string& body, httplib::DataSink& s) {
                            router_->chat_completion_stream(body, s);
                        }
                    );
                    return false;
                }
            );
        } else {
            std::cout << "[OllamaApi] POST /api/chat - Non-streaming (model: " << model << ")" << std::endl;

            auto openai_response = router_->chat_completion(openai_req);
            auto ollama_response = convert_openai_chat_to_ollama(openai_response, model);
            res.set_content(ollama_response.dump(), "application/json");
        }

    } catch (const std::exception& e) {
        std::cerr << "[OllamaApi] Error in /api/chat: " << e.what() << std::endl;
        res.status = 500;
        json error = {{"error", std::string(e.what())}};
        res.set_content(error.dump(), "application/json");
    }
}

// ============================================================================
// POST /api/generate — Ollama text generation
// ============================================================================
void OllamaApi::handle_generate(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = json::parse(req.body);

        std::string model = normalize_model_name(request_json.value("model", ""));
        if (model.empty()) {
            res.status = 400;
            res.set_content(R"({"error":"model is required"})", "application/json");
            return;
        }

        // Unload model if empty prompt + keep_alive=0 (Ollama unload convention)
        std::string prompt = request_json.value("prompt", "");
        if (prompt.empty() && request_json.contains("keep_alive") &&
            request_json["keep_alive"] == 0) {
            std::cout << "[OllamaApi] POST /api/generate - Unloading model: " << model << std::endl;
            try {
                router_->unload_model(model);
            } catch (...) {
                // Model may not be loaded, that's fine
            }
            json ollama_res = {
                {"model", model},
                {"created_at", "2024-01-01T00:00:00Z"},
                {"response", ""},
                {"done", true},
                {"done_reason", "unload"}
            };
            res.set_content(ollama_res.dump(), "application/json");
            return;
        }

        try {
            auto_load_model(model);
        } catch (const std::exception& e) {
            res.status = 404;
            json error = {{"error", "model '" + model + "' not found, try pulling it first"}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        // Check if this is an image generation model
        auto model_info = model_manager_->get_model_info(model);
        ModelType model_type = get_model_type_from_labels(model_info.labels);

        if (model_type == ModelType::IMAGE) {
            // Route to image generation
            handle_generate_image(request_json, res, model);
            return;
        }

        bool stream = request_json.value("stream", true);  // Ollama defaults to streaming

        auto openai_req = convert_ollama_to_openai_completion(request_json);

        if (stream) {
            std::cout << "[OllamaApi] POST /api/generate - Streaming (model: " << model << ")" << std::endl;

            openai_req["stream"] = true;
            std::string openai_body = openai_req.dump();

            res.set_chunked_content_provider(
                "application/x-ndjson",
                [this, openai_body, model](size_t offset, httplib::DataSink& sink) {
                    if (offset > 0) return false;
                    stream_sse_to_ndjson(openai_body, sink,
                        // Convert each SSE chunk to Ollama generate format
                        [&model](const json& openai_chunk) -> json {
                            json ollama_chunk = {
                                {"model", model}, {"created_at", "2024-01-01T00:00:00Z"},
                                {"done", false}
                            };
                            if (openai_chunk.contains("choices") && !openai_chunk["choices"].empty()) {
                                const auto& choice = openai_chunk["choices"][0];
                                if (choice.contains("text"))
                                    ollama_chunk["response"] = choice["text"];
                                else if (choice.contains("delta") && choice["delta"].contains("content"))
                                    ollama_chunk["response"] = choice["delta"]["content"];
                                else
                                    ollama_chunk["response"] = "";
                                if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                                    ollama_chunk["done"] = true;
                                    ollama_chunk["done_reason"] = choice["finish_reason"];
                                }
                            }
                            return ollama_chunk;
                        },
                        // Build final done message
                        [&model](int prompt_eval_count, int eval_count) -> json {
                            return {
                                {"model", model}, {"created_at", "2024-01-01T00:00:00Z"},
                                {"response", ""}, {"done", true}, {"done_reason", "stop"},
                                {"context", json::array()},
                                {"total_duration", 0}, {"load_duration", 0},
                                {"prompt_eval_count", prompt_eval_count}, {"prompt_eval_duration", 0},
                                {"eval_count", eval_count}, {"eval_duration", 0}
                            };
                        },
                        // Router stream function
                        [this](const std::string& body, httplib::DataSink& s) {
                            router_->completion_stream(body, s);
                        }
                    );
                    return false;
                }
            );
        } else {
            std::cout << "[OllamaApi] POST /api/generate - Non-streaming (model: " << model << ")" << std::endl;

            auto openai_response = router_->completion(openai_req);

            // Convert to Ollama generate format
            json ollama_res;
            ollama_res["model"] = model;
            ollama_res["created_at"] = "2024-01-01T00:00:00Z";
            ollama_res["done"] = true;
            ollama_res["done_reason"] = "stop";

            // Extract text from completion response
            if (openai_response.contains("choices") && !openai_response["choices"].empty()) {
                const auto& choice = openai_response["choices"][0];
                ollama_res["response"] = choice.value("text", "");
            } else {
                ollama_res["response"] = "";
            }

            // Usage stats
            if (openai_response.contains("usage")) {
                const auto& usage = openai_response["usage"];
                ollama_res["prompt_eval_count"] = usage.value("prompt_tokens", 0);
                ollama_res["eval_count"] = usage.value("completion_tokens", 0);
            }

            ollama_res["total_duration"] = 0;
            ollama_res["load_duration"] = 0;
            ollama_res["prompt_eval_duration"] = 0;
            ollama_res["eval_duration"] = 0;
            ollama_res["context"] = json::array();

            res.set_content(ollama_res.dump(), "application/json");
        }

    } catch (const std::exception& e) {
        std::cerr << "[OllamaApi] Error in /api/generate: " << e.what() << std::endl;
        res.status = 500;
        json error = {{"error", std::string(e.what())}};
        res.set_content(error.dump(), "application/json");
    }
}

// ============================================================================
// Image generation helper for /api/generate (when model is an image model)
// ============================================================================
void OllamaApi::handle_generate_image(const json& request_json, httplib::Response& res, const std::string& model) {
    try {
        std::cout << "[OllamaApi] POST /api/generate - Image generation (model: " << model << ")" << std::endl;

        std::string prompt = request_json.value("prompt", "");

        // Extract image generation parameters from top-level or options
        int width = 512;
        int height = 512;
        int steps = 0;
        double cfg_scale = 0.0;
        int seed = -1;

        // Check top-level params first, then options
        if (request_json.contains("width")) {
            width = request_json["width"].get<int>();
        } else if (request_json.contains("options") && request_json["options"].contains("width")) {
            width = request_json["options"]["width"].get<int>();
        }

        if (request_json.contains("height")) {
            height = request_json["height"].get<int>();
        } else if (request_json.contains("options") && request_json["options"].contains("height")) {
            height = request_json["options"]["height"].get<int>();
        }

        if (request_json.contains("steps")) {
            steps = request_json["steps"].get<int>();
        } else if (request_json.contains("options") && request_json["options"].contains("steps")) {
            steps = request_json["options"]["steps"].get<int>();
        }

        if (request_json.contains("cfg_scale")) {
            cfg_scale = request_json["cfg_scale"].get<double>();
        } else if (request_json.contains("options") && request_json["options"].contains("cfg_scale")) {
            cfg_scale = request_json["options"]["cfg_scale"].get<double>();
        }

        if (request_json.contains("seed")) {
            seed = request_json["seed"].get<int>();
        } else if (request_json.contains("options") && request_json["options"].contains("seed")) {
            seed = request_json["options"]["seed"].get<int>();
        }

        // Build OpenAI-compatible image generation request
        json openai_req;
        openai_req["model"] = model;
        openai_req["prompt"] = prompt;
        openai_req["size"] = std::to_string(width) + "x" + std::to_string(height);
        openai_req["response_format"] = "b64_json";

        if (steps > 0) {
            openai_req["steps"] = steps;
        }
        if (cfg_scale > 0.0) {
            openai_req["cfg_scale"] = cfg_scale;
        }
        if (seed >= 0) {
            openai_req["seed"] = seed;
        }

        auto openai_response = router_->image_generations(openai_req);

        // Convert OpenAI response to Ollama format
        json ollama_res;
        ollama_res["model"] = model;
        ollama_res["created_at"] = "2024-01-01T00:00:00Z";
        ollama_res["response"] = "";
        ollama_res["done"] = true;

        // Extract base64 image from OpenAI response
        if (openai_response.contains("data") && !openai_response["data"].empty()) {
            ollama_res["image"] = openai_response["data"][0].value("b64_json", "");
        } else {
            ollama_res["image"] = "";
        }

        res.set_content(ollama_res.dump(), "application/json");

    } catch (const std::exception& e) {
        std::cerr << "[OllamaApi] Error in image generation: " << e.what() << std::endl;
        res.status = 500;
        json error = {{"error", std::string(e.what())}};
        res.set_content(error.dump(), "application/json");
    }
}

// ============================================================================
// GET /api/tags — List models
// ============================================================================
void OllamaApi::handle_tags(const httplib::Request& req, httplib::Response& res) {
    try {
        auto models = model_manager_->get_downloaded_models();

        json response;
        response["models"] = json::array();

        for (const auto& [id, info] : models) {
            response["models"].push_back(build_ollama_model_entry(id, info));
        }

        res.set_content(response.dump(), "application/json");

    } catch (const std::exception& e) {
        std::cerr << "[OllamaApi] Error in /api/tags: " << e.what() << std::endl;
        res.status = 500;
        json error = {{"error", std::string(e.what())}};
        res.set_content(error.dump(), "application/json");
    }
}

// ============================================================================
// POST /api/show — Show model info
// ============================================================================
void OllamaApi::handle_show(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = json::parse(req.body);
        std::string name = normalize_model_name(request_json.value("name", request_json.value("model", "")));

        if (name.empty()) {
            res.status = 400;
            res.set_content(R"({"error":"name is required"})", "application/json");
            return;
        }

        if (!model_manager_->model_exists(name)) {
            res.status = 404;
            json error = {{"error", "model '" + name + "' not found"}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        auto info = model_manager_->get_model_info(name);

        json response = {
            {"modelfile", "# Modelfile generated by Lemonade\nFROM " + info.checkpoint()},
            {"parameters", ""},
            {"template", ""},
            {"details", build_ollama_details(name, info.recipe, info.checkpoint())},
            {"model_info", {
                {"general.architecture", info.recipe},
                {"general.file_type", 0},
                {"general.parameter_count", 0},
                {"general.quantization_version", 0}
            }}
        };

        res.set_content(response.dump(), "application/json");

    } catch (const std::exception& e) {
        std::cerr << "[OllamaApi] Error in /api/show: " << e.what() << std::endl;
        res.status = 500;
        json error = {{"error", std::string(e.what())}};
        res.set_content(error.dump(), "application/json");
    }
}

// ============================================================================
// DELETE /api/delete — Delete a model
// ============================================================================
void OllamaApi::handle_delete(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = json::parse(req.body);
        // Ollama uses "name" field (not "model")
        std::string name = normalize_model_name(request_json.value("name", request_json.value("model", "")));

        if (name.empty()) {
            res.status = 400;
            res.set_content(R"({"error":"name is required"})", "application/json");
            return;
        }

        // Unload if loaded
        if (router_->is_model_loaded(name)) {
            router_->unload_model(name);
        }

        model_manager_->delete_model(name);

        // Ollama returns 200 with no body on success
        res.status = 200;

    } catch (const std::exception& e) {
        std::cerr << "[OllamaApi] Error in /api/delete: " << e.what() << std::endl;
        std::string error_msg = e.what();
        if (error_msg.find("not found") != std::string::npos ||
            error_msg.find("not supported") != std::string::npos) {
            res.status = 404;
        } else {
            res.status = 500;
        }
        json error = {{"error", std::string(e.what())}};
        res.set_content(error.dump(), "application/json");
    }
}

// ============================================================================
// POST /api/pull — Download a model (NDJSON progress)
// ============================================================================
void OllamaApi::handle_pull(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = json::parse(req.body);
        std::string name = normalize_model_name(request_json.value("name", request_json.value("model", "")));

        if (name.empty()) {
            res.status = 400;
            res.set_content(R"({"error":"name is required"})", "application/json");
            return;
        }

        bool stream = request_json.value("stream", true);

        if (!model_manager_->model_exists(name)) {
            res.status = 404;
            json error = {{"error", "model '" + name + "' not found"}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        std::cout << "[OllamaApi] POST /api/pull - Pulling model: " << name << std::endl;

        if (stream) {
            res.set_chunked_content_provider(
                "application/x-ndjson",
                [this, name](size_t offset, httplib::DataSink& sink) {
                    if (offset > 0) return false;

                    try {
                        // Send initial status
                        std::string init = json({{"status", "pulling manifest"}}).dump() + "\n";
                        sink.write(init.c_str(), init.size());

                        auto info = model_manager_->get_model_info(name);

                        DownloadProgressCallback progress_cb = [&sink](const DownloadProgress& p) -> bool {
                            json progress;
                            if (p.complete) {
                                progress["status"] = "success";
                            } else {
                                progress["status"] = "downloading " + p.file;
                                progress["digest"] = "sha256:" + p.file;
                                progress["completed"] = static_cast<uint64_t>(p.bytes_downloaded);
                                progress["total"] = static_cast<uint64_t>(p.bytes_total);
                            }

                            std::string ndjson = progress.dump() + "\n";
                            if (!sink.write(ndjson.c_str(), ndjson.size())) {
                                return false;
                            }
                            return true;
                        };

                        model_manager_->download_model(name, "", "", false, false, false, false, false, "",
                                                       false, progress_cb);

                        // Final success
                        std::string success = json({{"status", "success"}}).dump() + "\n";
                        sink.write(success.c_str(), success.size());

                    } catch (const std::exception& e) {
                        std::string error_msg = e.what();
                        if (error_msg != "Download cancelled") {
                            json error = {{"error", error_msg}};
                            std::string ndjson = error.dump() + "\n";
                            sink.write(ndjson.c_str(), ndjson.size());
                        }
                    }

                    sink.done();
                    return false;
                }
            );
        } else {
            // Non-streaming: block until complete
            model_manager_->download_model(name);
            json response = {{"status", "success"}};
            res.set_content(response.dump(), "application/json");
        }

    } catch (const std::exception& e) {
        std::cerr << "[OllamaApi] Error in /api/pull: " << e.what() << std::endl;
        res.status = 500;
        json error = {{"error", std::string(e.what())}};
        res.set_content(error.dump(), "application/json");
    }
}

// ============================================================================
// POST /api/embed — Embeddings (new format, returns array of embeddings)
// ============================================================================
void OllamaApi::handle_embed(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = json::parse(req.body);

        std::string model = normalize_model_name(request_json.value("model", ""));
        if (model.empty()) {
            res.status = 400;
            res.set_content(R"({"error":"model is required"})", "application/json");
            return;
        }

        try {
            auto_load_model(model);
        } catch (const std::exception& e) {
            res.status = 404;
            json error = {{"error", "model '" + model + "' not found"}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        // Convert to OpenAI embeddings format
        json openai_req;
        openai_req["model"] = model;

        // Ollama accepts "input" as string or array
        if (request_json.contains("input")) {
            openai_req["input"] = request_json["input"];
        } else {
            res.status = 400;
            res.set_content(R"({"error":"input is required"})", "application/json");
            return;
        }

        auto openai_response = router_->embeddings(openai_req);

        // Convert OpenAI response to Ollama embed format
        json ollama_res;
        ollama_res["model"] = model;
        ollama_res["embeddings"] = json::array();

        if (openai_response.contains("data") && openai_response["data"].is_array()) {
            for (const auto& item : openai_response["data"]) {
                if (item.contains("embedding")) {
                    ollama_res["embeddings"].push_back(item["embedding"]);
                }
            }
        }

        ollama_res["total_duration"] = 0;
        ollama_res["load_duration"] = 0;
        ollama_res["prompt_eval_count"] = 0;

        res.set_content(ollama_res.dump(), "application/json");

    } catch (const std::exception& e) {
        std::cerr << "[OllamaApi] Error in /api/embed: " << e.what() << std::endl;
        res.status = 500;
        json error = {{"error", std::string(e.what())}};
        res.set_content(error.dump(), "application/json");
    }
}

// ============================================================================
// POST /api/embeddings — Legacy embeddings (returns single embedding)
// ============================================================================
void OllamaApi::handle_embeddings(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = json::parse(req.body);

        std::string model = normalize_model_name(request_json.value("model", ""));
        if (model.empty()) {
            res.status = 400;
            res.set_content(R"({"error":"model is required"})", "application/json");
            return;
        }

        try {
            auto_load_model(model);
        } catch (const std::exception& e) {
            res.status = 404;
            json error = {{"error", "model '" + model + "' not found"}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        // Convert to OpenAI embeddings format
        json openai_req;
        openai_req["model"] = model;

        // Legacy format uses "prompt" instead of "input"
        if (request_json.contains("prompt")) {
            openai_req["input"] = request_json["prompt"];
        } else if (request_json.contains("input")) {
            openai_req["input"] = request_json["input"];
        } else {
            res.status = 400;
            res.set_content(R"({"error":"prompt is required"})", "application/json");
            return;
        }

        auto openai_response = router_->embeddings(openai_req);

        // Convert to legacy Ollama format (single embedding)
        json ollama_res;
        ollama_res["model"] = model;

        if (openai_response.contains("data") && openai_response["data"].is_array() &&
            !openai_response["data"].empty()) {
            ollama_res["embedding"] = openai_response["data"][0]["embedding"];
        } else {
            ollama_res["embedding"] = json::array();
        }

        res.set_content(ollama_res.dump(), "application/json");

    } catch (const std::exception& e) {
        std::cerr << "[OllamaApi] Error in /api/embeddings: " << e.what() << std::endl;
        res.status = 500;
        json error = {{"error", std::string(e.what())}};
        res.set_content(error.dump(), "application/json");
    }
}

// ============================================================================
// GET /api/ps — List running models
// ============================================================================
void OllamaApi::handle_ps(const httplib::Request& req, httplib::Response& res) {
    try {
        auto loaded = router_->get_all_loaded_models();

        json response;
        response["models"] = json::array();

        if (loaded.is_array()) {
            for (const auto& m : loaded) {
                std::string name = m.value("model_name", "");
                std::string recipe = m.value("recipe", "");
                std::string checkpoint = m.value("checkpoint", "");
                json entry = {
                    {"name", name + ":latest"},
                    {"model", name + ":latest"},
                    {"size", 0},
                    {"digest", "sha256:0000000000000000000000000000000000000000000000000000000000000000"},
                    {"details", build_ollama_details(name, recipe, checkpoint)},
                    {"expires_at", "2099-01-01T00:00:00Z"},
                    {"size_vram", 0}
                };
                response["models"].push_back(entry);
            }
        }

        res.set_content(response.dump(), "application/json");

    } catch (const std::exception& e) {
        std::cerr << "[OllamaApi] Error in /api/ps: " << e.what() << std::endl;
        res.status = 500;
        json error = {{"error", std::string(e.what())}};
        res.set_content(error.dump(), "application/json");
    }
}

// ============================================================================
// GET /api/version — Version info
// ============================================================================
void OllamaApi::handle_version(const httplib::Request& req, httplib::Response& res) {
    json response = {{"version", "0.16.1"}};
    res.set_content(response.dump(), "application/json");
}

} // namespace lemon
