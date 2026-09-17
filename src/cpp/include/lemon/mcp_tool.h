#pragma once

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
        return handler_(arguments);
    }

private:
    nlohmann::json descriptor_;
    Handler handler_;
};

}  // namespace lemon
