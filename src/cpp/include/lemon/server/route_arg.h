#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lemon {

enum class ArgIn { JsonBody, Query, Path, Form };  // where an argument comes from

// How much of an argument Lemonade honors; the docs give each argument a badge for it.
enum class Support { Available, Partial, NotAvailable };

struct RouteArg {
    std::string name;
    ArgIn in;
    nlohmann::json schema;            // JSON-schema subset
    bool required = false;
    Support supported = Support::Available;  // Partial: the description states the limit
    std::string description;
    std::vector<std::string> aliases;  // other names clients send it under, e.g. "model" for "model_name"
    bool forwarded = false;            // lemond passes it to the backend without acting on it
};

} // namespace lemon
