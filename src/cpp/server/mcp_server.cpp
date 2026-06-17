#include "lemon/mcp_server.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <lemon/utils/aixlog.hpp>

#include "lemon/collection_orchestrator.h"
#include "lemon/model_types.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"
#include "lemon/version.h"

namespace lemon {

namespace {

constexpr int kJsonRpcParseError      = -32700;
constexpr int kJsonRpcInvalidRequest  = -32600;
constexpr int kJsonRpcMethodNotFound  = -32601;
constexpr int kJsonRpcInvalidParams   = -32602;
constexpr int kJsonRpcInternalError   = -32603;

constexpr const char* kMcpProtocolVersion = "2025-06-18";
constexpr const char* kServerName = "lemonade-mcp";

// Default model for `lemonade_omni`: the smaller of the two stock Omni
// collections (~5.5B). The 52B "Halo" build is preferable on capable hardware
// but is a multi-GB download; users opt into it by passing `model` explicitly.
constexpr const char* kDefaultOmniModel = "LMX-Omni-5.5B-Lite";

// Per-tool default models, used by resolve_model_for_tool() only as a last
// resort — when no model of the right type is already loaded or downloaded AND
// the caller passed `allow_download: true`. They are deliberately small/fast
// so an unattended download stays cheap; callers wanting something larger pass
// `model` explicitly.
constexpr const char* kDefaultChatModel       = "Qwen3.5-4B-MTP-GGUF";
constexpr const char* kDefaultTranscribeModel = "Whisper-Tiny";
constexpr const char* kDefaultImageModel      = "SD-Turbo";

bool is_notification(const json& message) {
    return !message.contains("id");
}

std::string extract_string_arg(const json& args, const char* key) {
    if (!args.contains(key) || !args[key].is_string()) {
        throw std::runtime_error(std::string("Missing or non-string argument: ") + key);
    }
    return args[key].get<std::string>();
}

// Some MCP clients (notably Claude Desktop) emit chat messages in Anthropic's
// content-block form — `content: [{type:"text", text:"..."}]` — instead of
// OpenAI's plain-string form. llama.cpp silently returns empty content when it
// sees the array form. Flatten any text blocks back into a single string.
json normalize_messages(const json& messages) {
    if (!messages.is_array()) return messages;
    json out = json::array();
    for (const auto& msg : messages) {
        if (!msg.is_object() || !msg.contains("content") || !msg["content"].is_array()) {
            out.push_back(msg);
            continue;
        }
        json normalized = msg;
        std::string text;
        for (const auto& block : msg["content"]) {
            if (block.is_string()) {
                text += block.get<std::string>();
            } else if (block.is_object() && block.value("type", std::string()) == "text" &&
                       block.contains("text") && block["text"].is_string()) {
                text += block["text"].get<std::string>();
            }
        }
        normalized["content"] = text;
        out.push_back(std::move(normalized));
    }
    return out;
}

// If `model` exists in the raw registry but was filtered out on this system
// (e.g. a `*-Hybrid` (ryzenai-llm) model on Linux/Docker, or a `*-FLM` model
// on a machine without an AMD Ryzen AI NPU), return a tool-call error JSON
// naming the reason and suggesting a portable alternative. Returns nullopt
// when the model is supported here, or when it's not in the registry at all
// — downstream handlers produce their normal errors in those cases.
//
// `tool_label` is the human-facing tool noun used in the suggestion sentence
// (e.g. "chat"). `recipe_substrings` lists recipe-name substrings that mark
// a candidate suitable for this tool kind; the first supported, suggested
// model whose recipe matches any substring wins. Pass an empty list to skip
// the suggestion (the filter reason alone is still returned).
std::optional<json> unsupported_model_error(
        ModelManager* model_manager,
        const std::string& model,
        const char* tool_label,
        std::initializer_list<const char*> recipe_substrings) {
    if (model_manager->model_exists(model)) return std::nullopt;
    if (!model_manager->model_exists_unfiltered(model)) return std::nullopt;

    std::string reason = model_manager->get_model_filter_reason(model);
    if (reason.empty()) {
        reason = "This model is not available on the running lemonade server.";
    }

    std::string msg = "Model '" + model + "' is not available on this lemonade server. " +
                      reason;

    // Suggest the first supported, `suggested`-flagged model whose recipe
    // matches one of the requested families.
    if (recipe_substrings.size() > 0) {
        std::string suggestion;
        for (const auto& [name, info] : model_manager->get_supported_models()) {
            if (!info.suggested) continue;
            bool matches = false;
            for (const char* needle : recipe_substrings) {
                if (info.recipe.find(needle) != std::string::npos) {
                    matches = true;
                    break;
                }
            }
            if (!matches) continue;
            suggestion = name;
            break;
        }
        if (!suggestion.empty()) {
            msg += " Try '" + suggestion + "' for portable " + tool_label +
                   ", or call `lemonade_list_models` to see what's available.";
        } else {
            msg += " Call `lemonade_list_models` to discover what's available.";
        }
    }

    return json{
        {"content", json::array({json{{"type", "text"}, {"text", msg}}})},
        {"isError", true},
    };
}

// Root directory that MCP image writes are confined to. Defaults to
// <cache_dir>/mcp-images; override with the LEMONADE_MCP_IMAGE_DIR environment
// variable (must be an absolute path). Created on demand. weakly_canonical
// resolves symlinks in the existing prefix so containment checks below cannot
// be fooled by a symlinked sandbox root.
std::filesystem::path mcp_image_sandbox_root() {
    const std::string override_dir =
        utils::get_environment_variable_utf8("LEMONADE_MCP_IMAGE_DIR");
    std::filesystem::path root =
        override_dir.empty()
            ? utils::path_from_utf8(utils::get_cache_dir()) / "mcp-images"
            : utils::path_from_utf8(override_dir);
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    std::filesystem::path canonical = std::filesystem::weakly_canonical(root, ec);
    return ec ? root.lexically_normal() : canonical;
}

// Resolve a caller-supplied path and confine it to `root`. Relative paths are
// taken relative to `root`; absolute paths must already live inside it. Rejects
// `..` traversal and symlink escapes by resolving with weakly_canonical and
// verifying the result is still a descendant of `root`. `label` names the
// argument for error messages.
std::filesystem::path sanitize_sandboxed_path(const std::string& raw,
                                              const std::filesystem::path& root,
                                              const char* label) {
    if (raw.empty()) {
        throw std::runtime_error(std::string("`") + label + "` must not be empty.");
    }
    const std::filesystem::path requested = utils::path_from_utf8(raw);
    const std::filesystem::path combined =
        requested.is_absolute() ? requested : (root / requested);
    std::error_code ec;
    std::filesystem::path resolved = std::filesystem::weakly_canonical(combined, ec);
    if (ec) resolved = combined.lexically_normal();

    const std::filesystem::path rel = resolved.lexically_relative(root);
    const bool inside = !rel.empty() && rel.begin()->string() != "..";
    if (!inside) {
        throw std::runtime_error(
            std::string("`") + label +
            "` must resolve to a location inside the MCP image sandbox (" +
            utils::path_to_utf8(root) +
            "). Set LEMONADE_MCP_IMAGE_DIR to change the sandbox root. Rejected: " +
            raw);
    }
    return resolved;
}

// Collision-resistant token for auto-generated artifact filenames. A single
// lemond serves multiple concurrent clients (AGENTS.md invariant 11), so the
// old deterministic names (image_0.png, omni_0.png) silently clobbered each
// other across calls. A millisecond timestamp plus a process-wide atomic
// counter makes every generated file land on a fresh name — no overwrite, and
// no need to error/regenerate on a clash. Callers that want an exact name use
// `output_path` instead.
std::string unique_token() {
    static std::atomic<std::uint64_t> counter{0};
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream oss;
    oss << std::hex << ms << "-" << std::setw(4) << std::setfill('0')
        << (counter.fetch_add(1, std::memory_order_relaxed) & 0xffff);
    return oss.str();
}

}  // namespace

McpServer::McpServer(Router* router, ModelManager* model_manager, EnsureLoadedFn ensure_loaded)
    : router_(router),
      model_manager_(model_manager),
      ensure_loaded_(std::move(ensure_loaded)) {}

McpServer::~McpServer() = default;

void McpServer::register_routes(httplib::Server& server) {
    auto self = shared_from_this();

    server.Post("/mcp", [self](const httplib::Request& req, httplib::Response& res) {
        std::string response_body;
        try {
            response_body = self->handle_request_body(req.body);
        } catch (const std::exception& e) {
            LOG(ERROR, "McpServer") << "Unhandled exception in POST /mcp: " << e.what() << std::endl;
            res.status = 500;
            json err = make_error_response(nullptr, kJsonRpcInternalError, e.what());
            res.set_content(err.dump(), "application/json");
            return;
        }

        if (response_body.empty()) {
            // All messages were notifications — Streamable HTTP expects 202.
            res.status = 202;
            return;
        }

        res.status = 200;
        res.set_content(response_body, "application/json");
    });

    server.Get("/mcp", [](const httplib::Request&, httplib::Response& res) {
        // No SSE channel in this MVP; refuse GET explicitly.
        res.status = 405;
        res.set_header("Allow", "POST");
        res.set_content(
            "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,"
            "\"message\":\"GET /mcp not supported; use POST with a JSON-RPC body\"},"
            "\"id\":null}",
            "application/json");
    });
}

std::string McpServer::handle_request_body(const std::string& body) {
    json parsed;
    try {
        parsed = json::parse(body);
    } catch (const json::parse_error& e) {
        json err = make_error_response(nullptr, kJsonRpcParseError,
                                       std::string("Invalid JSON: ") + e.what());
        return err.dump();
    }

    if (parsed.is_array()) {
        if (parsed.empty()) {
            json err = make_error_response(nullptr, kJsonRpcInvalidRequest,
                                           "Batch request must not be empty");
            return err.dump();
        }
        json responses = json::array();
        for (const auto& message : parsed) {
            json reply = dispatch_message(message);
            if (!reply.is_null()) {
                responses.push_back(std::move(reply));
            }
        }
        if (responses.empty()) return "";
        return responses.dump();
    }

    if (!parsed.is_object()) {
        json err = make_error_response(nullptr, kJsonRpcInvalidRequest,
                                       "Request must be a JSON object or array");
        return err.dump();
    }
    json reply = dispatch_message(parsed);
    if (reply.is_null()) return "";
    return reply.dump();
}

json McpServer::dispatch_message(const json& message) {
    if (!message.is_object()) {
        return make_error_response(nullptr, kJsonRpcInvalidRequest,
                                   "Request must be a JSON object");
    }

    const bool notification = is_notification(message);
    json id = message.value("id", json(nullptr));

    if (!message.contains("method") || !message["method"].is_string()) {
        if (notification) return json(nullptr);
        return make_error_response(id, kJsonRpcInvalidRequest, "Missing `method` field");
    }

    const std::string method = message["method"].get<std::string>();
    json params = message.value("params", json::object());

    try {
        if (method == "initialize") {
            if (notification) return json(nullptr);
            return handle_initialize(params, id);
        }
        if (method == "tools/list") {
            if (notification) return json(nullptr);
            return handle_tools_list(id);
        }
        if (method == "tools/call") {
            if (notification) return json(nullptr);
            return handle_tools_call(params, id);
        }
        if (method == "ping") {
            if (notification) return json(nullptr);
            return handle_ping(id);
        }
        if (method == "notifications/initialized" ||
            method == "notifications/cancelled") {
            return json(nullptr);
        }

        if (notification) return json(nullptr);
        return make_error_response(id, kJsonRpcMethodNotFound,
                                   "Unknown method: " + method);
    } catch (const std::exception& e) {
        LOG(ERROR, "McpServer") << "Method '" << method << "' threw: " << e.what() << std::endl;
        if (notification) return json(nullptr);
        return make_error_response(id, kJsonRpcInternalError, e.what());
    }
}

json McpServer::handle_initialize(const json& /*params*/, const json& id) {
    json result = {
        {"protocolVersion", kMcpProtocolVersion},
        {"capabilities", {
            {"tools", json::object()},
        }},
        {"serverInfo", {
            {"name", kServerName},
            {"version", LEMON_VERSION_STRING},
        }},
    };
    return make_success_response(id, std::move(result));
}

json McpServer::handle_tools_list(const json& id) {
    json result = {{"tools", tools_descriptor()}};
    return make_success_response(id, std::move(result));
}

json McpServer::handle_tools_call(const json& params, const json& id) {
    if (!params.is_object() || !params.contains("name") || !params["name"].is_string()) {
        return make_error_response(id, kJsonRpcInvalidParams,
                                   "tools/call requires a string `name` parameter");
    }
    const std::string tool_name = params["name"].get<std::string>();
    json arguments = params.value("arguments", json::object());
    if (!arguments.is_object()) {
        return make_error_response(id, kJsonRpcInvalidParams,
                                   "tools/call `arguments` must be an object");
    }

    LOG(INFO, "McpServer") << "tools/call: " << tool_name << std::endl;

    json result;
    try {
        if (tool_name == "lemonade_chat") {
            result = tool_chat(arguments);
        } else if (tool_name == "lemonade_transcribe_audio") {
            result = tool_transcribe_audio(arguments);
        } else if (tool_name == "lemonade_generate_image") {
            result = tool_generate_image(arguments);
        } else if (tool_name == "lemonade_omni") {
            result = tool_omni(arguments);
        } else if (tool_name == "lemonade_list_models") {
            result = tool_list_models(arguments);
        } else {
            // Per MCP spec, unknown-tool errors are isError=true results, not JSON-RPC errors.
            result = {
                {"content", json::array({text_content_block("Unknown tool: " + tool_name)})},
                {"isError", true},
            };
        }
    } catch (const std::exception& e) {
        LOG(ERROR, "McpServer") << "Tool '" << tool_name << "' failed: " << e.what() << std::endl;
        result = {
            {"content", json::array({text_content_block(std::string("Error: ") + e.what())})},
            {"isError", true},
        };
    }

    return make_success_response(id, std::move(result));
}

json McpServer::handle_ping(const json& id) {
    return make_success_response(id, json::object());
}

json McpServer::make_needs_model_result(const char* type_str,
                                        const char* default_model,
                                        const std::string& name_hint) {
    json status = {
        {"status", "needs_model"},
        {"tool_type", type_str},
        {"default_model", default_model},
        {"retry_with", json{
            {"model", name_hint},
            {"allow_download", true},
        }},
    };
    std::string msg =
        std::string("No ") + type_str + " model is currently loaded or "
        "downloaded on this lemonade server. Re-call this tool with an "
        "explicit `model` (" + name_hint + "; call `lemonade_list_models` to "
        "see what's available), or pass `allow_download: true` to download and "
        "load the default (" + default_model + "), which may be a multi-GB "
        "download.";
    return json{
        {"content", json::array({
            text_content_block(msg),
            text_content_block(status.dump()),
        })},
        {"isError", true},
    };
}

std::variant<std::string, json> McpServer::resolve_model_for_tool(
        const json& arguments,
        ModelType want_type,
        const char* type_str,
        const char* default_model,
        bool allow_download) {
    // 1. Explicit model argument always wins.
    if (arguments.contains("model") && arguments["model"].is_string() &&
        !arguments["model"].get<std::string>().empty()) {
        return arguments["model"].get<std::string>();
    }

    // 2. Reuse a model of the right type that's already loaded \u2014 zero cost.
    for (const auto& m : router_->get_all_loaded_models()) {
        if (m.value("type", std::string()) == type_str) {
            std::string name = m.value("model_name", std::string());
            if (!name.empty()) return name;
        }
    }

    // 3. Load a model of the right type that's already downloaded \u2014 no network.
    for (const auto& [name, info] : model_manager_->get_downloaded_models()) {
        if (info.type == want_type) return name;
    }

    // 4. Nothing local. Only fetch the default if the caller opted in;
    //    otherwise ask the agent to confirm a download or name a model.
    if (allow_download) return std::string(default_model);
    return make_needs_model_result(
        type_str, default_model, "an explicit model name");
}

json McpServer::tool_chat(const json& arguments) {
    if (!arguments.contains("messages") || !arguments["messages"].is_array()) {
        throw std::runtime_error("Missing or non-array argument: messages");
    }

    const bool allow_download = arguments.value("allow_download", false);
    auto resolved = resolve_model_for_tool(arguments, ModelType::LLM, "llm",
                                           kDefaultChatModel, allow_download);
    if (std::holds_alternative<json>(resolved)) return std::get<json>(resolved);
    const std::string model = std::get<std::string>(resolved);

    if (auto err = unsupported_model_error(model_manager_, model, "chat", {"llamacpp"})) {
        return *err;
    }

    ensure_loaded_(model);

    json openai_request = {
        {"model", model},
        {"messages", normalize_messages(arguments["messages"])},
        {"stream", false},
    };

    for (const char* key : {"temperature", "top_p", "max_tokens", "stop",
                            "seed", "presence_penalty", "frequency_penalty",
                            "tools", "tool_choice", "response_format",
                            "chat_template_kwargs"}) {
        if (arguments.contains(key)) {
            openai_request[key] = arguments[key];
        }
    }

    // Reasoning models (Qwen3, DeepSeek-R1) burn small max_tokens budgets on
    // <think> blocks, leaving content empty. Disable thinking by default;
    // callers can opt back in via chat_template_kwargs.enable_thinking=true.
    if (!openai_request.contains("chat_template_kwargs")) {
        openai_request["chat_template_kwargs"] = {{"enable_thinking", false}};
    }

    json response = router_->chat_completion(openai_request);
    if (response.contains("error")) {
        throw std::runtime_error(response["error"].value("message", "chat completion failed"));
    }

    // Reasoning-model fallback: if `content` is empty but `reasoning_content`
    // is not, surface the reasoning rather than returning an empty string.
    std::string assistant_text;
    std::string reasoning_text;
    std::string finish_reason;
    json tool_calls;
    if (response.contains("choices") && response["choices"].is_array() &&
        !response["choices"].empty()) {
        const auto& choice = response["choices"][0];
        finish_reason = choice.value("finish_reason", std::string());
        const auto& message = choice.value("message", json::object());
        if (message.contains("content") && message["content"].is_string()) {
            assistant_text = message["content"].get<std::string>();
        }
        if (message.contains("reasoning_content") &&
            message["reasoning_content"].is_string()) {
            reasoning_text = message["reasoning_content"].get<std::string>();
        }
        if (message.contains("tool_calls")) {
            tool_calls = message["tool_calls"];
        }
    }

    json content = json::array();
    if (assistant_text.empty() && !reasoning_text.empty()) {
        std::string note = "[reasoning only";
        if (finish_reason == "length") note += "; finish_reason=length";
        note += "]\n";
        content.push_back(text_content_block(note + reasoning_text));
    } else {
        content.push_back(text_content_block(assistant_text));
    }
    if (!tool_calls.is_null() && !tool_calls.empty()) {
        content.push_back(text_content_block(
            std::string("tool_calls: ") + tool_calls.dump()));
    }

    return json{
        {"content", std::move(content)},
        {"isError", false},
    };
}

json McpServer::tool_transcribe_audio(const json& arguments) {
    const bool allow_download = arguments.value("allow_download", false);
    auto resolved = resolve_model_for_tool(arguments, ModelType::TRANSCRIPTION,
                                           "transcription", kDefaultTranscribeModel,
                                           allow_download);
    if (std::holds_alternative<json>(resolved)) return std::get<json>(resolved);
    const std::string model = std::get<std::string>(resolved);

    // Same-machine deployment: prefer `audio_path` over base64 to avoid
    // multi-MB blobs through JSON-RPC. Agents reach for `audio_path` first;
    // accepting only base64 led to path-in-base64 errors.
    std::string audio_data;
    std::string filename = arguments.value("filename", std::string());
    const bool has_path = arguments.contains("audio_path") && arguments["audio_path"].is_string();
    const bool has_b64  = arguments.contains("audio_base64") && arguments["audio_base64"].is_string();
    if (!has_path && !has_b64) {
        throw std::runtime_error(
            "Provide either `audio_path` (path to a local audio file) or "
            "`audio_base64` (base64-encoded audio bytes).");
    }
    if (has_path) {
        const std::string path = arguments["audio_path"].get<std::string>();
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec) {
            throw std::runtime_error("audio_path not found: " + path);
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("Failed to open audio_path: " + path);
        }
        std::ostringstream buf;
        buf << in.rdbuf();
        audio_data = buf.str();
        if (audio_data.empty()) {
            throw std::runtime_error("audio_path is empty: " + path);
        }
        if (filename.empty()) {
            filename = std::filesystem::path(path).filename().string();
        }
    } else {
        audio_data = utils::JsonUtils::base64_decode(arguments["audio_base64"].get<std::string>());
        if (audio_data.empty()) {
            throw std::runtime_error(
                "audio_base64 decoded to zero bytes. If you meant to pass a "
                "file path, use the `audio_path` argument instead.");
        }
        if (filename.empty()) {
            filename = "audio.wav";
        }
    }

