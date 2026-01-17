#include "lemon/system_info.h"

#ifdef __APPLE__

#include <sys/sysctl.h>
#include <mach/mach.h>
#include <Metal/Metal.h>
#include <sstream>
#include <iomanip>

namespace lemon {

std::vector<GPUInfo> MacOSSystemInfo::detect_metal_gpus() {
    std::vector<GPUInfo> gpus;

    // Use Metal to enumerate available GPUs
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device) {
        GPUInfo gpu;
        gpu.name = [device.name UTF8String];
        gpu.available = true;

        // Get VRAM size
        uint64_t vram_bytes = [device recommendedMaxWorkingSetSize];
        gpu.vram_gb = vram_bytes / (1024.0 * 1024.0 * 1024.0);

        gpu.driver_version = "Metal";

        // Detect inference engines for Metal GPU
        gpu.inference_engines = detect_inference_engines("metal_gpu", gpu.name);

        gpus.push_back(gpu);

        // Metal can have multiple devices - enumerate all
        NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
        for (id<MTLDevice> dev in devices) {
            if (dev != device) {  // Skip the default device we already added
                GPUInfo additional_gpu;
                additional_gpu.name = [dev.name UTF8String];
                additional_gpu.available = true;

                uint64_t additional_vram = [dev recommendedMaxWorkingSetSize];
                additional_gpu.vram_gb = additional_vram / (1024.0 * 1024.0 * 1024.0);
                additional_gpu.driver_version = "Metal";

                // Detect inference engines for additional Metal GPU
                additional_gpu.inference_engines = detect_inference_engines("metal_gpu", additional_gpu.name);

                gpus.push_back(additional_gpu);
            }
        }
        [devices release];
    }

    [device release];

    if (gpus.empty()) {
        GPUInfo gpu;
        gpu.available = false;
        gpu.error = "No Metal-compatible GPU found";
        gpus.push_back(gpu);
    }

    return gpus;
}

} // namespace lemon

#endif // __APPLE__
