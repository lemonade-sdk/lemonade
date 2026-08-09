#include <lemon/gpu_memory_selection.h>

#include <cstdio>

using namespace lemon;

static GPUInfo gpu(double vram_gb) {
    GPUInfo info;
    info.available = true;
    info.vram_gb = vram_gb;
    return info;
}

int main() {
    int failures = 0;
    auto check = [&](bool condition, const char* name) {
        std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
        if (!condition) ++failures;
    };

    GPUInfo unavailable;
    std::vector<GPUInfo> amd{gpu(4.0)};
    std::vector<GPUInfo> nvidia{gpu(11.0)};

    auto cuda = select_gpu_memory_pool(
        gpu_memory_vendor_for_target("cuda", ""), unavailable, amd, nvidia, unavailable);
    check(cuda.total_gb == 11.0 && cuda.label == "NVIDIA", "CUDA selects NVIDIA memory");

    auto rocm = select_gpu_memory_pool(
        gpu_memory_vendor_for_target("rocm-stable", ""), unavailable, amd, nvidia, unavailable);
    check(rocm.total_gb == 4.0 && rocm.label == "AMD dGPU", "ROCm selects AMD memory");

    auto system_cuda = select_gpu_memory_pool(
        gpu_memory_vendor_for_target("system", "CUDA0"), unavailable, amd, nvidia, unavailable);
    check(system_cuda.total_gb == 11.0, "explicit CUDA device selects NVIDIA memory");

    auto automatic = select_gpu_memory_pool(
        GpuMemoryVendor::Any, unavailable, amd, nvidia, unavailable);
    check(automatic.total_gb == 4.0, "ambiguous target preserves fallback order");

    return failures == 0 ? 0 : 1;
}