    if (auto err = unsupported_model_error(model_manager_, model, "transcription", {"whispercpp"})) {
        return *err;
    }

    ensure_loaded_(model);

    json router_request = {
        {"model", model},
        {"file_data", std::move(audio_data)},
        {"filename", filename},
    };
    for (const char* key : {"language", "prompt", "response_format", "temperature"}) {
        if (arguments.contains(key)) {
            router_request[key] = arguments[key];
        }
    }

    json response = router_->audio_transcriptions(router_request);
    if (response.contains("error")) {
        throw std::runtime_error(response["error"].value("message", "transcription failed"));
    }

    std::string transcript = response.value("text", std::string());
    json content = json::array();
    content.push_back(text_content_block(transcript));
    content.push_back(text_content_block(response.dump()));

    return json{
        {"content", std::move(content)},
        {"isError", false},
    };
}

json McpServer::tool_generate_image(const json& arguments) {
    const std::string prompt = extract_string_arg(arguments, "prompt");

    const bool allow_download = arguments.value("allow_download", false);
    auto resolved = resolve_model_for_tool(arguments, ModelType::IMAGE, "image",
                                           kDefaultImageModel, allow_download);
    if (std::holds_alternative<json>(resolved)) return std::get<json>(resolved);
    const std::string model = std::get<std::string>(resolved);

    // Disk-output mode: MCP image content blocks cost tens of thousands of
    // tokens per image and some clients surface them as opaque resource URIs
    // the agent has to round-trip back to disk. Letting the caller name the
    // destination avoids both. Symmetric with `audio_path`.
    //
    // SECURITY: writes are confined to a sandbox directory (see
    // mcp_image_sandbox_root) so a cross-origin/unauthenticated caller cannot
    // overwrite arbitrary files (e.g. ~/.bashrc) via output_path/output_dir.
    const bool has_output_path = arguments.contains("output_path") &&
                                 arguments["output_path"].is_string();
    const bool has_output_dir  = arguments.contains("output_dir") &&
                                 arguments["output_dir"].is_string();
    if (has_output_path && has_output_dir) {
        throw std::runtime_error(
            "Provide at most one of `output_path` or `output_dir`, not both.");
    }
    const std::filesystem::path sandbox_root = mcp_image_sandbox_root();
    std::filesystem::path output_path;
    std::filesystem::path output_dir;
    if (has_output_path) {
        output_path = sanitize_sandboxed_path(
            arguments["output_path"].get<std::string>(), sandbox_root, "output_path");
    }
    if (has_output_dir) {
        output_dir = sanitize_sandboxed_path(
            arguments["output_dir"].get<std::string>(), sandbox_root, "output_dir");
    }

    if (auto err = unsupported_model_error(model_manager_, model, "image generation", {"sd-cpp"})) {
        return *err;
    }

    ensure_loaded_(model);

    json router_request = {
        {"model", model},
        {"prompt", prompt},
    };
    for (const char* key : {"size", "n", "quality", "style", "response_format",
                            "negative_prompt", "seed", "steps", "cfg_scale"}) {
        if (arguments.contains(key)) {
            router_request[key] = arguments[key];
        }
    }
    router_request["response_format"] = "b64_json";

    json response = router_->image_generations(router_request);
    if (response.contains("error")) {
        throw std::runtime_error(response["error"].value("message", "image generation failed"));
    }

    std::vector<std::string> b64_images;
    if (response.contains("data") && response["data"].is_array()) {
        for (const auto& entry : response["data"]) {
            if (!entry.contains("b64_json") || !entry["b64_json"].is_string()) continue;
            b64_images.push_back(entry["b64_json"].get<std::string>());
        }
    }
    if (b64_images.empty()) {
        throw std::runtime_error("Image generation returned no images");
    }

    if (!has_output_path && !has_output_dir) {
        json content = json::array();
        for (const auto& b64 : b64_images) {
            content.push_back({
                {"type", "image"},
                {"data", b64},
                {"mimeType", "image/png"},
            });
        }
        return json{
            {"content", std::move(content)},
            {"isError", false},
        };
    }

    if (has_output_path && b64_images.size() > 1) {
        throw std::runtime_error(
            "`output_path` only supports a single image; use `output_dir` "
            "when n > 1.");
    }

    std::vector<std::filesystem::path> written;
    written.reserve(b64_images.size());
    std::error_code ec;
    if (has_output_dir) {
        std::filesystem::create_directories(output_dir, ec);
        if (ec) {
            throw std::runtime_error(
                "Failed to create output_dir " + output_dir.string() + ": " + ec.message());
        }
    } else {
        auto parent = output_path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                throw std::runtime_error(
                    "Failed to create parent of output_path " + parent.string() + ": " + ec.message());
            }
        }
    }

    for (size_t i = 0; i < b64_images.size(); ++i) {
        // output_path: caller named the file explicitly, write exactly there.
        // output_dir: auto-generate a unique name so concurrent clients never
        // clobber one another (and we never have to error/regenerate).
        std::filesystem::path dest = has_output_path
            ? output_path
            : output_dir / ("image_" + unique_token() + "_" + std::to_string(i) + ".png");
        std::string bytes = utils::JsonUtils::base64_decode(b64_images[i]);
        if (bytes.empty()) {
            throw std::runtime_error("Decoded image is empty (index " + std::to_string(i) + ")");
        }
        std::ofstream out(dest, std::ios::binary);
        if (!out) {
            throw std::runtime_error("Failed to open for writing: " + dest.string());
        }
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            throw std::runtime_error("Failed to write: " + dest.string());
        }
        written.push_back(std::filesystem::absolute(dest, ec));
        if (ec) written.back() = dest;
    }

    std::string summary = (written.size() == 1)
        ? ("Wrote image to: " + written[0].string())
        : ("Wrote " + std::to_string(written.size()) + " images:");
    json content = json::array();
    content.push_back(text_content_block(summary));
    if (written.size() > 1) {
        for (const auto& p : written) {
            content.push_back(text_content_block(p.string()));
        }
    }
    json paths = json::array();
    for (const auto& p : written) paths.push_back(p.string());
    content.push_back(text_content_block(json{{"paths", std::move(paths)}}.dump()));

    return json{
        {"content", std::move(content)},
        {"isError", false},
    };
}

