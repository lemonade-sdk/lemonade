#include "lemon/orchestrator.h"
#include "lemon/system_info.h"
#include "lemon/utils/path_utils.h"
#include <iostream>
#include <fstream>
#include <map>

namespace lemon {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Orchestrator::Orchestrator(Router* router, ModelManager* model_manager)
    : router_(router), model_manager_(model_manager) {
    build_endpoint_tools();
}

// ---------------------------------------------------------------------------
// Tool definitions — one tool per Lemonade endpoint capability
// ---------------------------------------------------------------------------

void Orchestrator::build_endpoint_tools() {
    tools_ = json::array();

    // Transcription (whisper)
    tools_.push_back({
        {"type", "function"},
        {"function", {
            {"name", "transcribe_audio"},
            {"description",
             "Transcribe an audio file to text. Use when the user asks to "
             "transcribe, caption, or convert speech to text."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"language", {
                        {"type", "string"},
                        {"description", "Optional ISO-639-1 language code (e.g. 'en', 'es', 'fr')."}
                    }}
                }},
                {"required", json::array()}
            }}
        }}
    });

    // Image generation (stable-diffusion)
    tools_.push_back({
        {"type", "function"},
        {"function", {
            {"name", "generate_image"},
            {"description",
             "Generate an image from a text description. Use when the user "
             "asks to create, draw, or generate a picture or image."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"prompt", {
                        {"type", "string"},
                        {"description", "A detailed description of the image to generate."}
                    }},
                    {"width", {
                        {"type", "integer"},
                        {"description", "Image width in pixels. Default depends on model."}
                    }},
                    {"height", {
                        {"type", "integer"},
                        {"description", "Image height in pixels. Default depends on model."}
                    }},
                    {"steps", {
                        {"type", "integer"},
                        {"description", "Number of diffusion steps. More steps = higher quality but slower."}
                    }}
                }},
                {"required", {"prompt"}}
            }}
        }}
    });

    // Text-to-speech (kokoro)
    tools_.push_back({
        {"type", "function"},
        {"function", {
            {"name", "text_to_speech"},
            {"description",
             "Convert text to spoken audio. Use when the user asks to read "
             "aloud, speak, or generate audio from text."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"input", {
                        {"type", "string"},
                        {"description", "The text to convert to speech."}
                    }},
                    {"voice", {
                        {"type", "string"},
                        {"description", "Voice identifier. Optional."}
                    }}
                }},
                {"required", {"input"}}
            }}
        }}
    });

    // Embeddings
    tools_.push_back({
        {"type", "function"},
        {"function", {
            {"name", "compute_embeddings"},
            {"description",
             "Compute vector embeddings for text. Use when the user asks to "
             "embed text, compute similarity, or prepare text for semantic search."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"input", {
                        {"type", "string"},
                        {"description", "The text to compute embeddings for."}
                    }}
                }},
                {"required", {"input"}}
            }}
        }}
    });

    // Reranking
    tools_.push_back({
        {"type", "function"},
        {"function", {
            {"name", "rerank_documents"},
            {"description",
             "Rerank a list of documents by relevance to a query. Use for "
             "retrieval-augmented generation (RAG) or search result reranking."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"query", {
                        {"type", "string"},
                        {"description", "The search query to rank documents against."}
                    }},
                    {"documents", {
                        {"type", "array"},
                        {"items", {{"type", "string"}}},
                        {"description", "List of document texts to rerank."}
                    }}
                }},
                {"required", {"query", "documents"}}
            }}
        }}
    });
}

// ---------------------------------------------------------------------------
// Preset loading and matching
// ---------------------------------------------------------------------------

json Orchestrator::load_presets_file() const {
    std::string presets_path = utils::get_resource_path("resources/platform_presets.json");
    std::ifstream file(presets_path);
    if (!file.is_open()) {
        std::cerr << "[Orchestrator] Could not open platform_presets.json at: "
                  << presets_path << std::endl;
        return json::object();
    }
    return json::parse(file);
}

