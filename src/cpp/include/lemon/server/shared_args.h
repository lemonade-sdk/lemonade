#pragma once

#include "lemon/server/route_arg.h"

// Arguments that several routes take alike. Each is one constant that every such route lists.
namespace lemon {

namespace model_loading {
extern const RouteArg kModel;
extern const RouteArg kFormModel;
extern const RouteArg kCtxSize;
} // namespace model_loading

namespace common {
extern const RouteArg kStream;
extern const RouteArg kDryRun;
} // namespace common

namespace generation {
extern const RouteArg kTemperature;
extern const RouteArg kTopK;
extern const RouteArg kTopP;
extern const RouteArg kTools;
} // namespace generation

namespace openai::text {
extern const RouteArg kStop;
extern const RouteArg kRepeatPenalty;
extern const RouteArg kFrequencyPenalty;
extern const RouteArg kSeed;
extern const RouteArg kMaxTokens;
extern const RouteArg kRouteTrace;
} // namespace openai::text

namespace openai::image_uploads {
extern const RouteArg kN;
extern const RouteArg kResponseFormat;
extern const RouteArg kUser;
} // namespace openai::image_uploads

namespace openai::models {
extern const RouteArg kId;
} // namespace openai::models

namespace gateway {
extern const RouteArg kModel;
} // namespace gateway

namespace ollama {
extern const RouteArg kStream;
extern const RouteArg kKeepAlive;
} // namespace ollama

namespace ollama::embed {
extern const RouteArg kOptions;
} // namespace ollama::embed

namespace ollama::management {
extern const RouteArg kModel;
} // namespace ollama::management

namespace backend_routes {
extern const RouteArg kRecipe;
extern const RouteArg kBackend;
extern const RouteArg kAllowInsecureHttp;
} // namespace backend_routes

namespace downloads {
extern const RouteArg kSubscribe;
} // namespace downloads

namespace model_management {
extern const RouteArg kModelName;
} // namespace model_management

namespace model_options {
extern const RouteArg kCtxSize;
} // namespace model_options

namespace job_routes {
extern const RouteArg kId;
} // namespace job_routes

namespace inference {
extern const RouteArg kSteps;
extern const RouteArg kSeed;
} // namespace inference

} // namespace lemon
