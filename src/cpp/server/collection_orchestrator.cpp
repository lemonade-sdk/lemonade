#include "lemon/collection_orchestrator.h"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <map>
#include <set>
#include <string>

#include "lemon/logging_config.h"
#include "lemon/model_types.h"
#include "lemon/router.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"
#include <lemon/utils/aixlog.hpp>

namespace lemon {

namespace {

// Labels that identify a concrete non-chat component. A planner candidate is a
// component carrying none of these. Mirrors NON_CHAT_PLANNER_LABELS in
// src/app/src/renderer/utils/modelLabels.ts.
const std::set<std::string>& non_chat_planner_labels() {
    static const std::set<std::string> labels = {
        "image", "speech", "tts", "transcription", "embeddings",
        "embedding", "reranking", "edit", "esrgan",
    };
    return labels;
}

// Omni tools the server executes internally. v1 scope: image gen/edit + TTS.
// (transcribe_audio and analyze_image stay client-path tools — see
// docs/dev/lemonade-omni.md.)
const std::set<std::string>& supported_omni_tools() {
    static const std::set<std::string> tools = {
        "generate_image", "edit_image", "text_to_speech",
    };
    return tools;
}

// The canonical tool definitions, staged from the frontend's toolDefinitions.json.
// Loaded once; returns an empty object if missing so callers degrade gracefully.
const json& tool_definitions() {
    static const json defs = [] {
        try {
            return utils::JsonUtils::load_from_file(
                utils::get_resource_path("resources/toolDefinitions.json"));
        } catch (const std::exception& e) {
            LOG(ERROR, "Collection") << "Failed to load toolDefinitions.json: " << e.what() << std::endl;
            return json::object();
        }
    }();
    return defs;
}

// Fixed image size for collection-mode image tools, read from toolDefinitions.json
// (the single source of truth shared with the desktop app's collectionImageConfig.ts;
// both sides target SDServer::resolve_size). 2:1, 64-aligned. Falls back to a sane
// default if the field is missing.
const std::string& image_size() {
    static const std::string size = [] {
        const json& defs = tool_definitions();
        if (defs.contains("image_size") && defs["image_size"].is_string()) {
            return defs["image_size"].get<std::string>();
        }
        return std::string("512x256");
    }();
    return size;
}

std::string new_completion_id() {
    static std::atomic<uint64_t> counter{0};
    return "chatcmpl-omni-" + std::to_string(static_cast<long>(std::time(nullptr))) +
           "-" + std::to_string(counter.fetch_add(1));
}

bool labels_intersect(const std::vector<std::string>& labels, const std::set<std::string>& wanted) {
    for (const auto& l : labels) {
        if (wanted.count(l)) return true;
    }
    return false;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

// Substitute the {image_size} placeholder in a tool's parameter descriptions.
json materialize_tool(const json& def) {
    json tool = {{"type", "function"}, {"function", def.value("function", json::object())}};
    if (tool["function"].contains("parameters") &&
        tool["function"]["parameters"].contains("properties")) {
        for (auto& [key, prop] : tool["function"]["parameters"]["properties"].items()) {
            if (prop.contains("description") && prop["description"].is_string()) {
                prop["description"] =
                    replace_all(prop["description"].get<std::string>(), "{image_size}", image_size());
            }
        }
    }
    return tool;
}

std::string extract_b64(const json& image_response) {
    if (image_response.contains("data") && image_response["data"].is_array() &&
        !image_response["data"].empty() && image_response["data"][0].contains("b64_json")) {
        return image_response["data"][0]["b64_json"].get<std::string>();
    }
    return "";
}

std::string backend_error_message(const json& response, const std::string& fallback) {
    if (response.contains("error")) {
        if (response["error"].is_object() && response["error"].contains("message")) {
            return response["error"]["message"].get<std::string>();
        }
        if (response["error"].is_string()) return response["error"].get<std::string>();
    }
    return fallback;
}

json make_error_response(const std::string& message) {
    return {{"error", {{"message", message}, {"type", "internal_error"}}}};
}

// Replace every embedded `data:<mime>;base64,<payload>` media element in `text`
// with a short plain-text placeholder. Frontends echo our embedded media back in
// the next turn's history; left intact, one image is hundreds of thousands of
// tokens. The whole markdown/HTML wrapper is removed (not just the payload) so the
// planner sees "[generated image]" rather than a still-referenceable data URI it
// would copy into its answer as a broken image. The most recent image's mime +
// payload is captured (last wins) as an edit source.
std::string strip_base64_data_uris(const std::string& text, std::string& last_image_b64,
                                   std::string& last_image_mime) {
    static const std::string marker = ";base64,";
    auto is_b64 = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
    };
    std::string out;
    out.reserve(text.size());
    size_t pos = 0;
    while (true) {
        const size_t b = text.find(marker, pos);
        if (b == std::string::npos) {
            out.append(text, pos, std::string::npos);
            break;
        }
        const size_t data_at = text.rfind("data:", b);
        const size_t payload_start = b + marker.size();
        size_t e = payload_start;
        while (e < text.size() && is_b64(text[e])) ++e;

        std::string mime;
        if (data_at != std::string::npos && data_at < b) {
            mime = text.substr(data_at + 5, b - (data_at + 5));
        }
        // Only copy the (large) payload when it's the image we need as an edit
        // source — audio payloads are skipped to avoid the allocation.
        if (mime.rfind("image/", 0) == 0 && e > payload_start) {
            last_image_b64 = text.substr(payload_start, e - payload_start);
            last_image_mime = mime;
        }

        // Expand to the full media element so no `data:`/markdown survives.
        size_t elem_start = (data_at != std::string::npos) ? data_at : b;
        size_t elem_end = e;
        std::string placeholder = mime.rfind("audio/", 0) == 0 ? "[generated audio]"
                                                               : "[generated image]";
        // markdown image: ![alt](data:...)
        if (elem_start >= 2 && text[elem_start - 1] == '(' && text[elem_start - 2] == ']') {
            const size_t bang = text.rfind("![", elem_start - 2);
            if (bang != std::string::npos && e < text.size() && text[e] == ')') {
                elem_start = bang;
                elem_end = e + 1;
            }
        // <audio>data:...</audio>
        } else if (elem_start >= 7 && text.compare(elem_start - 7, 7, "<audio>") == 0) {
            static const std::string close = "</audio>";
            if (text.compare(e, close.size(), close) == 0) {
                elem_start = elem_start - 7;
                elem_end = e + close.size();
            }
        }

        out.append(text, pos, elem_start - pos);
        out.append(placeholder);
        pos = elem_end;
    }
    return out;
}

} // namespace

CollectionOrchestrator::CollectionOrchestrator(Router& router, ModelManager& model_manager,
                                               EnsureLoadedFn ensure_loaded)
    : router_(router), model_manager_(model_manager), ensure_loaded_(std::move(ensure_loaded)) {}

std::string CollectionOrchestrator::render_artifact(const Artifact& artifact) {
    const std::string data_uri = "data:" + artifact.mime + ";base64," + artifact.data;
    if (artifact.type == "image") return "![generated image](" + data_uri + ")";
    // The tag must be alone on its line so the markdown parser (Open WebUI's
    // marked) treats it as one HTML block — otherwise the data-URI renders as
    // plain text instead of an <audio> player. The browser strips the
    // surrounding newlines from the resolved src.
    if (artifact.type == "audio") return "<audio>\n" + data_uri + "\n</audio>";
    return "";
}

CollectionOrchestrator::ToolSet CollectionOrchestrator::build_tools(const ModelInfo& collection_info,
                                                                    const json& request) {
    ToolSet result;
    const auto& components = collection_info.components;

    // Cache each component's labels (one ModelManager lookup per component).
    std::map<std::string, std::vector<std::string>> labels;
    for (const auto& c : components) {
        try {
            labels[c] = model_manager_.get_model_info(c).labels;
        } catch (const std::exception&) {
            labels[c] = {};
        }
    }

    // Pick the chat/planner component: first that carries no non-chat label,
    // else the first component.
    for (const auto& c : components) {
        if (!labels_intersect(labels[c], non_chat_planner_labels())) {
            result.chat_model = c;
            break;
        }
    }
    if (result.chat_model.empty() && !components.empty()) {
        result.chat_model = components.front();
    }

    // A vision-capable planner can read uploaded images directly, so the loop
    // forwards user `image_url` parts to it instead of stripping them to a
    // placeholder (see run_loop's message pre-processing).
    if (!result.chat_model.empty()) {
        const auto& cm_labels = labels[result.chat_model];
        result.chat_supports_vision =
            std::find(cm_labels.begin(), cm_labels.end(), "vision") != cm_labels.end();
    }

    const json& defs = tool_definitions();
    std::string tool_list;
    // Per-tool prompt guidance, collected only for the tools actually included
    // so the prompt never references a tool the planner doesn't have (e.g.
    // analyze_image / transcribe_audio, which are not server-side tools).
    std::string tool_guidance;
    if (defs.contains("tools") && defs["tools"].is_array()) {
        for (const auto& def : defs["tools"]) {
            const std::string name = def.value("function", json::object()).value("name", "");
            if (!supported_omni_tools().count(name)) continue;  // v1 scope

            std::string match_model;
            if (def.contains("requires_labels") && def["requires_labels"].is_array()) {
                std::set<std::string> required(def["requires_labels"].begin(),
                                               def["requires_labels"].end());
                for (const auto& c : components) {
                    if (labels_intersect(labels[c], required)) {
                        match_model = c;
                        break;
                    }
                }
                if (match_model.empty()) continue;
            } else if (def.contains("requires_llm_labels") && def["requires_llm_labels"].is_array()) {
                std::set<std::string> required(def["requires_llm_labels"].begin(),
                                               def["requires_llm_labels"].end());
                if (result.chat_model.empty() ||
                    !labels_intersect(labels[result.chat_model], required)) {
                    continue;
                }
                match_model = result.chat_model;
            } else {
                continue;
            }

            json tool = materialize_tool(def);
            const std::string desc = tool["function"].value("description", "");
            tool_list += "- " + name + ": " + desc + "\n";
            result.tools.push_back(std::move(tool));
            result.tool_models[name] = match_model;
            const std::string guidance = def.value("prompt_guidance", "");
            if (!guidance.empty()) tool_guidance += "\n" + guidance;
        }
    }
    if (!tool_list.empty() && tool_list.back() == '\n') tool_list.pop_back();

    // Build the omni system prompt.
    if (defs.contains("system_prompt") && defs["system_prompt"].is_string()) {
        std::string prompt = replace_all(defs["system_prompt"].get<std::string>(),
                                         "{tool_list}", tool_list);
        result.system_prompt = replace_all(prompt, "{tool_guidance}", tool_guidance);
    }

    // Merge app-provided tools (omni tools win on name collision).
    if (request.contains("tools") && request["tools"].is_array()) {
        for (const auto& app_tool : request["tools"]) {
            const std::string name = app_tool.value("function", json::object()).value("name", "");
            if (!name.empty() && result.tool_models.count(name)) continue;
            result.tools.push_back(app_tool);
        }
    }

    return result;
}

bool CollectionOrchestrator::execute_tool(const std::string& tool_name, const std::string& model,
                                          const json& args, std::vector<Artifact>& artifacts,
                                          const std::string& source_image_b64,
                                          const std::string& source_image_mime,
                                          int& produced_index, std::string& success_text) {
    produced_index = -1;

    if (tool_name == "generate_image" || tool_name == "edit_image") {
        const std::string prompt = args.value("prompt", "");

        bool want_edit = (tool_name == "edit_image");
        const bool has_prev_image =
            std::any_of(artifacts.begin(), artifacts.end(),
                        [](const Artifact& a) { return a.type == "image"; });
        // Auto-switch generate->edit when a prior image exists and the model can edit.
        if (!want_edit && has_prev_image) {
            try {
                const auto info = model_manager_.get_model_info(model);
                if (std::find(info.labels.begin(), info.labels.end(), "edit") != info.labels.end()) {
                    want_edit = true;
                }
            } catch (const std::exception&) {}
        }

        if (want_edit) {
            // Source: most recent generated image, else the seeded history image.
            std::string img_b64, img_mime;
            for (auto it = artifacts.rbegin(); it != artifacts.rend(); ++it) {
                if (it->type == "image") {
                    img_b64 = it->data;
                    img_mime = it->mime;
                    break;
                }
            }
            if (img_b64.empty()) {
                img_b64 = source_image_b64;
                img_mime = source_image_mime.empty() ? "image/png" : source_image_mime;
            }
            if (!img_b64.empty()) {
                json req = {{"model", model}, {"prompt", prompt}, {"response_format", "b64_json"},
                            {"n", 1}, {"size", image_size()}, {"image_data", img_b64},
                            {"image_filename", "image.png"}};
                LOG(INFO, "Collection") << "image_edits: editing source image (" << img_b64.size()
                                        << " b64 chars)" << std::endl;
                json resp = router_.image_edits(req);
                const std::string b64 = extract_b64(resp);
                if (b64.empty()) throw std::runtime_error(backend_error_message(resp, "Image edit failed"));

                // Replace the last generated image this turn, else append.
                int last = -1;
                for (int i = static_cast<int>(artifacts.size()) - 1; i >= 0; --i) {
                    if (artifacts[i].type == "image") { last = i; break; }
                }
                if (last >= 0) {
                    artifacts[last] = Artifact{"image", b64, "image/png"};
                    produced_index = last;
                } else {
                    artifacts.push_back(Artifact{"image", b64, "image/png"});
                    produced_index = static_cast<int>(artifacts.size()) - 1;
                }
                success_text = "Image edited successfully.";
                return true;
            }
            // No source image available — fall through to a fresh generation.
        }

        json req = {{"model", model}, {"prompt", prompt}, {"response_format", "b64_json"},
                    {"n", 1}, {"size", image_size()}};
        LOG(INFO, "Collection") << "image_generations: fresh generation" << std::endl;
        json resp = router_.image_generations(req);
        const std::string b64 = extract_b64(resp);
        if (b64.empty()) throw std::runtime_error(backend_error_message(resp, "Image generation failed"));
        artifacts.push_back(Artifact{"image", b64, "image/png"});
        produced_index = static_cast<int>(artifacts.size()) - 1;
        success_text = "Image generated successfully.";
        return true;
    }

    if (tool_name == "text_to_speech") {
        const std::string input = args.value("input", "");
        const std::string voice = args.value("voice", "af_heart");
        json req = {{"model", model}, {"input", input}, {"voice", voice},
                    {"response_format", "mp3"}};

        // Capture the streamed audio into an in-memory buffer instead of an HTTP
        // sink. All of write/is_writable/done must be set — the byte-stream proxy
        // calls sink.done() to finish, and an empty std::function would throw
        // bad_function_call (whose error handler would then corrupt the buffer).
        std::string buffer;
        httplib::DataSink sink;
        sink.write = [&buffer](const char* data, size_t len) {
            buffer.append(data, len);
            return true;
        };
        sink.is_writable = []() { return true; };
        sink.done = []() {};
        router_.audio_speech(req, sink);
        if (buffer.empty()) throw std::runtime_error("Text-to-speech produced no audio");

        artifacts.push_back(Artifact{"audio", utils::JsonUtils::base64_encode(buffer), "audio/mpeg"});
        produced_index = static_cast<int>(artifacts.size()) - 1;
        success_text = "Audio generated successfully.";
        return true;
    }

    success_text = "Unknown tool: " + tool_name;
    return false;
}

CollectionOrchestrator::LoopResult CollectionOrchestrator::run_loop(
    const json& request, const ModelInfo& collection_info,
    const std::function<void(const Artifact&)>& on_artifact) {
    LoopResult result;

    ToolSet toolset = build_tools(collection_info, request);
    if (toolset.chat_model.empty()) {
        result.ok = false;
        result.error = make_error_response("Collection '" + collection_info.model_name +
                                           "' has no chat-capable component");
        return result;
    }

    // Pre-process messages: strip binary parts to text placeholders and seed an
    // edit source image from history.
    const json messages = request.contains("messages") && request["messages"].is_array()
                              ? request["messages"]
                              : json::array();
    json processed = json::array();
    int user_image_count = 0;
    std::string source_image_b64, source_image_mime;

    // Forward pass: the most recent image found becomes the edit source (last
    // wins). Two image carriers exist — base64 data-URIs embedded in a content
    // string (how this server returns generated media; frontends echo it back),
    // and `image_url` content-array parts (user uploads / OpenAI vision format).
    // Embedded data-URIs and generated-image echoes are stripped to small
    // placeholders so the planner prompt stays tiny. User-uploaded `image_url`
    // parts are instead forwarded intact when the planner is vision-capable, so
    // a plain "what's in this image?" turn actually reaches its vision encoder
    // (it stays an edit source either way).
    for (const auto& msg : messages) {
        const std::string role = msg.value("role", "");
        const bool is_user = (role == "user");

        // Sanitize in place: replace only `content`, keeping every other field
        // (tool_calls, tool_call_id, name, ...). A client resuming an app tool
        // call echoes the assistant turn with its tool_calls plus a role:"tool"
        // result keyed by tool_call_id — dropping those hands the planner an
        // orphaned tool result, which strict chat templates reject.
        if (msg.contains("content") && msg["content"].is_string()) {
            std::string img_b64, img_mime;
            std::string cleaned =
                strip_base64_data_uris(msg["content"].get<std::string>(), img_b64, img_mime);
            if (!img_b64.empty()) {
                source_image_b64 = img_b64;
                source_image_mime = img_mime;
            }
            json out = msg;
            out["content"] = std::move(cleaned);
            processed.push_back(std::move(out));
            continue;
        }
        if (!msg.contains("content") || !msg["content"].is_array()) {
            json out = msg;
            if (!out.contains("content")) out["content"] = "";
            processed.push_back(std::move(out));
            continue;
        }

        json new_content = json::array();
        for (const auto& item : msg["content"]) {
            const std::string itype = item.value("type", "");
            if (itype == "image_url" && item.contains("image_url")) {
                const std::string url = item["image_url"].value("url", "");
                const auto pos = url.find(";base64,");
                if (pos != std::string::npos && url.rfind("data:", 0) == 0) {
                    source_image_b64 = url.substr(pos + 8);
                    source_image_mime = url.substr(5, pos - 5);
                }
                if (is_user) {
                    ++user_image_count;
                    if (toolset.chat_supports_vision) {
                        // Forward the upload so the vision planner can actually
                        // read it; already captured above as an edit source.
                        new_content.push_back(item);
                    } else {
                        new_content.push_back({{"type", "text"},
                                               {"text", "[User provided image #" +
                                                            std::to_string(user_image_count) + "]"}});
                    }
                } else {
                    new_content.push_back({{"type", "text"}, {"text", "[Generated image]"}});
                }
            } else if (itype == "input_audio" && item.contains("input_audio")) {
                if (is_user) {
                    new_content.push_back({{"type", "text"}, {"text", "[User provided audio file]"}});
                }
            } else {
                new_content.push_back(item);
            }
        }
        json out = msg;
        if (new_content.size() == 1 && new_content[0].value("type", "") == "text") {
            out["content"] = new_content[0]["text"];
        } else {
            out["content"] = new_content.empty() ? json("") : new_content;
        }
        processed.push_back(std::move(out));
    }

    // Prepend (merge) the omni system prompt.
    if (!toolset.system_prompt.empty()) {
        if (!processed.empty() && processed[0].value("role", "") == "system") {
            const std::string existing =
                processed[0]["content"].is_string() ? processed[0]["content"].get<std::string>() : "";
            processed[0]["content"] = toolset.system_prompt + "\n\n" + existing;
        } else {
            processed.insert(processed.begin(),
                             json{{"role", "system"}, {"content", toolset.system_prompt}});
        }
    }

    // The caller (handle_collection_chat_completions) loads the whole collection
    // up front via the shared loader. Re-assert the chat component here as a cheap
    // no-op safety net in case it was evicted between load and use.
    ensure_loaded_(toolset.chat_model);

    json llm_messages = std::move(processed);
    std::vector<Artifact> artifacts;
    json base_response = json::object();
    constexpr int MAX_ITERATIONS = 5;

    // Build the per-call request once, carrying through sampling params but
    // dropping the original (possibly huge base64) messages — `llm_messages`
    // (stripped) is substituted each iteration.
    json req = request;
    req.erase("messages");
    req.erase("stream_options");
    req["model"] = toolset.chat_model;
    req["tools"] = toolset.tools;
    req["stream"] = false;

    for (int iteration = 0; iteration < MAX_ITERATIONS; ++iteration) {
        req["messages"] = llm_messages;

        json response = router_.chat_completion(req);
        if (response.contains("error")) {
            result.ok = false;
            result.error = std::move(response);
            return result;
        }
        if (!response.contains("choices") || !response["choices"].is_array() ||
            response["choices"].empty() || !response["choices"][0].contains("message")) {
            result.ok = false;
            result.error = make_error_response("Chat component returned an empty response");
            return result;
        }
        base_response = response;
        const json assistant_msg = response["choices"][0]["message"];

        const bool has_tool_calls = assistant_msg.contains("tool_calls") &&
                                    assistant_msg["tool_calls"].is_array() &&
                                    !assistant_msg["tool_calls"].empty();
        const std::string assistant_text =
            assistant_msg.contains("content") && assistant_msg["content"].is_string()
                ? assistant_msg["content"].get<std::string>()
                : "";

        if (!has_tool_calls) {
            result.final_text = assistant_text;
            result.artifacts = std::move(artifacts);
            result.base_response = std::move(response);
            return result;
        }

        // Partition the calls into omni (server-run) and app (passed back).
        json omni_calls = json::array();
        json app_calls = json::array();
        for (const auto& tc : assistant_msg["tool_calls"]) {
            const std::string name = tc.value("function", json::object()).value("name", "");
            if (toolset.tool_models.count(name)) {
                omni_calls.push_back(tc);
            } else {
                app_calls.push_back(tc);
            }
        }

        llm_messages.push_back(assistant_msg);

        for (const auto& tc : omni_calls) {
            const std::string name = tc.value("function", json::object()).value("name", "");
            const std::string model = toolset.tool_models.count(name) ? toolset.tool_models[name] : "";
            json args = json::object();
            try {
                args = json::parse(tc["function"].value("arguments", "{}"));
            } catch (const std::exception&) {}

            std::string success_text;
            int produced_index = -1;
            if (model.empty()) {
                success_text = "Error: tool '" + name + "' has no available model";
            } else {
                try {
                    LOG(INFO, "Collection") << "Tool call: " << name << " -> " << model << std::endl;
                    ensure_loaded_(model);
                    execute_tool(name, model, args, artifacts, source_image_b64, source_image_mime,
                                 produced_index, success_text);
                } catch (const std::exception& e) {
                    success_text = std::string("Error: ") + e.what();
                    produced_index = -1;
                }
            }
            if (produced_index >= 0 && produced_index < static_cast<int>(artifacts.size())) {
                on_artifact(artifacts[produced_index]);
            }
            llm_messages.push_back({{"role", "tool"},
                                    {"tool_call_id", tc.value("id", "")},
                                    {"content", success_text}});
        }

        // Mixed/app turn: fold omni media into content (already collected in
        // `artifacts`) and hand the app calls back to the caller to resume.
        if (!app_calls.empty()) {
            result.final_text = assistant_text;
            result.artifacts = std::move(artifacts);
            result.app_tool_calls = std::move(app_calls);
            result.finish_reason = "tool_calls";
            result.base_response = std::move(response);
            return result;
        }
        // Omni-only turn: loop again so the model can produce its final answer.
    }

    // Hit the iteration cap: the planner kept calling tools without ever
    // returning a final text answer. Log it so a stuck loop is diagnosable
    // rather than silently truncated.
    LOG(WARNING, "Collection") << "Tool-calling loop hit the " << MAX_ITERATIONS
                               << "-iteration cap for collection '" << collection_info.model_name
                               << "' (" << artifacts.size() << " artifact(s) produced)" << std::endl;
    result.final_text = artifacts.empty() ? "Sorry, I was unable to complete that request." : "";
    result.artifacts = std::move(artifacts);
    result.base_response = std::move(base_response);
    return result;
}

json CollectionOrchestrator::chat_completion(const json& request, const ModelInfo& collection_info) {
    LoopResult lr;
    try {
        lr = run_loop(request, collection_info, [](const Artifact&) {});
    } catch (const std::exception& e) {
        return make_error_response(e.what());
    }
    if (!lr.ok) return lr.error;

    std::string content = lr.final_text;
    for (const auto& a : lr.artifacts) {
        if (!content.empty()) content += "\n\n";
        content += render_artifact(a);
    }

    json response = (lr.base_response.is_object() && !lr.base_response.empty())
                        ? lr.base_response
                        : json{{"id", new_completion_id()},
                               {"created", static_cast<long>(std::time(nullptr))}};
    response["object"] = "chat.completion";
    response["model"] = request.value("model", collection_info.model_name);

    json message = {{"role", "assistant"}, {"content", content}};
    if (lr.app_tool_calls.is_array() && !lr.app_tool_calls.empty()) {
        message["tool_calls"] = lr.app_tool_calls;
    }
    response["choices"] = json::array({{{"index", 0},
                                        {"message", message},
                                        {"finish_reason", lr.finish_reason}}});
    return response;
}

void CollectionOrchestrator::chat_completion_stream(const json& request,
                                                    const ModelInfo& collection_info,
                                                    httplib::DataSink& sink) {
    const std::string model = request.value("model", collection_info.model_name);
    const std::string id = new_completion_id();
    const long created = static_cast<long>(std::time(nullptr));

    auto send = [&](const json& delta, const json& finish_reason) {
        json chunk = {{"id", id},
                      {"object", "chat.completion.chunk"},
                      {"created", created},
                      {"model", model},
                      {"choices", json::array({{{"index", 0},
                                                {"delta", delta},
                                                {"finish_reason", finish_reason}}})}};
        const std::string frame = "data: " + chunk.dump() + "\n\n";
        sink.write(frame.c_str(), frame.size());
    };
    // Emit content in slices so no single SSE line exceeds a client's line-length
    // limit (e.g. aiohttp's 128 KiB) — a base64 image is far larger. Clients
    // concatenate delta.content across chunks. Slices avoid splitting a UTF-8
    // sequence so each delta is valid JSON.
    auto send_content = [&](const std::string& text) {
        constexpr size_t MAX = 16000;
        size_t i = 0;
        while (i < text.size()) {
            size_t len = std::min(MAX, text.size() - i);
            if (i + len < text.size()) {
                while (len > 0 && (static_cast<unsigned char>(text[i + len]) & 0xC0) == 0x80) --len;
                if (len == 0) len = std::min(MAX, text.size() - i);  // no boundary found
            }
            send(json{{"content", text.substr(i, len)}}, nullptr);
            i += len;
        }
    };
    auto send_done = [&]() {
        const std::string done = "data: [DONE]\n\n";
        sink.write(done.c_str(), done.size());
        sink.done();  // terminate the chunked transfer (final 0-length chunk)
    };
    auto send_error = [&](const std::string& message) {
        send(json{{"content", "\n\nError: " + message}}, nullptr);
        send(json::object(), "stop");
        send_done();
    };

    send(json{{"role", "assistant"}}, nullptr);

    LoopResult lr;
    try {
        lr = run_loop(request, collection_info, [&](const Artifact& a) {
            send_content("\n\n" + render_artifact(a) + "\n\n");
        });
    } catch (const std::exception& e) {
        send_error(e.what());
        return;
    }

    if (!lr.ok) {
        const std::string msg =
            lr.error.is_object() && lr.error.contains("error") && lr.error["error"].is_object()
                ? lr.error["error"].value("message", "error")
                : "error";
        send_error(msg);
        return;
    }

    if (!lr.final_text.empty()) {
        send_content(lr.final_text);
    }
    if (lr.app_tool_calls.is_array() && !lr.app_tool_calls.empty()) {
        // Normalize to the OpenAI streaming tool-call shape: each delta entry
        // must carry an integer `index` so clients merge fragments by slot.
        // The non-streaming message.tool_calls objects we pass through omit it,
        // and the OpenAI SDK rejects an index-less tool-call delta.
        json stream_tool_calls = json::array();
        for (size_t i = 0; i < lr.app_tool_calls.size(); ++i) {
            json tc = lr.app_tool_calls[i];
            tc["index"] = static_cast<int>(i);
            stream_tool_calls.push_back(std::move(tc));
        }
        send(json{{"tool_calls", std::move(stream_tool_calls)}}, nullptr);
    }
    send(json::object(), lr.finish_reason);
    send_done();
}

} // namespace lemon
