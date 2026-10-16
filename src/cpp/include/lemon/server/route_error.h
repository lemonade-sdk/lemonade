#pragma once

#include <string>

namespace lemon {

// Declared once, so the status a route sends and the status its docs list cannot disagree.
struct RouteError {
    int status;                       // e.g. 404
    std::string code;                 // the code the body carries, e.g. "model_not_found"; empty when it carries none
    std::string when;                 // the condition, e.g. "`id` names no job"
};

} // namespace lemon
