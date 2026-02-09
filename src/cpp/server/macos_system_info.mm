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
} // namespace lemon

#endif // __APPLE__
