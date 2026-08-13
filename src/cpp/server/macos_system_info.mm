#include "lemon/system_info.h"
#include <MacTypes.h>

#ifdef __APPLE__

#include <sys/sysctl.h>
#include <mach/mach.h>
#include <Metal/Metal.h>
#include <sstream>
#include <iomanip>

namespace lemon {

std::vector<GPUInfo> MacOSSystemInfo::detect_metal_gpus() {
    std::vector<GPUInfo> gpus;
    //Check to make sure we are on at least version 10.11 to support Metal API
    if (@available(macOS 10.11, *)) {
        // Use Metal to enumerate available GPUs
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device) {
            GPUInfo gpu;
            NSString* deviceName = device.name;
            std::string device_name_str = deviceName ? [deviceName UTF8String] : "Unknown Metal Device";
            NSLog(@"[Metal] Detected device: %s", device_name_str.c_str());
            gpu.name = device_name_str;
            gpu.available = true;

            // Get VRAM size
            uint64_t vram_bytes = [device recommendedMaxWorkingSetSize];
            gpu.vram_gb = vram_bytes / (1024.0 * 1024.0 * 1024.0);

            gpu.driver_version = "Metal";
            gpus.push_back(gpu);

            // Metal can have multiple devices - enumerate all
            @autoreleasepool {
                NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
                for (id<MTLDevice> dev in devices) {
                    if (dev != device) {  // Skip the default device we already added
                        GPUInfo additional_gpu;
                        additional_gpu.name = [dev.name UTF8String];
                        additional_gpu.available = true;

                        uint64_t additional_vram = [dev recommendedMaxWorkingSetSize];
                        additional_gpu.vram_gb = additional_vram / (1024.0 * 1024.0 * 1024.0);
                        additional_gpu.driver_version = [dev.architecture.name UTF8String];
                        gpus.push_back(additional_gpu);
                    }
                }
                devices = nil;
            }
        }
        if (gpus.empty()) {
            GPUInfo gpu;
            gpu.available = false;
            gpu.error = "No Metal-compatible GPU found";
            gpus.push_back(gpu);
        }

        return gpus;
    }
    else {
        return {}; // or return empty vector
    }
}

// Apple Silicon presents a single unified-memory pool shared by CPU and GPU.
// Report it as a GPUInfo so auto-tune can treat it like any other GPU device:
//   vram_gb     = Metal's recommended GPU working-set budget (recommendedMaxWorkingSetSize)
//   virtual_gb  = total physical RAM (hw.memsize), the rest of the unified pool
GPUInfo MacOSSystemInfo::get_apple_silicon_device() {
    GPUInfo gpu;

    // Total unified memory (physical RAM).
    uint64_t mem_bytes = 0;
    size_t size = sizeof(mem_bytes);
    double total_ram_gb = 0.0;
    if (sysctlbyname("hw.memsize", &mem_bytes, &size, nullptr, 0) == 0 && mem_bytes > 0) {
        total_ram_gb = mem_bytes / (1024.0 * 1024.0 * 1024.0);
    }

    // Metal's advertised GPU working-set budget (a conservative slice of unified memory).
    std::vector<GPUInfo> metal_gpus = detect_metal_gpus();
    if (!metal_gpus.empty() && metal_gpus[0].available) {
        gpu = metal_gpus[0];
        gpu.virtual_gb = total_ram_gb;
        return gpu;
    }

    // No Metal device (e.g. very old macOS): fall back to reporting RAM only.
    if (total_ram_gb > 0.0) {
        gpu.available = true;
        gpu.name = "Apple Silicon (unified memory)";
        gpu.vram_gb = total_ram_gb;
        gpu.virtual_gb = total_ram_gb;
        return gpu;
    }

    gpu.available = false;
    gpu.error = "Could not determine Apple Silicon unified memory";
    return gpu;
}
} // namespace lemon

#endif // __APPLE__
