#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include "model_manager.h"

namespace lemon {

using json = nlohmann::json;

class Router;

// Server-side orchestrator for Omni "collection" models (recipe "collection.omni").
//
// A collection bundles several component models (a chat LLM, an image model, a
// TTS voice, etc.). This class lets a plain OpenAI /chat/completions request that
// targets the collection name behave like a single multimodal model: it injects
// the reference tool system prompt, runs an internal tool-calling loop against the
// chat component, executes the omni tools (image/edit/TTS) by routing to the
// matching component, and embeds the resulting media into the assistant message
// (markdown image data-URI, <audio> data-URI) so any OpenAI-compatible frontend
// (Open WebUI) renders it.
//
// Middleware semantics: app-provided tools are merged with the omni tools. Omni
// tool calls are resolved internally; any non-omni (app) tool calls are returned
// to the caller as a standard finish_reason:"tool_calls" response to execute and
// resume. The orchestrator holds no per-conversation state — the client re-sends
// full history and the server keys off the model name.
//
// This is a faithful port of the desktop frontend loop in
// src/app/src/renderer/utils/lemonadeTools.ts + LLMChatPanel.tsx::handleCollectionChat.
class CollectionOrchestrator {
public:
    // Loads a component model on demand (download + load). Wraps
    // Server::auto_load_model_if_needed so the orchestrator doesn't duplicate the
    // download/load logic. Throws on failure.
    using EnsureLoadedFn = std::function<void(const std::string&)>;

    CollectionOrchestrator(Router& router, ModelManager& model_manager, EnsureLoadedFn ensure_loaded);

    // Run the loop and return a complete OpenAI chat.completion JSON object.
    json chat_completion(const json& request, const ModelInfo& collection_info);

    // Run the loop and stream chat.completion.chunk SSE frames to `sink`
    // (terminated by `data: [DONE]`). Media is emitted as a content delta the
    // moment its tool finishes; the final text follows.
    void chat_completion_stream(const json& request, const ModelInfo& collection_info,
                                httplib::DataSink& sink);

private:
    // A piece of media produced by a tool this turn.
    struct Artifact {
        std::string type;  // "image" | "audio"
        std::string data;  // base64 payload
        std::string mime;  // e.g. "image/png", "audio/mpeg"
    };

    // Resolved tools + planner for a collection.
    struct ToolSet {
        json tools = json::array();                       // merged: omni tools + app tools
        std::string system_prompt;                        // omni prompt, {tool_list} substituted
        std::map<std::string, std::string> tool_models;   // omni tool name -> component model
                                                          // (its keys are the server-executed tools)
        std::string chat_model;                           // planner / chat component
        bool chat_supports_vision = false;                // planner carries the "vision" label
    };

    // Outcome of the internal loop, formatted by each public entry point.
    struct LoopResult {
        bool ok = true;
        json error = nullptr;                  // set when ok == false
        std::string final_text;                // terminal assistant text
        std::vector<Artifact> artifacts;       // media produced this turn, in order
        json app_tool_calls = nullptr;         // non-null array => passthrough to caller
        std::string finish_reason = "stop";    // "stop" | "tool_calls"
        json base_response = json::object();   // last component response (id/created/usage)
    };

    ToolSet build_tools(const ModelInfo& collection_info, const json& request);

    // Core loop. `on_artifact` is invoked as each artifact is produced (used by the
    // streaming path to emit media deltas immediately).
    LoopResult run_loop(const json& request, const ModelInfo& collection_info,
                        const std::function<void(const Artifact&)>& on_artifact);

    // Execute one omni tool call. Appends or replaces an entry in `artifacts` and
    // sets `produced_index` to that slot (or -1 if no media was produced).
    // `success_text` receives the role:"tool" content reported back to the model.
    // Returns true on success. Throws on backend error.
    bool execute_tool(const std::string& tool_name, const std::string& model,
                      const json& args, std::vector<Artifact>& artifacts,
                      const std::string& source_image_b64, const std::string& source_image_mime,
                      int& produced_index, std::string& success_text);

    // Render an artifact as the markdown/HTML a frontend (Open WebUI) displays.
    static std::string render_artifact(const Artifact& artifact);

    Router& router_;
    ModelManager& model_manager_;
    EnsureLoadedFn ensure_loaded_;
};

} // namespace lemon