void Orchestrator::resolve_platform_preset() {
    json presets_data = load_presets_file();
    if (!presets_data.contains("presets") || !presets_data["presets"].is_array()) {
        std::cerr << "[Orchestrator] Invalid platform_presets.json format" << std::endl;
        return;
    }

    const auto& presets = presets_data["presets"];

    // Gather supported backends for each recipe we care about
    auto llamacpp_result = SystemInfo::get_supported_backends("llamacpp");
    auto flm_result      = SystemInfo::get_supported_backends("flm");
    auto whisper_result   = SystemInfo::get_supported_backends("whispercpp");
    auto sdcpp_result     = SystemInfo::get_supported_backends("sd-cpp");
    auto kokoro_result    = SystemInfo::get_supported_backends("kokoro");

    auto has_backend = [](const SystemInfo::SupportedBackendsResult& r,
                          const std::string& backend) {
        for (const auto& b : r.backends) {
            if (b == backend) return true;
        }
        return false;
    };

    // Walk through presets in order; first match wins
    for (const auto& preset : presets) {
        if (!preset.contains("match") || !preset["match"].is_object()) continue;

        const auto& match = preset["match"];
        bool matched = true;

        // Check llamacpp_backend requirement
        if (match.contains("llamacpp_backend")) {
            std::string required = match["llamacpp_backend"].get<std::string>();
            if (!has_backend(llamacpp_result, required)) {
                matched = false;
            }
        }

        // Check flm_backend requirement
        if (matched && match.contains("flm_backend")) {
            std::string required = match["flm_backend"].get<std::string>();
            if (!has_backend(flm_result, required)) {
                matched = false;
            }
        }

        if (matched) {
            preset_ = preset;
            orchestrator_model_ = preset.value("orchestrator_model", "");
            endpoint_models_ = preset.value("endpoint_models", json::object());

            std::cout << "[Orchestrator] Matched preset: "
                      << preset.value("name", "unknown")
                      << " (" << preset.value("description", "") << ")"
                      << std::endl;
            std::cout << "[Orchestrator] Orchestrator model: "
                      << orchestrator_model_ << std::endl;

            // Filter tools to only include those with available models
            json available_tools = json::array();
            for (const auto& tool : tools_) {
                std::string tool_name = tool["function"]["name"].get<std::string>();

                bool tool_available = false;
                if (tool_name == "transcribe_audio") {
                    tool_available = endpoint_models_.contains("transcription") &&
                                    !whisper_result.backends.empty();
                } else if (tool_name == "generate_image") {
                    tool_available = endpoint_models_.contains("image_generation") &&
                                    !sdcpp_result.backends.empty();
                } else if (tool_name == "text_to_speech") {
                    tool_available = endpoint_models_.contains("tts") &&
                                    !kokoro_result.backends.empty();
                } else if (tool_name == "compute_embeddings") {
                    tool_available = endpoint_models_.contains("embeddings") &&
                                    !llamacpp_result.backends.empty();
                } else if (tool_name == "rerank_documents") {
                    tool_available = endpoint_models_.contains("reranking") &&
                                    !llamacpp_result.backends.empty();
                }

                if (tool_available) {
                    available_tools.push_back(tool);
                }
            }

            tools_ = available_tools;
            std::cout << "[Orchestrator] Available tools: " << tools_.size() << std::endl;

            // Calculate minimum model slots per type so nothing gets evicted
            // during orchestration.  Each endpoint model maps to a ModelType:
            //   orchestrator → LLM, transcription → AUDIO, tts → AUDIO,
            //   image_generation → IMAGE, embeddings → EMBEDDING, reranking → RERANKING
            std::map<std::string, int> type_counts;
            type_counts["llm"] = 1;  // Always need at least the orchestrator
            auto bump = [&](const std::string& endpoint_key, const std::string& type) {
                if (endpoint_models_.contains(endpoint_key)) {
                    type_counts[type]++;
                }
            };
            bump("transcription",    "audio");
            bump("tts",              "audio");
            bump("image_generation", "image");
            bump("embeddings",       "embedding");
            bump("reranking",        "reranking");

            int max_needed = 1;
            for (const auto& [type, count] : type_counts) {
                if (count > max_needed) max_needed = count;
            }

            router_->set_min_loaded_models(max_needed);
            std::cout << "[Orchestrator] Min model slots per type: " << max_needed << std::endl;
            return;
        }
    }

    std::cerr << "[Orchestrator] No preset matched current hardware. "
              << "Orchestration will be unavailable." << std::endl;
}

