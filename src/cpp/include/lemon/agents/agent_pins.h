#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lemon::agents {

// An error a host route answers with: the HTTP status and the body's error code.
struct AgentError : std::runtime_error {
    AgentError(int status, std::string code, const std::string& message,
               nlohmann::json extra = nlohmann::json::object())
        : std::runtime_error(message), status(status), code(std::move(code)),
          extra(std::move(extra)) {}
    int status;
    std::string code;
    nlohmann::json extra;
};

struct AgentDescriptor {
    std::string id;
    std::string name;
    std::string repo;    // GitHub owner/name that publishes the releases
    std::string binary;  // executable name inside the payload, without .exe
    // Release asset for an OS (linux, darwin, windows) and arch (x64, arm64).
    std::string (*asset)(const std::string& os, const std::string& arch);
};

const std::vector<AgentDescriptor>& agent_descriptors();
const AgentDescriptor& find_agent(const std::string& id);  // throws 404 unknown_agent

std::string host_os();
std::string host_arch();

struct AgentPin {
    std::string tag;      // as released, e.g. v1.5.0
    std::string version;  // tag without the leading v
    std::string asset;
    std::string sha256;   // sha256:<hex>
    std::string url;
};

// agent_versions.json from the resources, or LEMONADE_AGENT_VERSIONS in tests.
nlohmann::json load_agent_versions();

// Throws 422 unsupported_platform, or 500 pin_invalid when the version or its
// checksum is missing. base_url replaces https://github.com/<repo>/releases/download.
AgentPin resolve_pin(const AgentDescriptor& agent, const nlohmann::json& versions,
                     const std::string& os, const std::string& arch,
                     const std::string& base_url = "");

} // namespace lemon::agents
