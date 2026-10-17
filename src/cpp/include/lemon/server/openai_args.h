#pragma once

#include "lemon/server/route_arg.h"

// The arguments of the OpenAI routes that other code builds requests for, such as the Ollama
// and Anthropic gateways. Each route file defines its own; the arguments several routes
// take alike are in shared_args.h.
namespace lemon::openai {

namespace chat {
extern const RouteArg kMessages;
extern const RouteArg kLogprobs;
extern const RouteArg kToolChoice;
extern const RouteArg kResponseFormat;
extern const RouteArg kMaxCompletionTokens;
extern const RouteArg kEnableThinking;
} // namespace chat

namespace completions {
extern const RouteArg kPrompt;
extern const RouteArg kEcho;
extern const RouteArg kLogprobs;
} // namespace completions

namespace embeddings {
extern const RouteArg kInput;
extern const RouteArg kEncodingFormat;
} // namespace embeddings

// images/generations
namespace images {
extern const RouteArg kPrompt;
extern const RouteArg kSize;
extern const RouteArg kN;
extern const RouteArg kResponseFormat;
extern const RouteArg kCfgScale;
extern const RouteArg kSeed;
} // namespace images

} // namespace lemon::openai
