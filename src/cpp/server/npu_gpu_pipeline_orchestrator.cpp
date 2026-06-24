#include "lemon/npu_gpu_pipeline_orchestrator.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <ctime>
#include <stdexcept>

#include "lemon/error_types.h"
#include "lemon/model_types.h"
#include <lemon/utils/aixlog.hpp>

namespace lemon {

namespace {

std::string new_pipeline_completion_id() {
    static std::atomic<uint64_t> counter{0};
    return "chatcmpl-npu-gpu-" + std::to_string(static_cast<long>(std::time(nullptr))) +
           "-" + std::to_string(counter.fetch_add(1));
}

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), text.begin());
}

std::string trim_copy(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

int json_int_or(const json& value, int fallback) {
    if (value.is_number_integer()) return value.get<int>();
    if (value.is_number_unsigned()) return static_cast<int>(value.get<unsigned int>());
    if (value.is_string()) {
        try {
            return std::stoi(value.get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

} // namespace

NpuGpuPipelineOrchestrator::NpuGpuPipelineOrchestrator(Router& router,
                                                       ModelManager& model_manager,
                                                       AutoLoadFn auto_load)
    : router_(router), model_manager_(model_manager), auto_load_(std::move(auto_load)) {}

NpuGpuPipelineOrchestrator::Components
NpuGpuPipelineOrchestrator::resolve_components(const ModelInfo& collection_info) const {
    if (collection_info.components.size() != 2) {
        throw std::runtime_error(
            "collection.npu_gpu requires exactly two components: an FLM/NPU draft model "
            "followed by a GPU verifier model");
    }

    Components components{collection_info.components[0], collection_info.components[1]};
    ModelInfo draft_info = model_manager_.get_model_info(components.draft_model);
    ModelInfo verifier_info = model_manager_.get_model_info(components.verifier_model);

    if (draft_info.recipe != "flm" || !(draft_info.device & DEVICE_NPU)) {
        throw std::runtime_error(
            "collection.npu_gpu first component must be an FLM/NPU model: " +
            components.draft_model);
    }
    if (verifier_info.type != ModelType::LLM || verifier_info.recipe == "flm") {
        throw std::runtime_error(
            "collection.npu_gpu second component must be a non-FLM LLM verifier model: " +
            components.verifier_model);
    }

    return components;
}

int NpuGpuPipelineOrchestrator::resolve_max_tokens(const json& request) const {
    if (request.contains("max_completion_tokens")) {
        return std::max(1, json_int_or(request["max_completion_tokens"], 2048));
    }
    if (request.contains("max_tokens")) {
        return std::max(1, json_int_or(request["max_tokens"], 2048));
    }
    return 2048;
}

int NpuGpuPipelineOrchestrator::resolve_draft_tokens(const json& request,
                                                     const ModelInfo& collection_info,
                                                     int max_tokens) const {
    int configured = 128;
    json options = collection_info.recipe_options.to_json();
    if (options.contains("draft_tokens")) {
        configured = json_int_or(options["draft_tokens"], configured);
    }
    if (request.contains("npu_draft_tokens")) {
        configured = json_int_or(request["npu_draft_tokens"], configured);
    }
    configured = std::max(1, configured);
    return std::max(1, std::min(configured, std::max(1, max_tokens / 2)));
}

json NpuGpuPipelineOrchestrator::make_draft_request(const json& request,
                                                    const std::string& draft_model,
                                                    int draft_tokens) const {
    json draft = request;
    draft["model"] = draft_model;
    draft["stream"] = false;
    draft["max_tokens"] = draft_tokens;
    draft.erase("max_completion_tokens");
    // Draft models are used only to produce text prefix. Tool and structured-output
    // directives belong to the verifier pass, where the full model can honor them.
    draft.erase("tools");
    draft.erase("tool_choice");
    draft.erase("parallel_tool_calls");
    draft.erase("response_format");
    draft.erase("npu_draft_tokens");
    return draft;
}

json NpuGpuPipelineOrchestrator::make_verifier_request(const json& request,
                                                       const std::string& verifier_model,
                                                       const std::string& draft_text,
                                                       int verifier_tokens,
                                                       bool stream) const {
    json verifier = request;
    verifier["model"] = verifier_model;
    verifier["stream"] = stream;
    verifier["max_tokens"] = std::max(1, verifier_tokens);
    verifier.erase("max_completion_tokens");
    verifier.erase("npu_draft_tokens");

    if (!draft_text.empty()) {
        json messages = verifier.value("messages", json::array());
        if (!messages.is_array()) {
            throw std::runtime_error("chat completion request must contain a messages array");
        }
        messages.push_back({{"role", "assistant"}, {"content", draft_text}});
        verifier["messages"] = messages;
    }
    return verifier;
}

std::string NpuGpuPipelineOrchestrator::extract_message_content(const json& response) const {
    try {
        if (response.contains("choices") && response["choices"].is_array() &&
            !response["choices"].empty()) {
            const json& choice = response["choices"][0];
            if (choice.contains("message") && choice["message"].is_object() &&
                choice["message"].contains("content") &&
                choice["message"]["content"].is_string()) {
                return choice["message"]["content"].get<std::string>();
            }
            if (choice.contains("text") && choice["text"].is_string()) {
                return choice["text"].get<std::string>();
            }
        }
    } catch (...) {
    }
    return "";
}

std::string NpuGpuPipelineOrchestrator::strip_duplicate_prefix(const std::string& text,
                                                               const std::string& prefix) const {
    if (prefix.empty() || text.empty()) return text;
    if (starts_with(text, prefix)) {
        return text.substr(prefix.size());
    }
    const std::string trimmed_text = trim_copy(text);
    const std::string trimmed_prefix = trim_copy(prefix);
    if (!trimmed_prefix.empty() && starts_with(trimmed_text, trimmed_prefix)) {
        return trim_copy(trimmed_text.substr(trimmed_prefix.size()));
    }
    return text;
}

json NpuGpuPipelineOrchestrator::make_response(const json& request,
                                               const ModelInfo& collection_info,
                                               const std::string& content,
                                               const json& draft_response,
                                               const json& verifier_response) const {
    const std::string model = request.value("model", collection_info.model_name);
    json response = {
        {"id", new_pipeline_completion_id()},
        {"object", "chat.completion"},
        {"created", static_cast<long>(std::time(nullptr))},
        {"model", model},
        {"choices", json::array({{
            {"index", 0},
            {"message", {{"role", "assistant"}, {"content", content}}},
            {"finish_reason", "stop"}
        }})},
        {"lemonade_pipeline", {
            {"type", COLLECTION_NPU_GPU_MODEL_RECIPE},
            {"draft_response", draft_response},
            {"verifier_response", verifier_response}
        }}
    };

    int prompt_tokens = 0;
    int completion_tokens = 0;
    if (draft_response.contains("usage")) {
        prompt_tokens += draft_response["usage"].value("prompt_tokens", 0);
        completion_tokens += draft_response["usage"].value("completion_tokens", 0);
    }
    if (verifier_response.contains("usage")) {
        prompt_tokens += verifier_response["usage"].value("prompt_tokens", 0);
        completion_tokens += verifier_response["usage"].value("completion_tokens", 0);
    }
    response["usage"] = {
        {"prompt_tokens", prompt_tokens},
        {"completion_tokens", completion_tokens},
        {"total_tokens", prompt_tokens + completion_tokens}
    };
    return response;
}

json NpuGpuPipelineOrchestrator::chat_completion(const json& request,
                                                 const ModelInfo& collection_info) {
    Components components = resolve_components(collection_info);
    auto_load_(components.draft_model);
    auto_load_(components.verifier_model);

    const int max_tokens = resolve_max_tokens(request);
    const int draft_tokens = resolve_draft_tokens(request, collection_info, max_tokens);
    const int verifier_tokens = std::max(1, max_tokens - draft_tokens);

    json draft_request = make_draft_request(request, components.draft_model, draft_tokens);
    json draft_response = router_.chat_completion(draft_request);
    if (draft_response.contains("error")) return draft_response;

    std::string draft_text = extract_message_content(draft_response);

    json verifier_request = make_verifier_request(request, components.verifier_model,
                                                  draft_text, verifier_tokens, false);
    json verifier_response = router_.chat_completion(verifier_request);
    if (verifier_response.contains("error")) return verifier_response;

    std::string verifier_text = strip_duplicate_prefix(extract_message_content(verifier_response),
                                                       draft_text);
    return make_response(request, collection_info, draft_text + verifier_text,
                         draft_response, verifier_response);
}

void NpuGpuPipelineOrchestrator::write_sse_chunk(httplib::DataSink& sink,
                                                 const std::string& id,
                                                 long created,
                                                 const std::string& model,
                                                 const std::string& content,
                                                 bool final) const {
    json chunk = {
        {"id", id},
        {"object", "chat.completion.chunk"},
        {"created", created},
        {"model", model},
        {"choices", json::array({{
            {"index", 0},
            {"delta", final ? json::object() : json{{"content", content}}},
            {"finish_reason", final ? json("stop") : json(nullptr)}
        }})}
    };
    std::string wire = "data: " + chunk.dump() + "\n\n";
    sink.write(wire.c_str(), wire.size());
}

void NpuGpuPipelineOrchestrator::chat_completion_stream(const json& request,
                                                        const ModelInfo& collection_info,
                                                        httplib::DataSink& sink) {
    try {
        Components components = resolve_components(collection_info);
        auto_load_(components.draft_model);
        auto_load_(components.verifier_model);

        const std::string model = request.value("model", collection_info.model_name);
        const std::string id = new_pipeline_completion_id();
        const long created = static_cast<long>(std::time(nullptr));
        const int max_tokens = resolve_max_tokens(request);
        const int draft_tokens = resolve_draft_tokens(request, collection_info, max_tokens);
        const int verifier_tokens = std::max(1, max_tokens - draft_tokens);

        json draft_request = make_draft_request(request, components.draft_model, draft_tokens);
        json draft_response = router_.chat_completion(draft_request);
        if (draft_response.contains("error")) {
            std::string err = "data: " + draft_response.dump() + "\n\n";
            sink.write(err.c_str(), err.size());
            sink.done();
            return;
        }

        std::string draft_text = extract_message_content(draft_response);
        if (!draft_text.empty()) {
            write_sse_chunk(sink, id, created, model, draft_text, false);
        }

        // First PR implementation uses chunk-level verifier streaming: the NPU
        // prefix reaches the client early, then the verifier continuation is sent
        // as the next chunk. A future refinement can proxy verifier SSE and strip
        // duplicate prefill incrementally.
        json verifier_request = make_verifier_request(request, components.verifier_model,
                                                      draft_text, verifier_tokens, false);
        json verifier_response = router_.chat_completion(verifier_request);
        if (verifier_response.contains("error")) {
            std::string err = "data: " + verifier_response.dump() + "\n\n";
            sink.write(err.c_str(), err.size());
            sink.done();
            return;
        }
        std::string verifier_text = strip_duplicate_prefix(extract_message_content(verifier_response),
                                                           draft_text);
        if (!verifier_text.empty()) {
            write_sse_chunk(sink, id, created, model, verifier_text, false);
        }
        write_sse_chunk(sink, id, created, model, "", true);
        std::string done = "data: [DONE]\n\n";
        sink.write(done.c_str(), done.size());
        sink.done();
    } catch (const std::exception& e) {
        json error = ErrorResponse::create(e.what(), ErrorType::BACKEND_ERROR,
                                           {{"code", "npu_gpu_pipeline_error"}});
        std::string err = "data: " + error.dump() + "\n\n";
        sink.write(err.c_str(), err.size());
        sink.done();
    }
}

} // namespace lemon