json McpServer::tool_omni(const json& arguments) {
    if (!arguments.contains("messages") || !arguments["messages"].is_array()) {
        throw std::runtime_error("Missing or non-array argument: messages");
    }

    // Resolve which Omni collection to run. Collections are matched by recipe
    // (`collection.omni`) rather than ModelType, so this mirrors
    // resolve_model_for_tool's precedence inline:
    //   1. explicit `model` wins;
    //   2. an already-downloaded collection (no network);
    //   3. the default collection, but only when `allow_download` is set.
    // There is no "already loaded" step: a collection's components register as
    // their individual ModelTypes, so there is no single loaded "omni" entry to
    // detect; step 2 already avoids a download when one is on disk.
    std::string model;
    if (arguments.contains("model") && arguments["model"].is_string() &&
        !arguments["model"].get<std::string>().empty()) {
        model = arguments["model"].get<std::string>();
    } else {
        for (const auto& [name, info] : model_manager_->get_downloaded_models()) {
            if (is_collection_recipe(info.recipe)) {
                model = name;
                break;
            }
        }
        if (model.empty()) {
            const bool allow_download = arguments.value("allow_download", false);
            if (allow_download) {
                model = kDefaultOmniModel;
            } else {
                return make_needs_model_result(
                    "omni", kDefaultOmniModel,
                    "an Omni collection name, recipe=collection.omni");
            }
        }
    }

    if (auto err = unsupported_model_error(model_manager_, model, "Omni", {"collection.omni"})) {
        return *err;
    }

    if (!model_manager_->model_exists(model)) {
        return json{
            {"content", json::array({text_content_block(
                "Unknown model '" + model +
                "'. Call `lemonade_list_models` to discover available Omni "
                "collections (recipe = collection.omni), or omit `model` to "
                "use the default (" + std::string(kDefaultOmniModel) + ").")})},
            {"isError", true},
        };
    }

    ModelInfo info = model_manager_->get_model_info(model);
    if (!is_collection_recipe(info.recipe)) {
        return json{
            {"content", json::array({text_content_block(
                "Model '" + model + "' is not an Omni collection (recipe='" +
                info.recipe + "'). Use `lemonade_chat` for plain LLMs, or "
                "pass an Omni collection name (e.g. " +
                std::string(kDefaultOmniModel) + ").")})},
            {"isError", true},
        };
    }

    // Optional disk-output mode. MCP image/audio content blocks cost tens of
    // thousands of tokens per artifact; the same disk-mode reasoning as
    // `lemonade_generate_image` applies, plus a single Omni turn can emit
    // mixed media (one image + one TTS clip), so we only accept `output_dir`.
    //
    // SECURITY: like lemonade_generate_image, writes are confined to the MCP
    // image sandbox so an unauthenticated cross-origin caller cannot overwrite
    // arbitrary files via output_dir.
    const bool has_output_dir = arguments.contains("output_dir") &&
                                arguments["output_dir"].is_string();
    std::filesystem::path output_dir;
    if (has_output_dir) {
        output_dir = sanitize_sandboxed_path(
            arguments["output_dir"].get<std::string>(), mcp_image_sandbox_root(),
            "output_dir");
    }

    // Serialize concurrent lemonade_omni invocations process-wide. The
    // planner emits jinja-templated chat completions with a long system
    // prompt + multiple tool definitions; llama-server (Vulkan + MTP draft
    // + mmproj) can crash when several such requests land on it in
    // parallel. The crash is silent and lemond doesn't notice the dead
    // child, so follow-up calls surface as `CURL error: Couldn't connect
    // to server`. A function-local mutex keeps a single planner in flight
    // and avoids the trigger entirely; other MCP tools fan out to separate
    // backends and are unaffected.
    static std::mutex omni_serial_mutex;
    std::lock_guard<std::mutex> omni_lock(omni_serial_mutex);

    // ensure_loaded_ dispatches to ensure_collection_loaded() under the hood
    // when the recipe is `collection.omni`, so this pulls down + loads every
    // component (planner LLM, image model, TTS voice, ...).
    ensure_loaded_(model);

    json openai_request = {
        {"model", model},
        {"messages", normalize_messages(arguments["messages"])},
        {"stream", false},
    };
    for (const char* key : {"temperature", "top_p", "max_tokens", "stop",
                            "seed", "presence_penalty", "frequency_penalty",
                            "tools", "tool_choice", "response_format",
                            "chat_template_kwargs"}) {
        if (arguments.contains(key)) {
            openai_request[key] = arguments[key];
        }
    }

    CollectionOrchestrator orchestrator(*router_, *model_manager_, ensure_loaded_);
    CollectionOrchestrator::ChatParts parts =
        orchestrator.chat_completion_parts(openai_request, info);

    // Recover from a backend that died between requests. The router's
    // liveness check is handle-presence only, so a crashed subprocess stays
    // registered as loaded and `forward_request` hits a closed port. If we
    // see a transport-level failure, evict the collection's components and
    // retry the orchestrator once.
    auto looks_like_backend_died = [](const std::string& msg) {
        static constexpr const char* kMarkers[] = {
            "CURL error",
            "Network error",
            "Couldn't connect",
            "Failure when receiving",
        };
        for (const char* marker : kMarkers) {
            if (msg.find(marker) != std::string::npos) return true;
        }
        return false;
    };
    if (!parts.ok && looks_like_backend_died(parts.error_message)) {
        LOG(WARNING, "McpServer")
            << "Omni: backend looks dead (" << parts.error_message
            << "); evicting components and retrying once" << std::endl;
        for (const auto& component : info.components) {
            try {
                router_->unload_model(component);
            } catch (const std::exception&) {
                // Not loaded -> nothing to evict, fine.
            }
        }
        ensure_loaded_(model);
        parts = orchestrator.chat_completion_parts(openai_request, info);
    }

    if (!parts.ok) {
        return json{
            {"content", json::array({text_content_block(
                std::string("Omni run failed: ") + parts.error_message)})},
            {"isError", true},
        };
    }

    // Disk mode: create the directory once, up front, so artifact writes
    // below get a clean error if it's invalid.
    if (has_output_dir && !parts.artifacts.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(output_dir, ec);
        if (ec) {
            throw std::runtime_error(
                "Failed to create output_dir " + output_dir.string() + ": " + ec.message());
        }
    }

    json content = json::array();

    // Text first — always emit a block (possibly empty) so clients that index
    // by position see a consistent shape.
    content.push_back(text_content_block(parts.final_text));

    auto extension_for = [](const std::string& mime) -> std::string {
        if (mime == "image/png")  return ".png";
        if (mime == "image/jpeg") return ".jpg";
        if (mime == "audio/mpeg") return ".mp3";
        if (mime == "audio/wav")  return ".wav";
        return ".bin";
    };

    json written_paths = json::array();
    for (size_t i = 0; i < parts.artifacts.size(); ++i) {
        const auto& artifact = parts.artifacts[i];

        if (has_output_dir) {
            // Unique name so concurrent clients never clobber each other's
            // artifacts (and we never have to error/regenerate on a clash).
            std::filesystem::path dest =
                output_dir / ("omni_" + unique_token() + "_" + std::to_string(i) +
                              extension_for(artifact.mime));
            std::string bytes = utils::JsonUtils::base64_decode(artifact.data);
            if (bytes.empty()) {
                throw std::runtime_error(
                    "Decoded artifact is empty (index " + std::to_string(i) + ")");
            }
            std::ofstream out(dest, std::ios::binary);
            if (!out) {
                throw std::runtime_error("Failed to open for writing: " + dest.string());
            }
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!out) {
                throw std::runtime_error("Failed to write: " + dest.string());
            }
            std::error_code ec;
            auto abs = std::filesystem::absolute(dest, ec);
            if (ec) abs = dest;
            content.push_back(text_content_block(
                artifact.type + " -> " + abs.string()));
            written_paths.push_back(abs.string());
            continue;
        }

        // Inline mode: native MCP image/audio content blocks. Non-Open-WebUI
        // clients (Claude Desktop, Inspector, ...) cannot render markdown
        // data URIs, so these blocks — not the orchestrator's rendered
        // chat.completion content — are the right transport.
        if (artifact.type == "image") {
            content.push_back({
                {"type", "image"},
                {"data", artifact.data},
                {"mimeType", artifact.mime},
            });
        } else if (artifact.type == "audio") {
            content.push_back({
                {"type", "audio"},
                {"data", artifact.data},
                {"mimeType", artifact.mime},
            });
        } else {
            // Unknown artifact kind — fall back to text rather than dropping it.
            content.push_back(text_content_block(
                "[unrenderable artifact type=" + artifact.type +
                " mime=" + artifact.mime + "]"));
        }
    }

    if (has_output_dir) {
        content.push_back(text_content_block(
            json{{"paths", std::move(written_paths)}}.dump()));
    }

    if (parts.app_tool_calls.is_array() && !parts.app_tool_calls.empty()) {
        content.push_back(text_content_block(
            std::string("tool_calls: ") + parts.app_tool_calls.dump()));
    }

    return json{
        {"content", std::move(content)},
        {"isError", false},
    };
}

