// Inference endpoint handlers for lemon::Server
// These are Server methods extracted to a separate file for organization.
// Handlers: chat_completions, completions, embeddings, reranking, audio_transcriptions, responses

#include "lemon/server.h"
#include <iostream>
#include <iomanip>

namespace lemon {

void Server::handle_chat_completions(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);
        
        // Debug: Check if tools are present
        if (request_json.contains("tools")) {
            std::cout << "[Server DEBUG] Tools present in request: " << request_json["tools"].size() << " tool(s)" << std::endl;
            std::cout << "[Server DEBUG] Tools JSON: " << request_json["tools"].dump() << std::endl;
        } else {
            std::cout << "[Server DEBUG] No tools in request" << std::endl;
        }
        
        // Handle model loading/switching
        if (request_json.contains("model")) {
            std::string requested_model = request_json["model"];
            try {
                auto_load_model_if_needed(requested_model);
            } catch (const std::exception& e) {
                std::cerr << "[Server ERROR] Failed to load model: " << e.what() << std::endl;
                res.status = 404;
                res.set_content("{\"error\": \"" + std::string(e.what()) + "\"}", "application/json");
                return;
            }
        } else if (!router_->is_model_loaded()) {
            std::cerr << "[Server ERROR] No model loaded and no model specified in request" << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"No model loaded and no model specified in request\"}", "application/json");
            return;
        }
        
        // Check if the loaded model supports chat completion (only LLM models do)
        std::string model_to_check = request_json.contains("model") ? request_json["model"].get<std::string>() : "";
        if (router_->get_model_type(model_to_check) != ModelType::LLM) {
            std::cerr << "[Server ERROR] Model does not support chat completion" << std::endl;
            res.status = 400;
            res.set_content(R"({"error": {"message": "This model does not support chat completion. Only LLM models support this endpoint.", "type": "invalid_request_error"}})", "application/json");
            return;
        }

        // Check if streaming is requested
        bool is_streaming = request_json.contains("stream") && request_json["stream"].get<bool>();

        // Use original request body - each backend (FLM, llamacpp, etc.) handles
        // model name transformation internally via their forward methods
        std::string request_body = req.body;

        // Handle enable_thinking=false by prepending /no_think to last user message
        if (request_json.contains("enable_thinking") && 
            request_json["enable_thinking"].is_boolean() && 
            request_json["enable_thinking"].get<bool>() == false) {
            
            if (request_json.contains("messages") && request_json["messages"].is_array()) {
                auto& messages = request_json["messages"];
                
                // Find the last user message (iterate backwards)
                for (int i = messages.size() - 1; i >= 0; i--) {
                    if (messages[i].is_object() && 
                        messages[i].contains("role") && 
                        messages[i]["role"].is_string() && 
                        messages[i]["role"].get<std::string>() == "user") {
                        
                        // Prepend /no_think to the content
                        if (messages[i].contains("content") && messages[i]["content"].is_string()) {
                            std::string original_content = messages[i]["content"].get<std::string>();
                            messages[i]["content"] = "/no_think\n" + original_content;
                            
                            // Update request_body with modified JSON
                            request_body = request_json.dump();
                            break;
                        }
                    }
                }
            }
        }
        
        if (is_streaming) {
            try {
                // Log the HTTP request
                std::cout << "[Server] POST /api/v1/chat/completions - Streaming" << std::endl;
                
                // Set up streaming response with SSE headers
                res.set_header("Content-Type", "text/event-stream");
                res.set_header("Cache-Control", "no-cache");
                res.set_header("Connection", "keep-alive");
                res.set_header("X-Accel-Buffering", "no"); // Disable nginx buffering
                
                // Use cpp-httplib's chunked content provider for SSE streaming
                res.set_chunked_content_provider(
                    "text/event-stream",
                    [this, request_body](size_t offset, httplib::DataSink& sink) {
                        // For chunked responses, offset tracks bytes sent so far
                        // We only want to stream once when offset is 0
                        if (offset > 0) {
                            return false; // We're done after the first call
                        }
                        
                        // Use unified Router path for streaming
                        router_->chat_completion_stream(request_body, sink);
                        
                        // Return false to indicate we're done streaming
                        return false;
                    }
                );
            } catch (const std::exception& e) {
                std::cerr << "[Server ERROR] Streaming failed: " << e.what() << std::endl;
                res.status = 500;
                res.set_content("{\"error\":\"Internal server error during streaming\"}", "application/json");
            }
        } else {
            // Log the HTTP request
            std::cout << "[Server] POST /api/v1/chat/completions - ";
            
            auto response = router_->chat_completion(request_json);
            
            // Complete the log line with status
            std::cout << "200 OK" << std::endl;
            
            // Debug: Check if response contains tool_calls
            if (response.contains("choices") && response["choices"].is_array() && !response["choices"].empty()) {
                auto& first_choice = response["choices"][0];
                if (first_choice.contains("message")) {
                    auto& message = first_choice["message"];
                    if (message.contains("tool_calls")) {
                        std::cout << "[Server DEBUG] Response contains tool_calls: " << message["tool_calls"].dump() << std::endl;
                    } else {
                        std::cout << "[Server DEBUG] Response message does NOT contain tool_calls" << std::endl;
                        if (message.contains("content")) {
                            std::cout << "[Server DEBUG] Message content: " << message["content"].get<std::string>().substr(0, 200) << std::endl;
                        }
                    }
                }
            }
            
            res.set_content(response.dump(), "application/json");
            
            // Print and save telemetry for non-streaming
            // llama-server includes timing data in the response under "timings" field
            if (response.contains("timings")) {
                auto timings = response["timings"];
                int input_tokens = 0;
                int output_tokens = 0;
                double ttft_seconds = 0.0;
                double tps = 0.0;
                
                std::cout << "\n=== Telemetry ===" << std::endl;
                if (timings.contains("prompt_n")) {
                    input_tokens = timings["prompt_n"].get<int>();
                    std::cout << "Input tokens:  " << input_tokens << std::endl;
                }
                if (timings.contains("predicted_n")) {
                    output_tokens = timings["predicted_n"].get<int>();
                    std::cout << "Output tokens: " << output_tokens << std::endl;
                }
                if (timings.contains("prompt_ms")) {
                    ttft_seconds = timings["prompt_ms"].get<double>() / 1000.0;
                    std::cout << "TTFT (s):      " << std::fixed << std::setprecision(2) 
                             << ttft_seconds << std::endl;
                }
                if (timings.contains("predicted_per_second")) {
                    tps = timings["predicted_per_second"].get<double>();
                    std::cout << "TPS:           " << std::fixed << std::setprecision(2) 
                             << tps << std::endl;
                }
                std::cout << "=================" << std::endl;
                
                // Save telemetry to router
                router_->update_telemetry(input_tokens, output_tokens, ttft_seconds, tps);
            } else if (response.contains("usage")) {
                // OpenAI format uses "usage" field
                auto usage = response["usage"];
                int input_tokens = 0;
                int output_tokens = 0;
                double ttft_seconds = 0.0;
                double tps = 0.0;
                
                std::cout << "\n=== Telemetry ===" << std::endl;
                if (usage.contains("prompt_tokens")) {
                    input_tokens = usage["prompt_tokens"].get<int>();
                    std::cout << "Input tokens:  " << input_tokens << std::endl;
                }
                if (usage.contains("completion_tokens")) {
                    output_tokens = usage["completion_tokens"].get<int>();
                    std::cout << "Output tokens: " << output_tokens << std::endl;
                }
                
                // FLM format may include timing data
                if (usage.contains("prefill_duration_ttft")) {
                    ttft_seconds = usage["prefill_duration_ttft"].get<double>();
                    std::cout << "TTFT (s):      " << std::fixed << std::setprecision(2) 
                             << ttft_seconds << std::endl;
                }
                if (usage.contains("decoding_speed_tps")) {
                    tps = usage["decoding_speed_tps"].get<double>();
                    std::cout << "TPS:           " << std::fixed << std::setprecision(2) 
                             << tps << std::endl;
                }
                std::cout << "=================" << std::endl;
                
                // Save telemetry to router
                router_->update_telemetry(input_tokens, output_tokens, ttft_seconds, tps);
            }
            
            // Capture prompt_tokens from usage if available
            if (response.contains("usage")) {
                auto usage = response["usage"];
                if (usage.contains("prompt_tokens")) {
                    int prompt_tokens = usage["prompt_tokens"].get<int>();
                    router_->update_prompt_tokens(prompt_tokens);
                }
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[Server ERROR] Chat completion failed: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_completions(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);
        
        // Handle model loading/switching (same logic as chat_completions)
        if (request_json.contains("model")) {
            std::string requested_model = request_json["model"];
            try {
                auto_load_model_if_needed(requested_model);
            } catch (const std::exception& e) {
                std::cerr << "[Server ERROR] Failed to load model: " << e.what() << std::endl;
                res.status = 404;
                res.set_content("{\"error\": \"" + std::string(e.what()) + "\"}", "application/json");
                return;
            }
        } else if (!router_->is_model_loaded()) {
            std::cerr << "[Server ERROR] No model loaded and no model specified in request" << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"No model loaded and no model specified in request\"}", "application/json");
            return;
        }

        // Check if the loaded model supports completion (only LLM models do)
        std::string model_to_check = request_json.contains("model") ? request_json["model"].get<std::string>() : "";
        if (router_->get_model_type(model_to_check) != ModelType::LLM) {
            std::cerr << "[Server ERROR] Model does not support completion" << std::endl;
            res.status = 400;
            res.set_content(R"({"error": {"message": "This model does not support completion. Only LLM models support this endpoint.", "type": "invalid_request_error"}})", "application/json");
            return;
        }

        // Check if streaming is requested
        bool is_streaming = request_json.contains("stream") && request_json["stream"].get<bool>();

        // Use original request body - each backend handles model name transformation internally
        std::string request_body = req.body;
        
        if (is_streaming) {
            try {
                // Log the HTTP request
                std::cout << "[Server] POST /api/v1/completions - Streaming" << std::endl;
                
                // Set up SSE headers
                res.set_header("Content-Type", "text/event-stream");
                res.set_header("Cache-Control", "no-cache");
                res.set_header("Connection", "keep-alive");
                res.set_header("X-Accel-Buffering", "no");
                
                res.set_chunked_content_provider(
                    "text/event-stream",
                    [this, request_body](size_t offset, httplib::DataSink& sink) {
                        if (offset > 0) {
                            return false; // Already sent everything
                        }
                        
                        // Use unified Router path for streaming
                        router_->completion_stream(request_body, sink);
                        
                        return false; // Signal completion
                    }
                );
                
                std::cout << "[Server] Streaming completed - 200 OK" << std::endl;
                return;
                
            } catch (const std::exception& e) {
                std::cerr << "[Server ERROR] Streaming failed: " << e.what() << std::endl;
                res.status = 500;
                res.set_content("{\"error\": \"" + std::string(e.what()) + "\"}", "application/json");
                return;
            }
        } else {
            // Non-streaming
            auto response = router_->completion(request_json);
            
            // Check if response contains an error
            if (response.contains("error")) {
                std::cerr << "[Server] ERROR: Backend returned error response: " << response["error"].dump() << std::endl;
                res.status = 500;
                res.set_content(response.dump(), "application/json");
                return;
            }
            
            // Verify response has required fields
            if (!response.contains("choices")) {
                std::cerr << "[Server] ERROR: Response missing 'choices' field. Response: " << response.dump() << std::endl;
                res.status = 500;
                nlohmann::json error = {{"error", "Backend returned invalid response format"}};
                res.set_content(error.dump(), "application/json");
                return;
            }
            
            res.set_content(response.dump(), "application/json");
            
            // Print and save telemetry for non-streaming completions
            if (response.contains("timings")) {
                auto timings = response["timings"];
                int input_tokens = 0;
                int output_tokens = 0;
                double ttft_seconds = 0.0;
                double tps = 0.0;
                
                std::cout << "\n=== Telemetry ===" << std::endl;
                if (timings.contains("prompt_n")) {
                    input_tokens = timings["prompt_n"].get<int>();
                    std::cout << "Input tokens:  " << input_tokens << std::endl;
                }
                if (timings.contains("predicted_n")) {
                    output_tokens = timings["predicted_n"].get<int>();
                    std::cout << "Output tokens: " << output_tokens << std::endl;
                }
                if (timings.contains("prompt_ms")) {
                    ttft_seconds = timings["prompt_ms"].get<double>() / 1000.0;
                    std::cout << "TTFT (s):      " << std::fixed << std::setprecision(2) 
                             << ttft_seconds << std::endl;
                }
                if (timings.contains("predicted_per_second")) {
                    tps = timings["predicted_per_second"].get<double>();
                    std::cout << "TPS:           " << std::fixed << std::setprecision(2) 
                             << tps << std::endl;
                }
                std::cout << "=================" << std::endl;
                
                // Save telemetry to router
                router_->update_telemetry(input_tokens, output_tokens, ttft_seconds, tps);
            } else if (response.contains("usage")) {
                auto usage = response["usage"];
                int input_tokens = 0;
                int output_tokens = 0;
                double ttft_seconds = 0.0;
                double tps = 0.0;
                
                std::cout << "\n=== Telemetry ===" << std::endl;
                if (usage.contains("prompt_tokens")) {
                    input_tokens = usage["prompt_tokens"].get<int>();
                    std::cout << "Input tokens:  " << input_tokens << std::endl;
                }
                if (usage.contains("completion_tokens")) {
                    output_tokens = usage["completion_tokens"].get<int>();
                    std::cout << "Output tokens: " << output_tokens << std::endl;
                }
                
                // FLM format may include timing data
                if (usage.contains("prefill_duration_ttft")) {
                    ttft_seconds = usage["prefill_duration_ttft"].get<double>();
                    std::cout << "TTFT (s):      " << std::fixed << std::setprecision(2) 
                             << ttft_seconds << std::endl;
                }
                if (usage.contains("decoding_speed_tps")) {
                    tps = usage["decoding_speed_tps"].get<double>();
                    std::cout << "TPS:           " << std::fixed << std::setprecision(2) 
                             << tps << std::endl;
                }
                std::cout << "=================" << std::endl;
                
                // Save telemetry to router
                router_->update_telemetry(input_tokens, output_tokens, ttft_seconds, tps);
            }
            
            // Capture prompt_tokens from usage if available
            if (response.contains("usage")) {
                auto usage = response["usage"];
                if (usage.contains("prompt_tokens")) {
                    int prompt_tokens = usage["prompt_tokens"].get<int>();
                    router_->update_prompt_tokens(prompt_tokens);
                }
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[Server] ERROR in handle_completions: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_embeddings(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);
        
        // Handle model loading/switching using helper function
        if (request_json.contains("model")) {
            std::string requested_model = request_json["model"];
            auto_load_model_if_needed(requested_model);
        } else if (!router_->is_model_loaded()) {
            std::cerr << "[Server ERROR] No model loaded and no model specified in request" << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"No model loaded and no model specified in request\"}", "application/json");
            return;
        }
        
        // Call router's embeddings method
        auto response = router_->embeddings(request_json);
        res.set_content(response.dump(), "application/json");
        
    } catch (const std::exception& e) {
        std::cerr << "[Server] ERROR in handle_embeddings: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_reranking(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);

        // Handle model loading/switching using helper function
        if (request_json.contains("model")) {
            std::string requested_model = request_json["model"];
            auto_load_model_if_needed(requested_model);
        } else if (!router_->is_model_loaded()) {
            std::cerr << "[Server ERROR] No model loaded and no model specified in request" << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"No model loaded and no model specified in request\"}", "application/json");
            return;
        }

        // Call router's reranking method
        auto response = router_->reranking(request_json);
        res.set_content(response.dump(), "application/json");

    } catch (const std::exception& e) {
        std::cerr << "[Server] ERROR in handle_reranking: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_audio_transcriptions(const httplib::Request& req, httplib::Response& res) {
    try {
        std::cout << "[Server] POST /api/v1/audio/transcriptions" << std::endl;

        // OpenAI audio API uses multipart form data
        if (!req.is_multipart_form_data()) {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Request must be multipart/form-data"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        // Build request JSON for router
        nlohmann::json request_json;

        // Extract form fields
        if (req.form.has_field("model")) {
            request_json["model"] = req.form.get_field("model");
        }
        if (req.form.has_field("language")) {
            request_json["language"] = req.form.get_field("language");
        }
        if (req.form.has_field("prompt")) {
            request_json["prompt"] = req.form.get_field("prompt");
        }
        if (req.form.has_field("response_format")) {
            request_json["response_format"] = req.form.get_field("response_format");
        }
        if (req.form.has_field("temperature")) {
            request_json["temperature"] = std::stod(req.form.get_field("temperature"));
        }

        // Extract audio file
        const auto& files = req.form.files;
        bool found_audio = false;
        for (const auto& file_pair : files) {
            if (file_pair.first == "file") {
                const auto& file = file_pair.second;
                request_json["file_data"] = file.content;
                request_json["filename"] = file.filename;
                found_audio = true;
                std::cout << "[Server] Audio file: " << file.filename
                          << " (" << file.content.size() << " bytes)" << std::endl;
                break;
            }
        }

        if (!found_audio) {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Missing 'file' field in request"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        // Handle model loading
        if (request_json.contains("model")) {
            std::string requested_model = request_json["model"];
            try {
                auto_load_model_if_needed(requested_model);
            } catch (const std::exception& e) {
                std::cerr << "[Server ERROR] Failed to load audio model: " << e.what() << std::endl;
                res.status = 404;
                nlohmann::json error = {{"error", {
                    {"message", e.what()},
                    {"type", "model_not_found"}
                }}};
                res.set_content(error.dump(), "application/json");
                return;
            }
        } else {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Missing 'model' field in request"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        // Forward to router
        auto response = router_->audio_transcriptions(request_json);

        // Check for error in response
        if (response.contains("error")) {
            res.status = 500;
        }

        res.set_content(response.dump(), "application/json");

    } catch (const std::exception& e) {
        std::cerr << "[Server] ERROR in handle_audio_transcriptions: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", {
            {"message", e.what()},
            {"type", "internal_error"}
        }}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_responses(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);
        
        // Handle model loading/switching using helper function
        if (request_json.contains("model")) {
            std::string requested_model = request_json["model"];
            auto_load_model_if_needed(requested_model);
        } else if (!router_->is_model_loaded()) {
            std::cerr << "[Server ERROR] No model loaded and no model specified in request" << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"No model loaded and no model specified in request\"}", "application/json");
            return;
        }
        
        // Check if current model supports responses API (only oga-* recipes)
        std::string loaded_recipe = router_->get_loaded_recipe();
        if (loaded_recipe.find("oga-") == std::string::npos && loaded_recipe != "oga") {
            std::cerr << "[Server ERROR] Responses API not supported for recipe: " << loaded_recipe << std::endl;
            res.status = 422;
            nlohmann::json error_response = {
                {"error", {
                    {"message", "Responses API not supported for recipe: " + loaded_recipe},
                    {"type", "unsupported_recipe"},
                    {"code", "responses_not_supported"}
                }}
            };
            res.set_content(error_response.dump(), "application/json");
            return;
        }
        
        // Check if streaming is requested
        bool is_streaming = request_json.contains("stream") && request_json["stream"].get<bool>();
        
        if (is_streaming) {
            try {
                std::cout << "[Server] POST /api/v1/responses - Streaming" << std::endl;
                
                // Set up streaming response with SSE headers
                res.set_header("Content-Type", "text/event-stream");
                res.set_header("Cache-Control", "no-cache");
                res.set_header("Connection", "keep-alive");
                res.set_header("X-Accel-Buffering", "no");
                
                // Use cpp-httplib's chunked content provider for SSE streaming
                res.set_chunked_content_provider(
                    "text/event-stream",
                    [this, request_body = req.body](size_t offset, httplib::DataSink& sink) {
                        if (offset > 0) {
                            return false; // Only stream once
                        }
                        
                        // Use unified Router path for streaming
                        router_->responses_stream(request_body, sink);
                        
                        return false;
                    }
                );
            } catch (const std::exception& e) {
                std::cerr << "[Server ERROR] Streaming failed: " << e.what() << std::endl;
                res.status = 500;
                res.set_content("{\"error\":\"Internal server error during streaming\"}", "application/json");
            }
        } else {
            std::cout << "[Server] POST /api/v1/responses - Non-streaming" << std::endl;
            
            auto response = router_->responses(request_json);
            
            std::cout << "200 OK" << std::endl;
            res.set_content(response.dump(), "application/json");
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[Server] ERROR in handle_responses: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

} // namespace lemon

