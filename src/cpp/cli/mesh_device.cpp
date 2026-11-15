#include "lemon_cli/mesh_device.h"

#include <algorithm>
#include <cctype>

namespace lemon_cli {

namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string url_label(const std::string& url) {
    auto host = url;
    if (const auto scheme = host.find("://"); scheme != std::string::npos) host.erase(0, scheme + 3);
    return host.substr(0, host.find_first_of(".:/"));
}

std::string text(const nlohmann::json& value, const char* key) {
    const auto found = value.find(key);
    return found != value.end() && found->is_string() ? found->get<std::string>() : std::string();
}

}  // namespace

std::vector<MeshDevice> mesh_devices(const nlohmann::json& status) {
    std::vector<MeshDevice> devices;
    if (!status.is_object()) return devices;
    const auto port = status.find("mesh_port");
    if (port == status.end() || !port->is_object() || !port->value("running", false)) return devices;
    const auto urls = port->find("urls");
    if (urls == port->end() || !urls->is_array()) return devices;
    for (const auto& entry : *urls) {
        if (!entry.is_object() || text(entry, "url").empty()) continue;
        devices.push_back({text(entry, "node_id"), text(entry, "device_name"), text(entry, "url"), entry.value("self", false)});
    }
    return devices;
}

bool resolve_mesh_device(const nlohmann::json& status, const std::string& wanted, MeshDevice& device,
                         std::string& error) {
    const auto name = lower(wanted);
    if (!status.is_object() || !status.value("enabled", false)) {
        error = "The mesh is off on this server. Turn on Lemonade Nexus there first.";
        return false;
    }
    const auto devices = mesh_devices(status);
    if (devices.empty()) {
        error = "The mesh port is not running on this server, so no device can be reached through it.";
        return false;
    }
    const auto& port = status.at("mesh_port");
    if (text(port, "local_domain").empty()) {
        error = "This server's mesh port has no group domain yet, so it cannot name other devices";
        const auto tls = port.find("tls");
        const auto reason = tls != port.end() && tls->is_object() ? text(*tls, "error") : std::string();
        error += reason.empty() ? "." : " (" + reason + ").";
        error += " Its selection (POST /v1/nexus/selection) is the only way to reach one through it.";
        return false;
    }
    for (const auto& candidate : devices) {
        if (lower(candidate.device_name) == name || lower(candidate.node_id) == name || url_label(candidate.url) == name) {
            device = candidate;
            return true;
        }
    }
    std::string available;
    for (const auto& candidate : devices) {
        if (candidate.self) continue;
        available += (available.empty() ? "" : ", ") + candidate.device_name + " (" + candidate.node_id + ")";
    }
    const auto listed = available.empty() ? std::string("No other device is online and reachable through the mesh right now.")
                                          : "Devices reachable through the mesh: " + available + ".";
    const auto members = status.find("members");
    if (members != status.end() && members->is_array()) {
        for (const auto& member : *members) {
            if (!member.is_object()) continue;
            if (lower(text(member, "device_name")) == name || lower(text(member, "node_id")) == name) {
                error = "The device " + text(member, "device_name") +
                        " is in your group but is offline, not an inference server, or not one this device may connect to. " + listed;
                return false;
            }
        }
    }
    error = "No device in your group is named " + wanted + ". " + listed;
    return false;
}

}  // namespace lemon_cli
