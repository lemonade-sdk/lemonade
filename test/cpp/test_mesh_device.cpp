// Checks how `lemonade launch --device` picks a mesh device from lemond's
// /api/v1/nexus/status: the device's name, node id or mesh port host label selects
// the entry in mesh_port.urls, and every miss explains what is available.
// Build with: cmake --build --preset default --target test_mesh_device
// Run with: ctest --test-dir build -R '^MeshDeviceTest$' --output-on-failure

#include "lemon_cli/mesh_device.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <string>

using nlohmann::json;

namespace {

int failures = 0;

void check(const std::string& what, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

json status_with_port(bool running) {
    return json{
        {"enabled", true},
        {"node_id", "self0001"},
        {"members", json::array({
            {{"node_id", "self0001"}, {"device_name", "Laptop"}, {"is_online", true}, {"is_inference", true}},
            {{"node_id", "gpu00001"}, {"device_name", "Gaming PC"}, {"is_online", true}, {"is_inference", true}},
            {{"node_id", "mini0001"}, {"device_name", "Mac mini"}, {"is_online", false}, {"is_inference", true}},
            {{"node_id", "phone001"}, {"device_name", "Phone"}, {"is_online", true}, {"is_inference", false}},
        })},
        {"mesh_port", {
            {"running", running},
            {"port", 13306},
            {"scheme", "https"},
            {"local_domain", ".g1.lo.example.io"},
            {"urls", json::array({
                {{"node_id", "self0001"}, {"device_name", "Laptop"}, {"url", "https://laptop.g1.lo.example.io:13306"}, {"self", true}},
                {{"node_id", "gpu00001"}, {"device_name", "Gaming PC"}, {"url", "https://gaming-pc.g1.lo.example.io:13306"}, {"self", false}},
            })},
        }},
    };
}

}  // namespace

int main() {
    const auto status = status_with_port(true);
    lemon_cli::MeshDevice device;
    std::string error;

    const auto listed = lemon_cli::mesh_devices(status);
    check("the mesh port lists this device and every reachable inference device", listed.size() == 2);
    check("the mesh port URLs are kept as lemond gives them",
          listed.size() == 2 && listed[1].url == "https://gaming-pc.g1.lo.example.io:13306" && !listed[1].self && listed[0].self);

    check("a device is found by its name, whatever the case",
          lemon_cli::resolve_mesh_device(status, "gaming pc", device, error) && device.node_id == "gpu00001");
    check("a device is found by its node id",
          lemon_cli::resolve_mesh_device(status, "GPU00001", device, error) && device.device_name == "Gaming PC");
    check("a device is found by the host label of its mesh port URL",
          lemon_cli::resolve_mesh_device(status, "gaming-pc", device, error) &&
              device.url == "https://gaming-pc.g1.lo.example.io:13306");
    check("this server is found and marked as itself",
          lemon_cli::resolve_mesh_device(status, "Laptop", device, error) && device.self);

    error.clear();
    check("an offline member is named as such, with the devices that are available",
          !lemon_cli::resolve_mesh_device(status, "Mac mini", device, error) &&
              error.find("Mac mini") != std::string::npos && error.find("offline") != std::string::npos &&
              error.find("Gaming PC (gpu00001)") != std::string::npos && error.find("Laptop") == std::string::npos);
    error.clear();
    check("a member that is not an inference server is refused the same way",
          !lemon_cli::resolve_mesh_device(status, "phone001", device, error) && error.find("Phone") != std::string::npos);
    error.clear();
    check("an unknown name lists the devices that are available",
          !lemon_cli::resolve_mesh_device(status, "Toaster", device, error) &&
              error.find("No device in your group is named Toaster") == 0 &&
              error.find("Gaming PC (gpu00001)") != std::string::npos);

    error.clear();
    check("a mesh port that is not running reaches no device",
          !lemon_cli::resolve_mesh_device(status_with_port(false), "Gaming PC", device, error) &&
              error.find("mesh port is not running") != std::string::npos);
    error.clear();
    check("a mesh that is off says so",
          !lemon_cli::resolve_mesh_device(json{{"enabled", false}}, "Gaming PC", device, error) &&
              error.find("mesh is off") != std::string::npos);
    check("a status without a mesh port lists no device", lemon_cli::mesh_devices(json{{"enabled", true}}).empty());

    auto alone = status_with_port(true);
    alone["mesh_port"]["urls"] = json::array({alone["mesh_port"]["urls"][0]});
    error.clear();
    check("with no other device online the message says so",
          !lemon_cli::resolve_mesh_device(alone, "Gaming PC", device, error) &&
              error.find("No other device is online") != std::string::npos);

    // A mesh port without the group's domain names no device: it lists only itself as
    // a loopback URL, as a server whose certificate is not a group wildcard does.
    auto nameless = status_with_port(true);
    nameless["mesh_port"]["local_domain"] = "";
    nameless["mesh_port"]["scheme"] = "http";
    nameless["mesh_port"]["tls"] = {{"state", "unsupported_certificate"}, {"error", "The certificate is not a group wildcard"}};
    nameless["mesh_port"]["urls"] = json::array({{{"node_id", "self0001"}, {"device_name", ""}, {"url", "http://127.0.0.1:13306"}, {"self", true}}});
    error.clear();
    check("a mesh port without a group domain says it cannot name devices and why",
          !lemon_cli::resolve_mesh_device(nameless, "Gaming PC", device, error) &&
              error.find("no group domain") != std::string::npos &&
              error.find("not a group wildcard") != std::string::npos &&
              error.find("selection") != std::string::npos);

    std::printf("\n%d mesh device check(s) failed.\n", failures);
    return failures == 0 ? 0 : 1;
}