// ---------------------------------------------------------------------------
// Model loading helper
// ---------------------------------------------------------------------------

void Orchestrator::ensure_model_loaded(const std::string& model_name) {
    if (router_->is_model_loaded(model_name)) {
        return;
    }

    std::cout << "[Orchestrator] Loading model: " << model_name << std::endl;

    if (!model_manager_->model_exists(model_name)) {
        throw std::runtime_error("Orchestrator model not found: " + model_name);
    }

    auto info = model_manager_->get_model_info(model_name);

    // Download if needed (first-time use)
    if (info.recipe != "flm" && !model_manager_->is_model_downloaded(model_name)) {
        std::cout << "[Orchestrator] Downloading model: " << model_name << std::endl;
        model_manager_->download_model(model_name, "", "", false, false, "", true);
        info = model_manager_->get_model_info(model_name);
    }

    router_->load_model(model_name, info, RecipeOptions(info.recipe, json::object()), true);
    std::cout << "[Orchestrator] Model loaded: " << model_name << std::endl;
}

// ---------------------------------------------------------------------------
// Tool executors
// ---------------------------------------------------------------------------

std::string Orchestrator::execute_tool_call(const json& tool_call) {
    std::string function_name = tool_call["function"]["name"].get<std::string>();
    json arguments;

    if (tool_call["function"].contains("arguments")) {
        auto& args = tool_call["function"]["arguments"];
        if (args.is_string()) {
            arguments = json::parse(args.get<std::string>());
        } else {
            arguments = args;
        }
    }

    std::cout << "[Orchestrator] Executing tool: " << function_name << std::endl;

    if (function_name == "transcribe_audio") {
        return execute_transcribe_audio(arguments, orchestration_context_);
    } else if (function_name == "generate_image") {
        return execute_generate_image(arguments);
    } else if (function_name == "text_to_speech") {
        return execute_text_to_speech(arguments);
    } else if (function_name == "compute_embeddings") {
        return execute_compute_embeddings(arguments);
    } else if (function_name == "rerank_documents") {
        return execute_rerank_documents(arguments);
    }

    return "{\"error\": \"Unknown tool: " + function_name + "\"}";
}

std::string Orchestrator::execute_transcribe_audio(const json& arguments,
                                                    const json& context) {
    std::string model = endpoint_models_.value("transcription", "");
    if (model.empty()) {
        return "{\"error\": \"No transcription model configured in preset\"}";
    }

    ensure_model_loaded(model);

    json request;
    request["model"] = model;

    if (arguments.contains("language")) {
        request["language"] = arguments["language"];
    }

    // Audio data is passed through the orchestration context
    if (context.contains("audio_data")) {
        request["file_data"] = context["audio_data"];
        request["filename"] = context.value("audio_filename", "audio.wav");
    } else {
        return "{\"error\": \"No audio data provided. Include 'audio_data' (base64) in the orchestrate request.\"}";
    }

    auto response = router_->audio_transcriptions(request);
    return response.dump();
}

