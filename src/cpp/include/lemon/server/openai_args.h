#pragma once

#include "lemon/server/route_arg.h"

// The arguments of the OpenAI routes that other code builds requests for, such as the Ollama
// and Anthropic gateways. Each route file defines its own.
namespace lemon::openai {

// chat/completions and completions take these alike.
namespace text {
extern const RouteArg kModel;
extern const RouteArg kStream;
extern const RouteArg kStop;
extern const RouteArg kTemperature;
extern const RouteArg kRepeatPenalty;
extern const RouteArg kFrequencyPenalty;
extern const RouteArg kSeed;
extern const RouteArg kTopK;
extern const RouteArg kTopP;
extern const RouteArg kRouteTrace;
} // namespace text

namespace chat {
extern const RouteArg kMessages;
extern const RouteArg kLogprobs;
extern const RouteArg kTools;
extern const RouteArg kToolChoice;
extern const RouteArg kResponseFormat;
extern const RouteArg kMaxTokens;
extern const RouteArg kMaxCompletionTokens;
extern const RouteArg kEnableThinking;
} // namespace chat

namespace completions {
extern const RouteArg kPrompt;
extern const RouteArg kEcho;
extern const RouteArg kLogprobs;
extern const RouteArg kMaxTokens;
} // namespace completions

namespace embeddings {
extern const RouteArg kInput;
extern const RouteArg kModel;
extern const RouteArg kEncodingFormat;
} // namespace embeddings

// images/generations
namespace images {
extern const RouteArg kPrompt;
extern const RouteArg kModel;
extern const RouteArg kSize;
extern const RouteArg kN;
extern const RouteArg kResponseFormat;
extern const RouteArg kSteps;
extern const RouteArg kCfgScale;
extern const RouteArg kSeed;
} // namespace images

} // namespace lemon::openai
