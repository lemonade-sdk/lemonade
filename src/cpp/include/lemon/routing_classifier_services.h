#pragma once

#include "lemon/model_types.h"
#include "lemon/routing_policy.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace lemon {

class Router;

using EnsureClassifierModelLoaded = std::function<void(const std::string& model)>;
using RouterJsonCall = std::function<json(const json& request)>;
using RouterModelTypeCall = std::function<ModelType(const std::string& model)>;

ClassifierServices make_router_classifier_services(
    Router& router,
    EnsureClassifierModelLoaded ensure_loaded = {});

// Testable adapter seam: production binds these calls to Router::embeddings and
// Router::chat_completion (plus Router::classify/get_model_type for models
// tagged ModelType::CLASSIFICATION); unit tests bind them to fake Router-like
// functions. `classify`/`get_model_type` default to empty so existing
// embeddings+chat_completion-only call sites keep compiling unchanged — a
// `run_classifier` call against a model with no `get_model_type` configured,
// or one that isn't ModelType::CLASSIFICATION, falls back to the original
// chat-completion-based classification.
ClassifierServices make_classifier_services_from_router_calls(
    RouterJsonCall embeddings,
    RouterJsonCall chat_completion,
    EnsureClassifierModelLoaded ensure_loaded = {},
    RouterJsonCall classify = {},
    RouterModelTypeCall get_model_type = {});

std::vector<float> parse_embedding_vector(const json& response);
std::map<std::string, double> parse_classifier_scores(const json& response);
std::string extract_chat_text(const json& response);

// Translate an inbound chat/completions, completions, or responses body into a
// backend-agnostic RouteContext the routing engine can evaluate.
RouteContext build_route_context(const json& request_json, const std::string& model_name);

} // namespace lemon
