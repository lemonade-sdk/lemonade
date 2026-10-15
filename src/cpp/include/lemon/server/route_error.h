#pragma once

#include <string>

namespace lemon {

// Clients branch on a route's error statuses, so each one is declared once, and both the code
// that writes it and the docs read it from the declaration.
struct RouteError {
    int status;                       // e.g. 404
    std::string code;                 // the code the body carries, e.g. "model_not_found"; empty when it carries none
    std::string when;                 // the condition, e.g. "`id` names no job"
};

} // namespace lemon
