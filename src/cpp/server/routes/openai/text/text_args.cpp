#include "lemon/server/openai_args.h"

namespace lemon::openai::text {

using json = nlohmann::json;

const RouteArg kModel{"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
                      "The model to run."};
const RouteArg kStream{"stream", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                       "`true` sends tokens as server-sent events as they are generated; `false` sends one "
                       "response once complete. Defaults to `false`."};
const RouteArg kStop{"stop", ArgIn::JsonBody, {{"type", json::array({"string", "array"})}}, false, Support::Available,
                     "Sequences that stop generation, as a string or an array of strings. The returned "
                     "text leaves them out.", {}, true};
const RouteArg kTemperature{"temperature", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
                            "Sampling temperature.", {}, true};
const RouteArg kRepeatPenalty{"repeat_penalty", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
                              "From `1.0` to `2.0`: `1.0` applies no penalty, and higher values discourage "
                              "repetition.", {}, true};
const RouteArg kFrequencyPenalty{"frequency_penalty", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
                                 "From `-2.0` to `2.0`: positive values discourage tokens in proportion to how "
                                 "often they have appeared.", {}, true};
const RouteArg kSeed{"seed", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
                     "Random seed for reproducible sampling.", {}, true};
const RouteArg kTopK{"top_k", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
                     "Number of top tokens to consider during sampling.", {}, true};
const RouteArg kTopP{"top_p", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
                     "Cumulative probability of the top tokens to consider during nucleus sampling, from "
                     "`0.0` to `1.0`.", {}, true};
const RouteArg kRouteTrace{"route_trace", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                           "Lemonade extension for `collection.router` models: `true` adds the per-condition "
                           "`trace` to `x_lemonade_route`. Defaults to `false`."};

} // namespace lemon::openai::text
