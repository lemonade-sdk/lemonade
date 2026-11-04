#include "lemon/server/gateway_conversion.h"

#include <cctype>
#include <chrono>
#include <string>
#include <vector>

#include <lemon/utils/aixlog.hpp>

#include "lemon/anthropic_error.h"
#include "lemon/error_types.h"
#include "lemon/model_manager.h"
#include "lemon/server/model_loader.h"
#include "lemon/server/shared_args.h"

namespace lemon {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

namespace {

// e.g. "Qwen3-0.6B-GGUF" -> "0.6B", "Gemma-3-4b-it-GGUF" -> "4B"
static std::string extract_parameter_size(const std::string& model_name) {
    std::string result;
    size_t i = 0;
    while (i < model_name.size()) {
        size_t seg_start = i;
        while (i < model_name.size() && model_name[i] != '-') i++;
        std::string segment = model_name.substr(seg_start, i - seg_start);
        if (i < model_name.size()) i++;

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
                    result = segment.substr(0, segment.size() - 1) +
                             static_cast<char>(std::toupper(last));
                }
            }
        }
    }
    return result;  // returns last match (param size typically follows version numbers)
}

// e.g. "unsloth/Qwen3-0.6B-GGUF:Q4_0" -> "Q4_0",
//      "unsloth/gemma-3-270m-it-GGUF:gemma-3-270m-it-UD-IQ2_M.gguf" -> "IQ2_M"
static std::string extract_quantization_level(const std::string& checkpoint) {
    std::string search_str = checkpoint;
    size_t colon_pos = checkpoint.find(':');
    if (colon_pos != std::string::npos) {
        search_str = checkpoint.substr(colon_pos + 1);
    }

    for (size_t i = 0; i < search_str.size(); i++) {
        size_t start = i;
        if (search_str[i] == 'I' && i + 1 < search_str.size() && search_str[i + 1] == 'Q') {
            i++;
        }
        if (search_str[i] == 'Q' && i + 1 < search_str.size() && std::isdigit(search_str[i + 1])) {
            if (start > 0 && std::isalnum(search_str[start - 1])) {
                i = start;
                continue;
            }
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

} // namespace


json gateway_load_options(const json& options, const json& ctx_size) {
    json result = json::object();

    if (options.is_object() && options.contains("num_ctx")) {
        result["ctx_size"] = options["num_ctx"];
    }

    // Top-level wins over options.num_ctx, matching map_ollama_options precedence.
    if (!ctx_size.is_null()) {
        result["ctx_size"] = ctx_size;
    }

    return result;
}

json ollama_details(const std::string& model_name, const std::string& recipe,
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

ordered_json ollama_details_schema() {
    return ordered_json::parse(R"({
        "type": "object",
        "description": "Inferred from the model name, recipe and checkpoint.",
        "properties": {
            "parent_model": {"type": "string", "description": "Always empty."},
            "format": {"type": "string", "description": "Always `gguf`."},
            "family": {"type": "string", "description": "The model's recipe, e.g. `llamacpp`."},
            "families": {"type": "array", "items": {"type": "string"}, "description": "A list holding only `family`."},
            "parameter_size": {"type": "string", "description": "From the model name, e.g. `0.6B`; empty when the name has none."},
            "quantization_level": {"type": "string", "description": "From the checkpoint, e.g. `Q4_0`; empty when the checkpoint has none."}
        }
    })");
}

namespace {

struct OptionMapping { const char* ollama_key; const RouteArg& openai_arg; };

} // namespace

// Ollama accepts these both inside options and at the top level; top-level wins.
void map_ollama_options(const json& options, const json& ollama_request, json& openai_req) {
    const OptionMapping mappings[] = {
        {"temperature",    generation::kTemperature},
        {"top_p",          generation::kTopP},
        {"seed",           openai::text::kSeed},
        {"stop",           openai::text::kStop},
        {"num_predict",    openai::text::kMaxTokens},
        {"repeat_penalty", openai::text::kFrequencyPenalty},
    };
    if (options.is_object()) {
        for (const auto& m : mappings) {
            if (options.contains(m.ollama_key)) {
                openai_req[m.openai_arg.name] = options[m.ollama_key];
            }
        }
    }

    for (const auto& m : mappings) {
        if (ollama_request.contains(m.ollama_key)) {
            openai_req[m.openai_arg.name] = ollama_request[m.ollama_key];
        }
    }
}

const RouteError kGatewayModelNotLoaded{
    404, "",
    "`model` fails to load for any reason other than a residency conflict, such as naming no "
    "model or a failed download"};

namespace {

// Ollama and Anthropic clients each read the conflict type where their own errors keep it.
void write_residency_conflict(const std::exception& error, ErrorFormat format,
                              httplib::Response& res) {
    json body;
    if (format == ErrorFormat::Anthropic) {
        body = {
            {"type", "error"},
            {"error", {
                {"type", ErrorType::ROUTER_RESIDENCY_CONFLICT},
                {"message", error.what()},
                {"code", ErrorType::ROUTER_RESIDENCY_CONFLICT},
            }},
        };
    } else {
        body = {
            {"error", error.what()},
            {"type", ErrorType::ROUTER_RESIDENCY_CONFLICT},
            {"code", ErrorType::ROUTER_RESIDENCY_CONFLICT},
        };
    }
    res.status = ModelLoader::kRouterResidencyConflict.status;
    res.set_content(body.dump(), "application/json");
}

} // namespace

bool gateway_ensure_loaded(ModelLoader& loader, const std::string& model, const json& load_options,
                           ErrorFormat format, httplib::Response& res) {
    try {
        loader.ensure_loaded(model, load_options);
        return true;
    } catch (const RouterResidencyConflictException& e) {
        write_residency_conflict(e, format, res);
    } catch (const std::exception&) {
        write_error(res, format, kGatewayModelNotLoaded,
                    "model '" + model + "' not found, try pulling it first");
    }
    return false;
}

std::vector<RouteError> gateway_load_errors() {
    return {ModelLoader::kRouterResidencyConflict, kGatewayModelNotLoaded};
}

const RouteError kOllamaModelNotFound{
    404, "", "`model` names no model in the registry, or one filtered out on this system"};

bool ollama_model_registered(ModelManager& model_manager, const std::string& name,
                             httplib::Response& res) {
    if (model_manager.model_exists(name)) return true;
    write_plain_error(res, kOllamaModelNotFound, "model '" + name + "' not found");
    return false;
}

const RouteError kOllamaBackendError{500, "", "The backend reports an error, whatever its own status"};

bool write_ollama_backend_error(const json& response, httplib::Response& res) {
    if (!response.contains("error")) return false;
    LOG(ERROR, "OllamaApi") << "Backend returned error: " << response["error"].dump() << std::endl;
    res.status = kOllamaBackendError.status;
    json error = {{"error", response["error"].value("message", "backend error")}};
    res.set_content(error.dump(), "application/json");
    return true;
}

std::string anthropic_message_id() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return "msg_" + std::to_string(millis);
}

std::string anthropic_stop_reason(const json& choice) {
    std::string finish_reason = choice.value("finish_reason", "stop");

    if (finish_reason == "length") {
        return "max_tokens";
    }
    if (finish_reason == "tool_calls") {
        return "tool_use";
    }
    return "end_turn";
}


void stream_sse_to_ndjson(const std::string& openai_body, httplib::DataSink& client_sink,
                          NdjsonChunkConverter convert_chunk, NdjsonDoneBuilder build_done,
                          ApiRoute::StreamFn call_router) {
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
                LOG(ERROR, "OllamaApi") << "Failed to parse SSE chunk: " << e.what() << std::endl;
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

void stream_openai_sse_to_anthropic_sse(const std::string& openai_body,
                                        httplib::DataSink& client_sink,
                                        const std::string& model,
                                        const std::vector<std::string>& warnings,
                                        ApiRoute::StreamFn call_router) {
    httplib::DataSink adapter_sink;
    std::string sse_buffer;

    bool sent_message_start = false;
    bool sent_text_content_start = false;
    bool sent_text_content_stop = false;
    bool sent_error = false;
    std::vector<bool> started_tool_blocks;
    std::vector<bool> stopped_tool_blocks;
    std::vector<std::string> tool_ids;
    std::vector<std::string> tool_names;
    std::string stop_reason = "end_turn";
    int input_tokens = 0;
    int output_tokens = 0;
    std::string message_id = anthropic_message_id();

    adapter_sink.is_writable = client_sink.is_writable;

    adapter_sink.write = [&client_sink,
                          &sse_buffer,
                          &sent_message_start,
                          &sent_text_content_start,
                          &sent_text_content_stop,
                          &sent_error,
                          &started_tool_blocks,
                          &stopped_tool_blocks,
                          &tool_ids,
                          &tool_names,
                          &stop_reason,
                          &input_tokens,
                          &output_tokens,
                          &message_id,
                          &model](const char* data, size_t len) -> bool {
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

                if (openai_chunk.contains("error")) {
                    const auto& error = openai_chunk["error"];
                    std::cerr << "[OllamaApi] Backend error in Anthropic stream: "
                              << error.dump() << std::endl;
                    sent_error = true;
                    write_sse_event(client_sink, "error",
                                    anthropic::build_anthropic_error(
                                        error, anthropic::backend_error_http_status(error)));
                    return false;
                }

                if (!sent_message_start) {
                    if (openai_chunk.contains("id") && openai_chunk["id"].is_string()) {
                        message_id = openai_chunk["id"].get<std::string>();
                    }

                    json message_start = {
                        {"type", "message_start"},
                        {"message", {
                            {"id", message_id},
                            {"type", "message"},
                            {"role", "assistant"},
                            {"model", model},
                            {"content", json::array()},
                            {"stop_reason", nullptr},
                            {"stop_sequence", nullptr},
                            {"usage", {{"input_tokens", 0}, {"output_tokens", 0}}}
                        }}
                    };
                    if (!write_sse_event(client_sink, "message_start", message_start)) {
                        return false;
                    }
                    sent_message_start = true;
                }

                if (openai_chunk.contains("usage") && openai_chunk["usage"].is_object()) {
                    const auto& usage = openai_chunk["usage"];
                    input_tokens = usage.value("prompt_tokens", input_tokens);
                    output_tokens = usage.value("completion_tokens", output_tokens);
                }

                if (openai_chunk.contains("choices") && openai_chunk["choices"].is_array() &&
                    !openai_chunk["choices"].empty()) {
                    const auto& choice = openai_chunk["choices"][0];

                    if (choice.contains("delta") && choice["delta"].is_object()) {
                        const auto& delta = choice["delta"];
                        if (delta.contains("content") && delta["content"].is_string()) {
                            std::string delta_text = delta["content"].get<std::string>();
                            if (!delta_text.empty()) {
                                if (!sent_text_content_start) {
                                    json content_start = {
                                        {"type", "content_block_start"},
                                        {"index", 0},
                                        {"content_block", {{"type", "text"}, {"text", ""}}}
                                    };
                                    if (!write_sse_event(client_sink, "content_block_start", content_start)) {
                                        return false;
                                    }
                                    sent_text_content_start = true;
                                }

                                json content_delta = {
                                    {"type", "content_block_delta"},
                                    {"index", 0},
                                    {"delta", {{"type", "text_delta"}, {"text", delta_text}}}
                                };
                                if (!write_sse_event(client_sink, "content_block_delta", content_delta)) {
                                    return false;
                                }
                            }
                        }

                        if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                            for (const auto& tool_delta : delta["tool_calls"]) {
                                if (!tool_delta.is_object()) {
                                    continue;
                                }

                                int openai_tool_index = tool_delta.value("index", 0);
                                if (openai_tool_index < 0) {
                                    continue;
                                }

                                size_t idx = static_cast<size_t>(openai_tool_index);
                                if (started_tool_blocks.size() <= idx) {
                                    started_tool_blocks.resize(idx + 1, false);
                                    stopped_tool_blocks.resize(idx + 1, false);
                                    tool_ids.resize(idx + 1);
                                    tool_names.resize(idx + 1);
                                }

                                if (tool_delta.contains("id") && tool_delta["id"].is_string()) {
                                    tool_ids[idx] = tool_delta["id"].get<std::string>();
                                }

                                if (tool_delta.contains("function") && tool_delta["function"].is_object()) {
                                    const auto& fn = tool_delta["function"];
                                    if (fn.contains("name") && fn["name"].is_string()) {
                                        tool_names[idx] = fn["name"].get<std::string>();
                                    }
                                }

                                if (!started_tool_blocks[idx]) {
                                    if (tool_ids[idx].empty()) {
                                        tool_ids[idx] = anthropic_message_id();
                                    }
                                    if (tool_names[idx].empty()) {
                                        tool_names[idx] = "unknown_tool";
                                    }

                                    json tool_block_start = {
                                        {"type", "content_block_start"},
                                        {"index", static_cast<int>(idx) + 1},
                                        {"content_block", {
                                            {"type", "tool_use"},
                                            {"id", tool_ids[idx]},
                                            {"name", tool_names[idx]},
                                            {"input", json::object()}
                                        }}
                                    };
                                    if (!write_sse_event(client_sink, "content_block_start", tool_block_start)) {
                                        return false;
                                    }
                                    started_tool_blocks[idx] = true;
                                }

                                if (tool_delta.contains("function") && tool_delta["function"].is_object()) {
                                    const auto& fn = tool_delta["function"];
                                    if (fn.contains("arguments") && fn["arguments"].is_string()) {
                                        std::string args_delta = fn["arguments"].get<std::string>();
                                        if (!args_delta.empty()) {
                                            json tool_input_delta = {
                                                {"type", "content_block_delta"},
                                                {"index", static_cast<int>(idx) + 1},
                                                {"delta", {
                                                    {"type", "input_json_delta"},
                                                    {"partial_json", args_delta}
                                                }}
                                            };
                                            if (!write_sse_event(client_sink, "content_block_delta", tool_input_delta)) {
                                                return false;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                        stop_reason = anthropic_stop_reason(choice);
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "[OllamaApi] Failed to parse Anthropic stream chunk: " << e.what() << std::endl;
            }
        }
        return true;
    };

    adapter_sink.done = [&client_sink,
                         &sent_message_start,
                         &sent_text_content_start,
                         &sent_text_content_stop,
                         &sent_error,
                         &started_tool_blocks,
                         &stopped_tool_blocks,
                         &stop_reason,
                         &input_tokens,
                         &output_tokens,
                         &warnings]() {
        // The error event terminates the stream. Emitting the closing frames
        // here would let a client read the failure as a turn that produced no
        // content, which is the state this path exists to avoid.
        if (sent_error) {
            client_sink.done();
            return;
        }

        if (!sent_message_start) {
            json message_start = {
                {"type", "message_start"},
                {"message", {
                    {"id", anthropic_message_id()},
                    {"type", "message"},
                    {"role", "assistant"},
                    {"content", json::array()},
                    {"stop_reason", nullptr},
                    {"stop_sequence", nullptr},
                    {"usage", {{"input_tokens", 0}, {"output_tokens", 0}}}
                }}
            };
            if (!write_sse_event(client_sink, "message_start", message_start)) {
                client_sink.done();
                return;
            }
            sent_message_start = true;
        }

        if (!sent_text_content_start && started_tool_blocks.empty()) {
            json content_start = {
                {"type", "content_block_start"},
                {"index", 0},
                {"content_block", {{"type", "text"}, {"text", ""}}}
            };
            if (!write_sse_event(client_sink, "content_block_start", content_start)) {
                client_sink.done();
                return;
            }
            sent_text_content_start = true;
        }

        if (sent_text_content_start && !sent_text_content_stop) {
            json content_stop = {
                {"type", "content_block_stop"},
                {"index", 0}
            };
            if (!write_sse_event(client_sink, "content_block_stop", content_stop)) {
                client_sink.done();
                return;
            }
            sent_text_content_stop = true;
        }

        for (size_t idx = 0; idx < started_tool_blocks.size(); ++idx) {
            if (started_tool_blocks[idx] && !stopped_tool_blocks[idx]) {
                json tool_stop = {
                    {"type", "content_block_stop"},
                    {"index", static_cast<int>(idx) + 1}
                };
                if (!write_sse_event(client_sink, "content_block_stop", tool_stop)) {
                    client_sink.done();
                    return;
                }
                stopped_tool_blocks[idx] = true;
            }
        }

        if (stop_reason == "end_turn") {
            for (bool started : started_tool_blocks) {
                if (started) {
                    stop_reason = "tool_use";
                    break;
                }
            }
        }

        json message_delta = {
            {"type", "message_delta"},
            {"delta", {
                {"stop_reason", stop_reason},
                {"stop_sequence", nullptr}
            }},
            {"usage", {
                {"input_tokens", input_tokens},
                {"output_tokens", output_tokens}
            }}
        };
        if (!warnings.empty()) {
            message_delta["warnings"] = warnings;
        }
        if (!write_sse_event(client_sink, "message_delta", message_delta)) {
            client_sink.done();
            return;
        }

        json message_stop = {{"type", "message_stop"}};
        write_sse_event(client_sink, "message_stop", message_stop);
        client_sink.done();
    };

    call_router(openai_body, adapter_sink);
}

} // namespace lemon
