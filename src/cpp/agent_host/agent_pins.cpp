#include "lemon/agents/agent_pins.h"

#include <fstream>

#include "lemon/utils/path_utils.h"

namespace lemon::agents {

namespace {

std::string pi_asset(const std::string& os, const std::string& arch) {
    if ((os != "linux" && os != "darwin" && os != "windows") ||
        (arch != "x64" && arch != "arm64")) {
        return "";
    }
    return "pi-" + os + "-" + arch + (os == "windows" ? ".zip" : ".tar.gz");
}

} // namespace

const std::vector<AgentDescriptor>& agent_descriptors() {
    static const std::vector<AgentDescriptor> agents = {
        {"pi", "Pi", "earendil-works/pi", "pi", pi_asset},
    };
    return agents;
}

const AgentDescriptor& find_agent(const std::string& id) {
    for (const auto& agent : agent_descriptors()) {
        if (agent.id == id) {
            return agent;
        }
    }
    throw AgentError(404, "unknown_agent", "Lemonade doesn't manage an agent named '" + id + "'.");
}

std::string host_os() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#else
    return "";
#endif
}

std::string host_arch() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#else
    return "";
#endif
}

nlohmann::json load_agent_versions() {
    std::string path = lemon::utils::get_environment_variable_utf8("LEMONADE_AGENT_VERSIONS");
    if (path.empty()) {
        path = lemon::utils::get_resource_path("resources/agent_versions.json");
    }
    std::ifstream in(lemon::utils::path_from_utf8(path));
    nlohmann::json versions = nlohmann::json::parse(in, nullptr, false);
    if (!versions.is_object()) {
        throw AgentError(500, "pin_invalid", "Can't read agent versions from " + path + ".");
    }
    return versions;
}

AgentPin resolve_pin(const AgentDescriptor& agent, const nlohmann::json& versions,
                     const std::string& os, const std::string& arch,
                     const std::string& base_url) {
    AgentPin pin;
    pin.asset = agent.asset(os, arch);
    if (pin.asset.empty()) {
        throw AgentError(422, "unsupported_platform",
                         agent.name + " has no release for " + os + "-" + arch + ".");
    }

    const auto entry = versions.find(agent.id);
    if (entry == versions.end() || !entry->is_object() || !entry->contains("version") ||
        !(*entry)["version"].is_string()) {
        throw AgentError(500, "pin_invalid", "No pinned version for " + agent.name + ".");
    }
    pin.tag = (*entry)["version"].get<std::string>();
    pin.version = !pin.tag.empty() && pin.tag[0] == 'v' ? pin.tag.substr(1) : pin.tag;

    const nlohmann::json* checksum = &versions;
    for (const std::string& key : {std::string("checksums"), std::string("github"), agent.repo,
                                   pin.tag, pin.asset}) {
        if (!checksum->is_object() || !checksum->contains(key)) {
            checksum = nullptr;
            break;
        }
        checksum = &(*checksum)[key];
    }
    // A pin without a checksum is an error: payloads are only installed verified.
    if (checksum == nullptr || !checksum->is_string() ||
        checksum->get<std::string>().rfind("sha256:", 0) != 0) {
        throw AgentError(500, "pin_invalid",
                         "No sha256 checksum is pinned for " + pin.asset + " " + pin.tag + ".");
    }
    pin.sha256 = checksum->get<std::string>();

    const std::string base =
        base_url.empty() ? "https://github.com/" + agent.repo + "/releases/download" : base_url;
    pin.url = base + "/" + pin.tag + "/" + pin.asset;
    return pin;
}

} // namespace lemon::agents