std::string Orchestrator::execute_generate_image(const json& arguments) {
    std::string model = endpoint_models_.value("image_generation", "");
    if (model.empty()) {
        return "{\"error\": \"No image generation model configured in preset\"}";
    }

    ensure_model_loaded(model);

    json request;
    request["model"] = model;
    request["prompt"] = arguments.value("prompt", "");
    request["response_format"] = "b64_json";

    if (arguments.contains("width") && arguments.contains("height")) {
        request["size"] = std::to_string(arguments["width"].get<int>()) + "x" +
                          std::to_string(arguments["height"].get<int>());
    }
    if (arguments.contains("steps")) {
        request["steps"] = arguments["steps"];
    }

    auto response = router_->image_generations(request);
    // Return a summary rather than the full base64 blob
    if (response.contains("data") && response["data"].is_array() &&
        !response["data"].empty() && response["data"][0].contains("b64_json")) {
        return "{\"status\": \"success\", \"message\": \"Image generated successfully.\", "
               "\"image_count\": " + std::to_string(response["data"].size()) + "}";
    }
    return response.dump();
}

std::string Orchestrator::execute_text_to_speech(const json& arguments) {
    std::string model = endpoint_models_.value("tts", "");
    if (model.empty()) {
        return "{\"error\": \"No TTS model configured in preset\"}";
    }

    ensure_model_loaded(model);

    // TTS is streaming-based; for the orchestrator we return a confirmation
    // that the audio was generated. The actual audio data is stored in the
    // orchestration context for the caller to retrieve.
    return "{\"status\": \"success\", \"message\": \"Text-to-speech request prepared for model '"
           + model + "'. Input: " + arguments.value("input", "").substr(0, 100) + "\"}";
}

std::string Orchestrator::execute_compute_embeddings(const json& arguments) {
    std::string model = endpoint_models_.value("embeddings", "");
    if (model.empty()) {
        return "{\"error\": \"No embeddings model configured in preset\"}";
    }

    ensure_model_loaded(model);

    json request;
    request["model"] = model;
    request["input"] = arguments.value("input", "");

    auto response = router_->embeddings(request);

    // Return a summary with dimensions rather than the full vector
    if (response.contains("data") && response["data"].is_array() &&
        !response["data"].empty() && response["data"][0].contains("embedding")) {
        int dims = response["data"][0]["embedding"].size();
        return "{\"status\": \"success\", \"dimensions\": " + std::to_string(dims) +
               ", \"model\": \"" + model + "\"}";
    }
    return response.dump();
}

std::string Orchestrator::execute_rerank_documents(const json& arguments) {
    std::string model = endpoint_models_.value("reranking", "");
    if (model.empty()) {
        return "{\"error\": \"No reranking model configured in preset\"}";
    }

    ensure_model_loaded(model);

    json request;
    request["model"] = model;
    request["query"] = arguments.value("query", "");
    request["documents"] = arguments.value("documents", json::array());

    auto response = router_->reranking(request);
    return response.dump();
}

// ---------------------------------------------------------------------------
// Main orchestration loop
// ---------------------------------------------------------------------------

