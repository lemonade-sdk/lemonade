#include "lemon/server/shared_args.h"

namespace lemon {

using json = nlohmann::json;

namespace model_loading {
const RouteArg kModel{"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
                      "The model to run; loaded on first use."};
const RouteArg kFormModel{"model", ArgIn::Form, {{"type", "string"}}, true, Support::Available,
                          "The model to run; loaded on first use."};
const RouteArg kCtxSize{"ctx_size", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
                        "Lemonade extension: context size to load the model with, when this request loads it. "
                        "Ignored when the model is already loaded."};
} // namespace model_loading

namespace common {
const RouteArg kStream{"stream", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                       "`true` sends the response as server-sent events as it progresses; `false` sends one "
                       "response once complete. Defaults to `false`."};
const RouteArg kDryRun{"dry_run", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                       "`true` reports what the request would do without doing it. Defaults to `false`."};
} // namespace common

namespace generation {
const RouteArg kTemperature{"temperature", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
                            "Sampling temperature.", {}, true};
const RouteArg kTopK{"top_k", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
                     "Number of top tokens to consider during sampling.", {}, true};
const RouteArg kTopP{"top_p", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
                     "Cumulative probability of the top tokens to consider during nucleus sampling, from "
                     "`0.0` to `1.0`.", {}, true};
const RouteArg kTools{"tools", ArgIn::JsonBody, {{"type", "array"}}, false, Support::Available,
                      "A list of tools the model may call."};
} // namespace generation

namespace openai::text {
const RouteArg kStop{"stop", ArgIn::JsonBody, {{"type", json::array({"string", "array"})}}, false, Support::Available,
                     "Sequences that stop generation, as a string or an array of strings. The returned "
                     "text leaves them out.", {}, true};
const RouteArg kRepeatPenalty{"repeat_penalty", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
                              "From `1.0` to `2.0`: `1.0` applies no penalty, and higher values discourage "
                              "repetition.", {}, true};
const RouteArg kFrequencyPenalty{"frequency_penalty", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
                                 "From `-2.0` to `2.0`: positive values discourage tokens in proportion to how "
                                 "often they have appeared.", {}, true};
const RouteArg kSeed{"seed", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
                     "Random seed for reproducible sampling.", {}, true};
const RouteArg kMaxTokens{"max_tokens", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
                          "Upper bound on generated tokens.", {}, true};
const RouteArg kRouteTrace{"route_trace", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                           "Lemonade extension for `collection.router` models: `true` adds the per-condition "
                           "`trace` to `x_lemonade_route`. Defaults to `false`."};
} // namespace openai::text

namespace openai::image_uploads {
const RouteArg kN{"n", ArgIn::Form, {{"type", "integer"}, {"minimum", 1}, {"maximum", 10}}, false, Support::Available,
                  "Number of images to generate, from `1` to `10`. Defaults to `1`."};
const RouteArg kResponseFormat{"response_format", ArgIn::Form, {{"const", "b64_json"}}, false, Support::Partial,
                               "Only `b64_json`, a base64-encoded image, is supported.", {}, true};
const RouteArg kUser{"user", ArgIn::Form, {{"type", "string"}}, false, Support::NotAvailable,
                     "Accepted for OpenAI compatibility and ignored."};
} // namespace openai::image_uploads

namespace openai::models {
const RouteArg kId{"id", ArgIn::Path, {{"type", "string"}, {"pattern", ".+"}}, true, Support::Available,
                   "Model id, as listed by [`GET /v1/models`](#get-v1models), or an alias."};
} // namespace openai::models

namespace gateway {
const RouteArg kModel{"model", ArgIn::JsonBody, {{"type", "string"}, {"minLength", 1}}, true, Support::Available,
                      "The model to run; loaded on first use."};
} // namespace gateway

namespace ollama {
const RouteArg kStream{"stream", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                       "`true` streams the response as newline-delimited JSON; `false` sends one response "
                       "once complete. Defaults to `true`."};
const RouteArg kKeepAlive{"keep_alive", ArgIn::JsonBody, json::object(), false, Support::Available,
                          "`0` with an empty input unloads the model, answering with `done_reason: "
                          "\"unload\"`. Other values are ignored."};
} // namespace ollama

namespace ollama::embed {
const RouteArg kOptions{"options", ArgIn::JsonBody, {{"type", "object"}}, false, Support::Partial,
                        "`num_ctx` sets the context size when this request loads the model, unless "
                        "`ctx_size` is sent. Other keys are ignored."};
} // namespace ollama::embed

namespace ollama::management {
const RouteArg kModel{"model", ArgIn::JsonBody, {{"type", "string"}, {"minLength", 1}}, true, Support::Available,
                      "The model's name.", {"name"}};
} // namespace ollama::management

namespace backend_routes {
const RouteArg kRecipe{"recipe", ArgIn::JsonBody, {{"type", "string"}, {"minLength", 1}}, false, Support::Available,
                       "Local backends, required: the recipe, such as `llamacpp`, `flm`, `whispercpp`, "
                       "`sd-cpp` or `ryzenai-llm`."};
const RouteArg kBackend{"backend", ArgIn::JsonBody, {{"type", "string"}, {"minLength", 1}}, true, Support::Available,
                        "The backend within the recipe, such as `vulkan`, `rocm`, `cpu` or `default`."};
const RouteArg kAllowInsecureHttp{"allow_insecure_http", ArgIn::JsonBody, {{"type", "boolean"}}, false,
                                  Support::Available,
                                  "Must be `true` to send an API key to a provider whose base URL is "
                                  "`http://`; the opt-in is saved with the provider. Defaults to `false`."};
} // namespace backend_routes

namespace downloads {
const RouteArg kSubscribe{"subscribe", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                          "Only applies when `stream` is `true`. If `false`, the server owns the download and "
                          "answers at once with a snapshot of its job, which "
                          "[`GET /v1/downloads`](#get-v1downloads) and "
                          "[`POST /v1/downloads/control`](#post-v1downloadscontrol) then report and control. "
                          "With `true`, closing the stream cancels the download. Defaults to `true`."};
} // namespace downloads

namespace model_management {
const RouteArg kModelName{"model_name", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
                          "[Lemonade Server model name](https://lemonade-server.ai/models.html).", {"model"}};
} // namespace model_management

namespace model_options {
const RouteArg kCtxSize{"ctx_size", ArgIn::JsonBody, {{"type", json::array({"integer", "null"})}}, false, Support::Available,
                        "Context size: a positive whole number, or `-1` to size it automatically even when the "
                        "server-wide `ctx_size` is a specific number. Only recipes that take a context size use "
                        "it, such as `llamacpp`, `flm`, `ryzenai-llm` and `vllm`."};
} // namespace model_options

namespace job_routes {
const RouteArg kId{"id", ArgIn::Path, {{"type", "string"}}, true, Support::Available,
                   "Job id, from [`POST /v1/jobs`](#post-v1jobs)."};
} // namespace job_routes

namespace inference {
const RouteArg kSteps{"steps", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
                      "Number of inference steps; lower is faster, higher can improve quality. Defaults to "
                      "the model's own setting, else the backend's."};
const RouteArg kSeed{"seed", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
                     "Random seed for reproducibility. Defaults to the backend's own."};
} // namespace inference

} // namespace lemon
