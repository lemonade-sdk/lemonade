#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace lemon_cli {

/// One device the mesh port names: this device itself, or an online Lemonade server
/// of the group that this device may connect to. `url` is the mesh port URL
/// "https://<device>.<group>.lo.<base>:13306" (http before the port has its
/// certificate); the mesh port forwards a request to that name over the mesh to that
/// device.
struct MeshDevice {
    std::string node_id;
    std::string device_name;
    std::string url;
    bool self{false};
};

/// The devices in `status` (lemond's GET /v1/nexus/status), from `mesh_port.urls`.
/// Empty when the mesh is off or the mesh port is not running.
std::vector<MeshDevice> mesh_devices(const nlohmann::json& status);

/// Finds the device `wanted` names: its device name (case-insensitive), its node id,
/// or the host label of its mesh port URL.
bool resolve_mesh_device(const nlohmann::json& status, const std::string& wanted, MeshDevice& device,
                         std::string& error);

}  // namespace lemon_cli
