#pragma once

#include "lemon/model_types.h"
#include "lemon/routing_policy.h"

#include <functional>
#include <map>
#include <optional>
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

// Resolve CostInfo from optional typed per-million fields plus recognized
// extras keys (cost_tier, cost_*_per_million, latency_ms_hint). Typed values
// win when present; extras fill gaps. cost_tier must be one of
// free|low|medium|high — other values are dropped. Used by the Router
// CostServices wiring and unit tests — keeps ModelInfo out of this header's
// include graph.
CostInfo resolve_cost_info(std::optional<double> cost_input_per_million,
                           std::optional<double> cost_output_per_million,
                           const std::map<std::string, json>& extras);

CostServices make_router_cost_services(Router& router);

std::vector<float> parse_embedding_vector(const json& response);
std::map<std::string, double> parse_classifier_scores(const json& response);
std::string extract_chat_text(const json& response);

// Translate an inbound chat/completions, completions, or responses body into a
// backend-agnostic RouteContext the routing engine can evaluate.
RouteContext build_route_context(const json& request_json, const std::string& model_name);

} // namespace lemon
