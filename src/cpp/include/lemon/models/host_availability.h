#pragma once

#include <lemon/models/backend_availability.h>
#include <nlohmann/json.hpp>

#include <map>
#include <string>

namespace lemon {

struct ModelInfo;

namespace models {

// The memory one reported device can offer a model. An AMD APU is reported
// under "amd_gpu" with "integrated": true and its aperture in virtual_mem_gb,
// so the device decides, not the device-type key.
double device_memory_pool_gb(const std::string& device_type,
                             const nlohmann::json& device,
                             bool assume_dgpu_gtt);

// The machine facts the hardware filter needs, read from configuration and
// system info so filter_hardware() stays a pure function of its inputs.
BackendAvailability probe_backend_availability(const std::map<std::string, ModelInfo>& models);

}  // namespace models
}  // namespace lemon