json McpServer::tool_list_models(const json& arguments) {
    const bool include_available = arguments.value("include_available", true);
    const bool include_suggested = arguments.value("include_suggested", true);

    json loaded = router_->get_all_loaded_models();
    auto downloaded = model_manager_->get_downloaded_models();

    json available = json::array();
    if (include_available) {
        // Only downloaded models — the full registry would blow up client context.
        for (const auto& [model_id, info] : downloaded) {
            json entry = {
                {"model_name", model_id},
                {"recipe", info.recipe},
                {"downloaded", info.downloaded},
                {"suggested", info.suggested},
                {"labels", info.labels},
            };
            if (info.size > 0.0) entry["size_gb"] = info.size;
            available.push_back(std::move(entry));
        }
    }

    json suggested_to_pull = json::array();
    if (include_suggested) {
        for (const auto& [model_id, info] : model_manager_->get_supported_models()) {
            if (!info.suggested) continue;
            if (downloaded.count(model_id)) continue;
            json entry = {
                {"model_name", model_id},
                {"recipe", info.recipe},
                {"labels", info.labels},
            };
            if (info.size > 0.0) entry["size_gb"] = info.size;
            suggested_to_pull.push_back(std::move(entry));
        }
    }

    // Prefer a loaded LLM; fall back to the most recently loaded model of any type.
    std::string recommended;
    for (const auto& m : loaded) {
        if (m.value("type", std::string()) == "llm") {
            recommended = m.value("model_name", std::string());
            break;
        }
    }
    if (recommended.empty() && !loaded.empty()) {
        recommended = loaded.back().value("model_name", std::string());
    }

    json payload = {
        {"loaded", std::move(loaded)},
        {"available", std::move(available)},
        {"suggested_to_pull", std::move(suggested_to_pull)},
        {"recommended_chat_model", recommended},
    };

    std::string summary;
    if (payload["loaded"].empty()) {
        summary = "No models are currently loaded. ";
    } else {
        summary = "Loaded models: ";
        bool first = true;
        for (const auto& m : payload["loaded"]) {
            if (!first) summary += ", ";
            summary += m.value("model_name", std::string());
            summary += " (" + m.value("type", std::string()) + ")";
            first = false;
        }
        summary += ". ";
    }
    if (!recommended.empty()) {
        summary += "Use `" + recommended + "` for chat unless the user asks otherwise.";
    } else if (!payload["suggested_to_pull"].empty()) {
        summary += "No loaded LLM. Suggested models to pull: ";
        bool first = true;
        for (const auto& m : payload["suggested_to_pull"]) {
            if (!first) summary += ", ";
            summary += m.value("model_name", std::string());
            first = false;
        }
        summary += ".";
    }

    return json{
        {"content", json::array({
            text_content_block(summary),
            text_content_block(payload.dump()),
        })},
        {"isError", false},
    };
}

