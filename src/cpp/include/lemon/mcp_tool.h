#pragma once

#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace lemon {

// One declarative MCP tool contract. The descriptor is the single source of
// truth consumed by tools/list and generated documentation; the handler stored
// beside it is the implementation used by tools/call.
class McpTool {
public:
    using Handler = std::function<nlohmann::json(const nlohmann::json&)>;

    McpTool(nlohmann::json descriptor, Handler handler)
        : descriptor_(std::move(descriptor)), handler_(std::move(handler)) {
        if (!descriptor_.is_object()) {
            throw std::invalid_argument("MCP tool descriptor must be an object");
        }
        if (!descriptor_.contains("name") || !descriptor_["name"].is_string() ||
            descriptor_["name"].get<std::string>().empty()) {
            throw std::invalid_argument("MCP tool descriptor requires a non-empty string name");
        }
        if (!descriptor_.contains("description") || !descriptor_["description"].is_string() ||
            descriptor_["description"].get<std::string>().empty()) {
            throw std::invalid_argument(
                "MCP tool descriptor requires a non-empty string description");
        }
        if (!descriptor_.contains("inputSchema") || !descriptor_["inputSchema"].is_object() ||
            descriptor_["inputSchema"].value("type", std::string()) != "object") {
            throw std::invalid_argument(
                "MCP tool descriptor requires an object inputSchema with type=object");
        }
        if (descriptor_.contains("annotations") && !descriptor_["annotations"].is_object()) {
            throw std::invalid_argument("MCP tool annotations must be an object");
        }
        if (!handler_) {
            throw std::invalid_argument("MCP tool requires a handler");
        }
    }

    const nlohmann::json& descriptor() const noexcept { return descriptor_; }

    std::string name() const {
        return descriptor_["name"].get<std::string>();
    }

    nlohmann::json call(const nlohmann::json& arguments) const {
        try {
            validate_value(arguments, descriptor_["inputSchema"], "$");
        } catch (const std::invalid_argument& e) {
            throw std::invalid_argument(
                "Invalid arguments for MCP tool '" + name() + "': " + e.what());
        }
        return handler_(arguments);
    }

private:
    // Validate the JSON-Schema subset used by the current MCP descriptors.
    // This keeps the advertised contract on tools/list on the actual call path.
    static bool is_json_integer(const nlohmann::json& value) {
        if (value.is_number_integer() || value.is_number_unsigned()) {
            return true;
        }
        if (!value.is_number_float()) {
            return false;
        }
        const double number = value.get<double>();
        return std::isfinite(number) && std::floor(number) == number;
    }

    static bool matches_type(const nlohmann::json& value, const std::string& type) {
        if (type == "object") return value.is_object();
        if (type == "array") return value.is_array();
        if (type == "string") return value.is_string();
        if (type == "boolean") return value.is_boolean();
        if (type == "number") return value.is_number();
        if (type == "integer") return is_json_integer(value);
        if (type == "null") return value.is_null();
        throw std::invalid_argument("unsupported schema type '" + type + "'");
    }

    static void validate_value(const nlohmann::json& value,
                               const nlohmann::json& schema,
                               const std::string& path) {
        if (!schema.is_object()) {
            throw std::invalid_argument(path + ": schema must be an object");
        }

        const auto type_it = schema.find("type");
        if (type_it != schema.end()) {
            if (!type_it->is_string()) {
                throw std::invalid_argument(path + ": schema type must be a string");
            }
            const std::string expected = type_it->get<std::string>();
            if (!matches_type(value, expected)) {
                throw std::invalid_argument(
                    path + ": expected " + expected + ", got " + value.type_name());
            }
        }

        const auto enum_it = schema.find("enum");
        if (enum_it != schema.end()) {
            if (!enum_it->is_array()) {
                throw std::invalid_argument(path + ": schema enum must be an array");
            }
            bool matched = false;
            for (const auto& candidate : *enum_it) {
                if (value == candidate) {
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                throw std::invalid_argument(
                    path + ": value is not one of " + enum_it->dump());
            }
        }

        const auto minimum_it = schema.find("minimum");
        if (minimum_it != schema.end()) {
            if (!minimum_it->is_number()) {
                throw std::invalid_argument(path + ": schema minimum must be numeric");
            }
            if (value.is_number() &&
                value.get<double>() < minimum_it->get<double>()) {
                throw std::invalid_argument(
                    path + ": value must be >= " + minimum_it->dump());
            }
        }

        if (value.is_object()) {
            const auto required_it = schema.find("required");
            if (required_it != schema.end()) {
                if (!required_it->is_array()) {
                    throw std::invalid_argument(path + ": schema required must be an array");
                }
                for (const auto& required : *required_it) {
                    if (!required.is_string()) {
                        throw std::invalid_argument(
                            path + ": schema required entries must be strings");
                    }
                    const std::string key = required.get<std::string>();
                    if (!value.contains(key)) {
                        throw std::invalid_argument(
                            path + ": missing required property '" + key + "'");
                    }
                }
            }

            const auto properties_it = schema.find("properties");
            if (properties_it != schema.end()) {
                if (!properties_it->is_object()) {
                    throw std::invalid_argument(path + ": schema properties must be an object");
                }
                for (const auto& item : properties_it->items()) {
                    if (!value.contains(item.key())) continue;
                    const auto& property_value = value.at(item.key());
                    if (property_value.is_null()) {
                        bool is_required = false;
                        if (required_it != schema.end()) {
                            for (const auto& required : *required_it) {
                                if (required.is_string() &&
                                    required.get<std::string>() == item.key()) {
                                    is_required = true;
                                    break;
                                }
                            }
                        }
                        if (!is_required) continue;
                    }
                    validate_value(
                        property_value, item.value(), path + "." + item.key());
                }
            }
        }

        if (value.is_array()) {
            const auto items_it = schema.find("items");
            if (items_it != schema.end()) {
                if (!items_it->is_object()) {
                    throw std::invalid_argument(path + ": schema items must be an object");
                }
                for (std::size_t i = 0; i < value.size(); ++i) {
                    validate_value(value[i], *items_it,
                                   path + "[" + std::to_string(i) + "]");
                }
            }
        }
    }

    nlohmann::json descriptor_;
    Handler handler_;
};

}  // namespace lemon
