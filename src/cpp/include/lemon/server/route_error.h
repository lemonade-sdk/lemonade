#pragma once

#include <string>

namespace lemon {

struct RouteError {
    int status;                       // e.g. 404
    std::string code;                 // the code the body carries, e.g. "model_not_found"; empty when it carries none
    std::string when;                 // the condition, e.g. "`id` names no job"
};

} // namespace lemon
