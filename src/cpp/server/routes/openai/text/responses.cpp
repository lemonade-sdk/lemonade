#include <lemon/utils/aixlog.hpp>

#include <algorithm>

#include "lemon/model_manager.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_loader.h"
#include "lemon/server/router_dispatch.h"
#include "lemon/server/shared_args.h"
#include "lemon/streaming_proxy.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kNoResponsesApi{
    400, "",
    "The request does not stream and `model` is a FastFlowLM or cloud model, which has no "
    "Responses API"};
const RouteError kNotLlm{
    400, "model_not_applicable", "The request does not stream and `model` is not an LLM"};
const RouteError kRouterResponseChain{
    400, "router_response_chain_unsupported",
    "The request continues earlier responses through `previous_response_id`, `conversation` or an "
    "`item_reference` input, and `model` is a `collection.router` model whose policy can pick a "
    "different candidate than the one holding that state"};

ordered_json response_schema() {
    auto schema = ordered_json::parse(R"({
        "type": "object",
        "required": ["id", "object", "created_at", "model", "output"],
        "properties": {
            "id": {"type": "string", "description": "Identifier of the response."},
            "object": {"const": "response", "description": "Always `\"response\"`."},
            "created_at": {"type": "number", "description": "Unix timestamp of when the response was created."},
            "completed_at": {"type": "number", "description": "Unix timestamp of when the response completed, when the backend reports it."},
            "model": {"type": "string", "description": "The model that generated the response."},
            "status": {"type": "string", "description": "State of the response, such as `completed`."},
            "output": {"type": "array", "items": {"type": "object"},
                       "description": "Output items; a message item carries its text in `content[].text`."},
            "usage": {
                "type": "object",
                "description": "Token counts for the request.",
                "properties": {
                    "input_tokens": {"type": "integer", "description": "Tokens in the input."},
                    "output_tokens": {"type": "integer", "description": "Tokens generated."},
                    "total_tokens": {"type": "integer", "description": "Sum of `input_tokens` and `output_tokens`."},
                    "input_tokens_details": {"type": "object", "description": "Breakdown of the input tokens, such as `cached_tokens`, when the backend reports it."}
                }
            }
        }
    })");
    schema["properties"]["x_lemonade_route"] = {
        {"type", "object"},
        {"description",
         "The routing decision, when `model` names a `collection.router` model. See "
         "[Routing](./router.md#routing-collectionrouter) for the decision object."},
    };
    return schema;
}

ordered_json response_event_schema() {
    auto schema = ordered_json::parse(R"({
        "type": "object",
        "title": "Response event",
        "description": "An event of the response, passed through from the backend.",
        "required": ["type"],
        "properties": {
            "type": {"type": "string", "description": "Event type, passed through from the backend, such as `response.created`, `response.output_text.delta` or `response.completed`; see OpenAI's [streaming reference](https://platform.openai.com/docs/api-reference/responses-streaming)."},
            "response": {"type": "object", "description": "Lifecycle events, such as `response.created`, `response.in_progress` and `response.completed`, only: the response so far."},
            "item": {"type": "object", "description": "Output item events, such as `response.output_item.added`, only: the output item."},
            "output_index": {"type": "integer", "description": "Zero-based index of the output item in the response's `output` array, on output-item and delta events."},
            "item_id": {"type": "string", "description": "Delta events only: the `id` of the output item the text belongs to."},
            "delta": {"type": "string", "description": "Delta events, such as `response.output_text.delta` and `response.reasoning_text.delta`, only: the next text."},
            "timings": {"type": "object", "description": "`response.completed` only: prompt processing and generation timings, passed through from backends that report them, such as llama.cpp."}
        }
    })");
    schema["properties"]["x_lemonade_route"] = {
        {"type", "object"},
        {"description",
         "The routing decision, when `model` names a `collection.router` model. See "
         "[Routing](./router.md#routing-collectionrouter) for the decision object."},
    };
    return schema;
}

const RouteArg kInput{"input", ArgIn::JsonBody, {{"type", json::array({"string", "array"})}}, true, Support::Available,
                      "The input, as a string or an array of input items."};
const RouteArg kMaxOutputTokens{"max_output_tokens", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
                                "Upper bound on generated tokens."};
const RouteArg kPreviousResponseId{
    "previous_response_id", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Partial,
    "The response this one continues. llama.cpp rejects it."};

bool non_empty(const json& value) {
    if (value.is_string()) return !value.get_ref<const std::string&>().empty();
    if (value.is_object()) return !value.empty();
    return false;
}

// Earlier responses live in the backend that created them, so a request that
// refers to them can only be answered by that backend.
bool references_backend_state(const json& body) {
    const auto previous = body.find(kPreviousResponseId.name);
    if (previous != body.end() && non_empty(*previous)) return true;
    const auto conversation = body.find("conversation");
    if (conversation != body.end() && non_empty(*conversation)) return true;
    const auto input = body.find(kInput.name);
    if (input == body.end() || !input->is_array()) return false;
    return std::any_of(input->begin(), input->end(), [](const json& item) {
        return item.is_object() && item.value("type", std::string()) == "item_reference";
    });
}

class ResponsesRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.responses";
        s.methods = {"POST"};
        s.paths = {"responses"};
        s.summary = "Generate a response in OpenAI's Responses format";
        s.modality = "prompt/messages -> event";
        s.description =
            "Generates a model response to an input, in OpenAI's Responses format, downloading and "
            "loading the model if it is not loaded.";
        s.notes = {
            "Naming a `collection.router` model routes the request to one of its candidates; "
            "see [Router API](./router.md).",
            "A request that continues earlier responses, through `previous_response_id`, "
            "`conversation` or an `item_reference` input, is rejected on a `collection.router` "
            "model, since the candidate picked for this turn may not be the one holding them. "
            "Resend the full `input`, or send the request to the `model` the earlier response "
            "reports.",
        };
        s.args = {
            kInput, model_loading::kModel, kMaxOutputTokens, kPreviousResponseId, generation::kTemperature,
            openai::text::kRepeatPenalty, generation::kTopK, generation::kTopP, common::kStream,
            openai::text::kRouteTrace, model_loading::kCtxSize
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = response_schema();

        RouteResponse stream;
        stream.format = ResponseFormat::EventStream;
        stream.schema = {{"description", "One event per `data:` line. The stream ends with `data: [DONE]`."}, {"oneOf", ordered_json::array({
            response_event_schema(), StreamingProxy::error_event_schema()})}};

        s.responses = {response, stream};

        RouteExample response_example;
        response_example.name = "non-streaming";
        response_example.format = ResponseFormat::Json;
        response_example.args = ordered_json::parse(R"({
            "model": "Llama-3.2-1B-Instruct-GGUF",
            "input": "What is the population of Paris?",
            "stream": false
        })");

        RouteExample stream_example;
        stream_example.name = "streaming";
        stream_example.format = ResponseFormat::EventStream;
        stream_example.args = ordered_json::parse(R"({
            "model": "Llama-3.2-1B-Instruct-GGUF",
            "input": "What is the population of Paris?",
            "stream": true
        })");

        s.examples = {response_example, stream_example};
        s.errors = join_errors({{kBodyNotJson, kArgMissing, kArgInvalid, kNoResponsesApi, kNotLlm,
                                 kRouterResponseChain,
                                 kBackendError, kUnexpectedException},
                                ModelLoader::errors()});
        s.error_format = ErrorFormat::OpenAi;
        s.request_format = RequestFormat::Json;
        return s;
    }

protected:
    const RouteArg& model_arg() const override { return model_loading::kModel; }

    bool validate(RouteRequest& req, httplib::Response& res) override {
        // A failed registry lookup cannot tell the model is a collection, so the
        // normal path reports what is wrong with the model instead.
        std::optional<ModelInfo> collection;
        try {
            collection = router_collection_info(ctx_, req.model);
        } catch (const std::exception&) {
        }
        if (collection && collection->route_policy && references_backend_state(req.body)) {
            LOG(INFO, "Server") << "Rejected /responses continuing earlier responses on router "
                                << "collection '" << req.model << "'" << std::endl;
            write_openai_error(
                res, kRouterResponseChain,
                "This request continues earlier responses, which are held by the backend that "
                "served them, and router collection '" + req.model + "' may select a different "
                "candidate for this turn. Resend the full 'input', or send the request to the "
                "'model' reported by the earlier response.");
            return false;
        }
        // Router policies call backends here, before run() does.
        auto cancel = cancel_on_disconnect(req);
        req.route_decision = router_dispatch(
            ctx_, req.model, req.value(openai::text::kRouteTrace, false), req.body, collection);
        return true;
    }

    void run(RouteRequest& req, httplib::Response& res) override {
        bool is_streaming = req.value(common::kStream, false);
        if (is_streaming) {
            LOG(INFO, "Server") << "POST /api/v1/responses - Streaming" << std::endl;
            Router* router = ctx_.router;
            stream_response(req, res, [router](const std::string& body, httplib::DataSink& sink) {
                router->responses_stream(body, sink);
            });
            return;
        }

        LOG(INFO, "Server") << "POST /api/v1/responses - Non-streaming" << std::endl;
        auto cancel = cancel_on_disconnect(req);
        auto response = ctx_.router->responses(req.body);
        // A Responses object carries "error": null when it succeeded.
        if (response.contains("error") && !response["error"].is_null()) {
            LOG(ERROR, "Server") << "Responses backend error: " << response["error"].dump() << std::endl;
            set_error_response(response, res, kBackendError);
            return;
        }

        attach_route_decision(response, res, req.route_decision);
        res.set_content(response.dump(), "application/json");
        record_usage(response, req);
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_responses_route(ServerContext& ctx) {
    return std::make_unique<ResponsesRoute>(ctx);
}

} // namespace lemon