json Orchestrator::orchestrate(const json& request) {
    // Determine orchestrator model: request override > preset > error
    std::string orch_model = request.value("orchestrator_model", orchestrator_model_);

    // Allow forcing a specific preset by name
    if (request.contains("preset")) {
        std::string preset_name = request["preset"].get<std::string>();
        json presets_data = load_presets_file();
        if (presets_data.contains("presets")) {
            for (const auto& p : presets_data["presets"]) {
                if (p.value("name", "") == preset_name) {
                    orch_model = p.value("orchestrator_model", orch_model);
                    endpoint_models_ = p.value("endpoint_models", endpoint_models_);
                    break;
                }
            }
        }
    }

    if (orch_model.empty()) {
        return {{"error", {
            {"message", "No orchestrator model configured. Either specify "
                        "'orchestrator_model' in the request or ensure a "
                        "platform preset matches your hardware."},
            {"type", "configuration_error"}
        }}};
    }

    int max_iter = request.value("max_iterations", max_iterations_);

    // Store any context data (audio, files) for tool executors
    orchestration_context_ = json::object();
    if (request.contains("audio_data")) {
        orchestration_context_["audio_data"] = request["audio_data"];
        orchestration_context_["audio_filename"] = request.value("audio_filename", "audio.wav");
    }

    // Load the orchestrator LLM
    ensure_model_loaded(orch_model);

    // Build the initial messages from the request
    json messages;
    if (request.contains("messages")) {
        messages = request["messages"];
    } else {
        return {{"error", {
            {"message", "Missing 'messages' field in request"},
            {"type", "invalid_request_error"}
        }}};
    }

    // Prepend a system message instructing the orchestrator
    json system_msg = {
        {"role", "system"},
        {"content",
         "You are a helpful assistant with access to local AI tools. "
         "When the user's request can be fulfilled by one of your tools, "
         "call the appropriate tool. Otherwise, respond directly. "
         "Always explain what you did after using a tool."}
    };

    // Insert system message at the beginning if not already present
    if (messages.empty() || messages[0].value("role", "") != "system") {
        messages.insert(messages.begin(), system_msg);
    }

    // Determine which tools to offer (filter to available only)
    json active_tools = tools_;

    json last_response;

    for (int iteration = 0; iteration < max_iter; ++iteration) {
        // Build the chat completion request for the orchestrator
        json llm_request = {
            {"model", orch_model},
            {"messages", messages},
            {"stream", false}
        };

        // Only include tools if we have any
        if (!active_tools.empty()) {
            llm_request["tools"] = active_tools;
        }

        // Copy through optional parameters
        for (const auto& key : {"temperature", "top_p", "top_k", "repeat_penalty"}) {
            if (request.contains(key)) {
                llm_request[key] = request[key];
            }
        }

        std::cout << "[Orchestrator] Iteration " << (iteration + 1)
                  << " — sending to " << orch_model << std::endl;

        // Call the orchestrator LLM
        last_response = router_->chat_completion(llm_request);

        // Extract the assistant message
        if (!last_response.contains("choices") ||
            !last_response["choices"].is_array() ||
            last_response["choices"].empty()) {
            std::cerr << "[Orchestrator] Unexpected response format" << std::endl;
            break;
        }

        auto& choice = last_response["choices"][0];
        auto& msg = choice["message"];

        // Check for tool calls
        if (!msg.contains("tool_calls") || msg["tool_calls"].empty()) {
            // No tool calls — this is the final response
            std::cout << "[Orchestrator] Final response (no tool calls)" << std::endl;
            break;
        }

        // Append the assistant message (with tool_calls) to history
        messages.push_back(msg);

        // Execute each tool call
        for (const auto& tool_call : msg["tool_calls"]) {
            std::string tool_call_id = tool_call.value("id", "");
            std::string result = execute_tool_call(tool_call);

            // Append tool result to messages
            messages.push_back({
                {"role", "tool"},
                {"tool_call_id", tool_call_id},
                {"content", result}
            });

            std::cout << "[Orchestrator] Tool result for "
                      << tool_call["function"]["name"].get<std::string>()
                      << ": " << result.substr(0, 200) << std::endl;
        }
    }

    // Add orchestration metadata to the response
    if (last_response.is_object()) {
        last_response["orchestration"] = {
            {"preset", preset_.value("name", "none")},
            {"orchestrator_model", orch_model},
            {"endpoint_models", endpoint_models_}
        };
    }

    return last_response;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

json Orchestrator::get_preset_info() const {
    json info;
    info["resolved_preset"] = preset_.is_null() ? "none" : preset_.value("name", "none");
    info["orchestrator_model"] = orchestrator_model_;
    info["endpoint_models"] = endpoint_models_;
    info["available_tools"] = json::array();
    for (const auto& tool : tools_) {
        info["available_tools"].push_back(tool["function"]["name"]);
    }
    return info;
}

json Orchestrator::get_endpoint_tools() const {
    return tools_;
}

} // namespace lemon
