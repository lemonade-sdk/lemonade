#pragma once

#include "lemon/routing_policy.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace lemon {

class Router;

using EnsureClassifierModelLoaded = std::function<void(const std::string& model)>;
using RouterJsonCall = std::function<json(const json& request)>;

ClassifierServices make_router_classifier_services(
    Router& router,
    EnsureClassifierModelLoaded ensure_loaded = {});

// Testable adapter seam: production binds these calls to Router::embeddings and
// Router::chat_completion; unit tests bind them to fake Router-like functions.
ClassifierServices make_classifier_services_from_router_calls(
    RouterJsonCall embeddings,
    RouterJsonCall chat_completion,
    EnsureClassifierModelLoaded ensure_loaded = {});

std::vector<float> parse_embedding_vector(const json& response);
std::map<std::string, double> parse_classifier_scores(const json& response);
std::string extract_chat_text(const json& response);

// Translate an inbound chat/completions, completions, or responses body into a
// backend-agnostic RouteContext the routing engine can evaluate.
RouteContext build_route_context(const json& request_json, const std::string& model_name);

} // namespace lemon