json McpServer::tools_descriptor() {
    return json::array({
        {
            {"name", "lemonade_list_models"},
            {"description",
             "List models known to the Lemonade server. ALWAYS call this "
             "first if you don't already know the exact model name to use "
             "for chat/transcribe/image — passing a wrong name may trigger a "
             "multi-GB download. Returns a summary text block plus a JSON "
             "block with `{loaded, available, suggested_to_pull, "
             "recommended_chat_model}`."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"include_available", {{"type", "boolean"}}},
                    {"include_suggested", {{"type", "boolean"}}},
                }},
            }},
        },
        {
            {"name", "lemonade_chat"},
            {"description",
             "Chat completion against a locally hosted LLM. Pass a `messages` "
             "array (OpenAI chat format). `model` is OPTIONAL: when omitted, "
             "the server reuses an already-loaded LLM, else an already-"
             "downloaded one; if neither exists it asks you to either pass a "
             "`model` or `allow_download: true` (which downloads the default, "
             "Qwen3.5-4B-MTP-GGUF). Call `lemonade_list_models` first if you "
             "want to choose explicitly — a wrong name may trigger a multi-GB "
             "download."},
            {"inputSchema", {
                {"type", "object"},
                {"required", json::array({"messages"})},
                {"properties", {
                    {"model",       {{"type", "string"},
                                     {"description", "Optional. Omit to auto-select a loaded/downloaded LLM; defaults to Qwen3.5-4B-MTP-GGUF only with allow_download=true."}}},
                    {"allow_download", {{"type", "boolean"},
                                        {"description", "Permit downloading the default model when none is loaded or downloaded. Defaults to false."}}},
                    {"messages",    {{"type", "array"}, {"items", {{"type", "object"}}}}},
                    {"temperature", {{"type", "number"}}},
                    {"top_p",       {{"type", "number"}}},
                    {"max_tokens",  {{"type", "integer"}}},
                    {"stop",        {{"description", "stop sequences (string or array)"}}},
                    {"seed",        {{"type", "integer"}}},
                    {"tools",       {{"type", "array"}, {"items", {{"type", "object"}}}}},
                    {"tool_choice", {{"description", "auto | none | required | {type: function, ...}"}}},
                    {"response_format", {{"type", "object"}}},
                    {"chat_template_kwargs", {{"type", "object"},
                                              {"description", "e.g. {\"enable_thinking\": true} to enable reasoning blocks; disabled by default"}}},
                }},
            }},
        },
        {
            {"name", "lemonade_transcribe_audio"},
            {"description",
             "Transcribe an audio clip with a Whisper-class model. The "
             "Lemonade MCP server always runs on the same machine as the "
             "caller, so prefer `audio_path` (an absolute path to a local "
             "audio file: wav, mp3, m4a, ogg, flac, webm). Use "
             "`audio_base64` only when you genuinely have audio bytes in "
             "memory. Exactly one of the two must be provided. `model` is "
             "OPTIONAL: when omitted, the server reuses an already-loaded "
             "transcription model, else an already-downloaded one; if neither "
             "exists it asks you to pass a `model` or `allow_download: true` "
             "(which downloads the default, Whisper-Tiny)."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"model",         {{"type", "string"},
                                       {"description", "Optional. Omit to auto-select a loaded/downloaded model; defaults to Whisper-Tiny only with allow_download=true."}}},
                    {"allow_download", {{"type", "boolean"},
                                        {"description", "Permit downloading the default model when none is loaded or downloaded. Defaults to false."}}},
                    {"audio_path",    {{"type", "string"},
                                       {"description", "Absolute path to a local audio file. Preferred over audio_base64."}}},
                    {"audio_base64",  {{"type", "string"}}},
                    {"filename",      {{"type", "string"}}},
                    {"language",      {{"type", "string"}}},
                    {"prompt",        {{"type", "string"}}},
                    {"response_format", {{"type", "string"},
                                         {"enum", json::array({"json", "text", "srt", "verbose_json", "vtt"})}}},
                    {"temperature",   {{"type", "number"}}},
                }},
            }},
        },
        {
            {"name", "lemonade_generate_image"},
            {"description",
             "Generate one or more images from a text prompt. The Lemonade "
             "MCP server always runs on the same machine as the caller, so "
             "PREFER writing the result directly to disk by passing "
             "`output_path` (single image) or `output_dir` (one or more). "
             "When you do, the tool returns absolute file path(s) as text — "
             "no base64 round-trip and dramatically fewer tokens. Only omit "
             "both arguments when you genuinely need the image inline. For "
             "safety, disk writes are confined to a sandbox directory "
             "(<cache_dir>/mcp-images, or LEMONADE_MCP_IMAGE_DIR if set); "
             "paths outside it are rejected. `output_dir` writes get unique "
             "auto-generated filenames (so concurrent callers never clobber "
             "each other); use `output_path` when you need an exact name. "
             "`model` is OPTIONAL: when "
             "omitted, the server reuses an already-loaded image model, else "
             "an already-downloaded one; if neither exists it asks you to pass "
             "a `model` or `allow_download: true` (which downloads the "
             "default, SD-Turbo)."},
            {"inputSchema", {
                {"type", "object"},
                {"required", json::array({"prompt"})},
                {"properties", {
                    {"model",  {{"type", "string"},
                                {"description", "Optional. Omit to auto-select a loaded/downloaded image model; defaults to SD-Turbo only with allow_download=true."}}},
                    {"allow_download", {{"type", "boolean"},
                                        {"description", "Permit downloading the default model when none is loaded or downloaded. Defaults to false."}}},
                    {"prompt", {{"type", "string"}}},
                    {"output_path", {{"type", "string"},
                                     {"description", "Exact path of the PNG file to write, inside the MCP image sandbox (<cache_dir>/mcp-images or LEMONADE_MCP_IMAGE_DIR). Relative paths resolve against the sandbox root; absolute paths must stay within it. Written as named (overwrites if it already exists). Only valid when n == 1."}}},
                    {"output_dir",  {{"type", "string"},
                                     {"description", "Directory to write generated images into, inside the MCP image sandbox. Filenames are auto-generated and unique (image_<token>_<i>.png); the returned paths tell you the exact names. Relative paths resolve against the sandbox root; absolute paths must stay within it."}}},
                    {"size",   {{"type", "string"}}},
                    {"n",      {{"type", "integer"}, {"minimum", 1}}},
                    {"negative_prompt", {{"type", "string"}}},
                    {"seed",   {{"type", "integer"}}},
                    {"steps",  {{"type", "integer"}}},
                    {"cfg_scale", {{"type", "number"}}},
                }},
            }},
        },
        {
            {"name", "lemonade_omni"},
            {"description",
             "Multimodal turn against a Lemonade Omni collection (one tool "
             "call -> text + images + speech in the same response). The "
             "server runs an internal tool-calling loop against the "
             "collection's planner LLM and executes its image / image-edit / "
             "TTS tools by routing to the bundled component models; the "
             "result comes back as a text block plus native MCP `image` / "
             "`audio` content blocks (one per artifact). `model` is "
             "OPTIONAL: when omitted, the server reuses an already-downloaded "
             "Omni collection; if none is downloaded it asks you to pass a "
             "`model` or `allow_download: true` (which downloads the default, "
             "`LMX-Omni-5.5B-Lite`). Pass `model='LMX-Omni-52B-Halo'` (or any "
             "other recipe='collection.omni' model from `lemonade_list_models`) "
             "to opt into a larger collection; that model may be multi-GB. "
             "Same-machine deployment: PREFER `output_dir` to write artifacts "
             "to disk and avoid expensive inline base64 blobs. For plain text "
             "chat against a regular LLM, use `lemonade_chat` instead."},
            {"inputSchema", {
                {"type", "object"},
                {"required", json::array({"messages"})},
                {"properties", {
                    {"model",       {{"type", "string"},
                                     {"description", "Optional. Omni collection name (recipe='collection.omni'). Omit to reuse a downloaded collection; defaults to LMX-Omni-5.5B-Lite only with allow_download=true."}}},
                    {"allow_download", {{"type", "boolean"},
                                        {"description", "Permit downloading the default collection when none is downloaded. Defaults to false."}}},
                    {"messages",    {{"type", "array"}, {"items", {{"type", "object"}}}}},
                    {"output_dir",  {{"type", "string"},
                                     {"description", "Directory to write produced artifacts into, inside the MCP image sandbox (<cache_dir>/mcp-images or LEMONADE_MCP_IMAGE_DIR). Filenames are auto-generated and unique (omni_<token>_<i>.<ext>); the returned paths tell you the exact names. Relative paths resolve against the sandbox root; absolute paths must stay within it. PREFER this to inline base64 when caller and server share a filesystem. Omit to receive artifacts inline as MCP content blocks."}}},
                    {"temperature", {{"type", "number"}}},
                    {"top_p",       {{"type", "number"}}},
                    {"max_tokens",  {{"type", "integer"}}},
                    {"stop",        {{"description", "stop sequences (string or array)"}}},
                    {"seed",        {{"type", "integer"}}},
                    {"tools",       {{"type", "array"}, {"items", {{"type", "object"}}}}},
                    {"tool_choice", {{"description", "auto | none | required | {type: function, ...}"}}},
                    {"response_format", {{"type", "object"}}},
                    {"chat_template_kwargs", {{"type", "object"}}},
                }},
            }},
        },
    });
}

json McpServer::make_error_response(const json& id, int code, const std::string& message) {
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", {
            {"code", code},
            {"message", message},
        }},
    };
}

json McpServer::make_success_response(const json& id, json result) {
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", std::move(result)},
    };
}

json McpServer::text_content_block(const std::string& text) {
    return {
        {"type", "text"},
        {"text", text},
    };
}

}  // namespace lemon
