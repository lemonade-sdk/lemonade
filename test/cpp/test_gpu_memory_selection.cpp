#include <lemon/gpu_memory_selection.h>
#include <lemon/nvidia_smi_parse.h>

#include <cmath>
#include <cstdio>

using namespace lemon;

static GPUInfo gpu(double vram_gb, double used_gb = 0.0, int index = -1,
                   const std::string& uuid = "") {
    GPUInfo info;
    info.available = true;
    info.vram_gb = vram_gb;
    info.vram_used_gb = used_gb;
    info.index = index;
    info.uuid = uuid;
    return info;
}

int main() {
    int failures = 0;
    auto check = [&](bool condition, const char* name) {
        std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
        if (!condition) ++failures;
    };

    GPUInfo unavailable;
    std::vector<GPUInfo> amd{gpu(4.0, 1.0)};
    std::vector<GPUInfo> nvidia{gpu(11.0, 2.0, 0)};

    auto cuda = select_gpu_memory_pool(
        gpu_memory_vendor_for_target("cuda", ""), unavailable, amd, nvidia, unavailable);
    check(cuda.total_gb == 11.0 && cuda.used_gb == 2.0 && cuda.label == "NVIDIA",
          "CUDA selects NVIDIA total and used memory");

    auto rocm = select_gpu_memory_pool(
        gpu_memory_vendor_for_target("rocm-stable", ""), unavailable, amd, nvidia, unavailable);
    check(rocm.total_gb == 4.0 && rocm.used_gb == 1.0 && rocm.label == "AMD dGPU",
          "ROCm selects AMD total and used memory");

    auto system_cuda = select_gpu_memory_pool(
        gpu_memory_vendor_for_target("system", "CUDA0"), unavailable, amd, nvidia, unavailable,
        "CUDA0");
    check(system_cuda.total_gb == 11.0, "explicit CUDA device selects NVIDIA memory");

    std::vector<GPUInfo> two_nvidia{gpu(8.0, 1.0, 0), gpu(24.0, 4.0, 1)};
    auto cuda1 = select_gpu_memory_pool(
        gpu_memory_vendor_for_target("system", "CUDA1"), unavailable, amd, two_nvidia,
        unavailable, "CUDA1");
    check(cuda1.total_gb == 24.0 && cuda1.used_gb == 4.0 && cuda1.label == "NVIDIA CUDA1",
          "CUDA1 selects NVIDIA device 1");

    std::vector<GPUInfo> filtered_nvidia{
        gpu(8.0, 1.0, 0, "GPU-aaaa"), gpu(12.0, 2.0, 1, "GPU-bbbb"),
        gpu(16.0, 3.0, 2, "GPU-cccc"), gpu(24.0, 4.0, 3, "GPU-dddd")};
    auto filtered_cuda1 = select_gpu_memory_pool(
        gpu_memory_vendor_for_target("system", "CUDA1"), unavailable, amd,
        filtered_nvidia, unavailable, "CUDA1", "2,3");
    check(filtered_cuda1.total_gb == 24.0 && filtered_cuda1.used_gb == 4.0,
          "CUDA1 follows CUDA_VISIBLE_DEVICES ordinal mapping");

    auto uuid_filtered_cuda0 = select_gpu_memory_pool(
        gpu_memory_vendor_for_target("system", "CUDA0"), unavailable, amd,
        filtered_nvidia, unavailable, "CUDA0", "GPU-dddd,GPU-cccc");
    check(uuid_filtered_cuda0.total_gb == 24.0 && uuid_filtered_cuda0.used_gb == 4.0,
          "CUDA0 follows UUID CUDA_VISIBLE_DEVICES ordering");

    GPUInfo amd_igpu = gpu(2.0, 0.5);
    amd_igpu.virtual_gb = 6.0;
    amd_igpu.virtual_used_gb = 1.5;
    std::vector<GPUInfo> two_amd{gpu(16.0, 3.0)};
    auto rocm1 = select_gpu_memory_pool(
        gpu_memory_vendor_for_target("system", "ROCm1"), amd_igpu, two_amd, nvidia,
        unavailable, "ROCm1");
    check(rocm1.total_gb == 16.0 && rocm1.used_gb == 3.0 && rocm1.label == "AMD ROCm1",
          "ROCm1 selects AMD device 1");

    auto cuda_list = select_gpu_memory_pool(
        gpu_memory_vendor_for_target("system", "CUDA0,CUDA1"), unavailable, amd, two_nvidia,
        unavailable, "CUDA0,CUDA1");
    check(cuda_list.label == "NVIDIA CUDA0" && cuda_list.total_gb - cuda_list.used_gb == 7.0,
          "CUDA device list uses the most constrained selected GPU");

    auto missing_cuda = select_gpu_memory_pool(
        gpu_memory_vendor_for_target("system", "CUDA2"), unavailable, amd, two_nvidia,
        unavailable, "CUDA2");
    check(missing_cuda.total_gb == 0.0, "unknown explicit CUDA device does not use another GPU");

    auto malformed_cuda = select_gpu_memory_pool(
        gpu_memory_vendor_for_target("system", "CUDA0+1"), unavailable, amd, two_nvidia,
        unavailable, "CUDA0+1");
    check(malformed_cuda.total_gb == 0.0,
          "malformed CUDA device does not silently select another GPU");

    auto partly_malformed_cuda = select_gpu_memory_pool(
        gpu_memory_vendor_for_target("system", "CUDA0,CUDA1+2"), unavailable, amd,
        two_nvidia, unavailable, "CUDA0,CUDA1+2");
    check(partly_malformed_cuda.total_gb == 0.0,
          "malformed CUDA device list is not partially accepted");

    auto bare_cuda = select_gpu_memory_pool(
        gpu_memory_vendor_for_target("system", "CUDA"), unavailable, amd, two_nvidia,
        unavailable, "CUDA");
    check(bare_cuda.total_gb == 8.0, "bare CUDA selects the first NVIDIA pool");

    auto filtered_bare_cuda = select_gpu_memory_pool(
        gpu_memory_vendor_for_target("system", "CUDA"), unavailable, amd,
        filtered_nvidia, unavailable, "CUDA", "2,3");
    check(filtered_bare_cuda.total_gb == 16.0,
          "bare CUDA selects the first visible NVIDIA pool");

    auto hidden_cuda = select_gpu_memory_pool(
        gpu_memory_vendor_for_target("system", "CUDA0"), unavailable, amd,
        filtered_nvidia, unavailable, "CUDA0", "-1");
    check(hidden_cuda.total_gb == 0.0,
          "CUDA_VISIBLE_DEVICES=-1 leaves no NVIDIA memory pool");

    auto explicitly_empty_cuda = select_gpu_memory_pool(
        gpu_memory_vendor_for_target("cuda", ""), unavailable, amd,
        filtered_nvidia, unavailable, "", std::string{});
    check(explicitly_empty_cuda.total_gb == 0.0,
          "empty CUDA_VISIBLE_DEVICES leaves no NVIDIA memory pool");

    auto generic_ignores_cuda_visibility = select_gpu_memory_pool(
        GpuMemoryVendor::Any, unavailable, {}, filtered_nvidia, unavailable,
        "", std::string{});
    check(generic_ignores_cuda_visibility.total_gb == 8.0,
          "generic GPU selection ignores CUDA visibility filtering");

    auto bare_rocm = select_gpu_memory_pool(
        gpu_memory_vendor_for_target("system", "ROCm"), amd_igpu, two_amd, nvidia,
        unavailable, "ROCm");
    check(bare_rocm.total_gb == 8.0, "bare ROCm selects the first AMD pool");

    auto lowercase_cuda = select_gpu_memory_pool(
        gpu_memory_vendor_for_target("SYSTEM", "cuda1"), unavailable, amd, two_nvidia,
        unavailable, "cuda1");
    check(lowercase_cuda.total_gb == 24.0,
          "CUDA vendor and device matching is case-insensitive");

    GPUInfo amd_igpu_unknown_usage = gpu(2.0, 0.5);
    amd_igpu_unknown_usage.virtual_gb = 6.0;
    auto unknown_usage = select_gpu_memory_pool(
        GpuMemoryVendor::Amd, amd_igpu_unknown_usage, two_amd, nvidia, unavailable);
    check(unknown_usage.used_gb < 0.0,
          "incomplete unified-memory usage remains unknown");

    std::vector<GPUInfo> mixed_usage_nvidia{
        gpu(24.0, -1.0, 0), gpu(8.0, 1.0, 1)};
    auto unknown_constrained = select_gpu_memory_pool(
        gpu_memory_vendor_for_target("cuda", "CUDA0,CUDA1"), unavailable, amd,
        mixed_usage_nvidia, unavailable, "CUDA0,CUDA1");
    check(unknown_constrained.label == "NVIDIA CUDA0" && unknown_constrained.used_gb < 0.0,
          "unknown usage is the most constrained selected GPU");

    GPUInfo apple = gpu(16.0, 2.0);
    auto metal = select_gpu_memory_pool(
        GpuMemoryVendor::Metal, unavailable, amd, nvidia, apple);
    check(metal.vendor == GpuMemoryVendor::Metal,
          "Metal pool carries typed vendor identity");

    auto automatic = select_gpu_memory_pool(
        GpuMemoryVendor::Any, unavailable, amd, nvidia, unavailable);
    check(automatic.total_gb == 4.0, "ambiguous target preserves fallback order");

    // ROCm assigns ordinals by ascending KFD node number, which need not match
    // SystemInfo's iGPU-then-dGPU enumeration order. A host where the dGPU has the
    // lower KFD node (ordinal 0) and the iGPU the higher one (ordinal 1) must select
    // by GPUInfo::index, not by vector position.
    GPUInfo ordinal_igpu = gpu(2.0, 0.5, /*index=*/1);
    ordinal_igpu.virtual_gb = 6.0;
    ordinal_igpu.virtual_used_gb = 1.5;
    std::vector<GPUInfo> ordinal_dgpu{gpu(16.0, 3.0, /*index=*/0)};
    auto rocm0_by_ordinal = select_gpu_memory_pool(
        gpu_memory_vendor_for_target("system", "ROCm0"), ordinal_igpu, ordinal_dgpu,
        nvidia, unavailable, "ROCm0");
    check(rocm0_by_ordinal.total_gb == 16.0 && rocm0_by_ordinal.label == "AMD ROCm0",
          "ROCm0 selects the KFD-ordinal-0 device even when it is not first in "
          "enumeration order");
    auto rocm1_by_ordinal = select_gpu_memory_pool(
        gpu_memory_vendor_for_target("system", "ROCm1"), ordinal_igpu, ordinal_dgpu,
        nvidia, unavailable, "ROCm1");
    check(rocm1_by_ordinal.total_gb == 2.0 + 6.0 && rocm1_by_ordinal.label == "AMD ROCm1",
          "ROCm1 selects the KFD-ordinal-1 iGPU even when it is enumerated first");

    // --- nvidia-smi CSV row parsing ---

    auto well_formed = parse_nvidia_smi_line(
        "0, GPU-1234, NVIDIA GeForce RTX 4090, 8.9, 550.54.15, 24564, 1024", 7);
    check(well_formed.index == 0 && well_formed.uuid == "GPU-1234" &&
              well_formed.name == "NVIDIA GeForce RTX 4090" &&
              well_formed.compute_cap == "8.9" &&
              std::abs(well_formed.vram_gb - 24564.0 / 1024.0) < 1e-6 &&
              std::abs(well_formed.vram_used_gb - 1024.0 / 1024.0) < 1e-6,
          "well-formed nvidia-smi row parses all fields");

    auto comma_in_name = parse_nvidia_smi_line(
        "1, GPU-5678, NVIDIA T400, 4GB, 7.5, 528.02, 4096, 512", 7);
    check(comma_in_name.name == "NVIDIA T400, 4GB" && comma_in_name.compute_cap == "7.5",
          "comma embedded in GPU name does not break field splitting");

    auto not_supported_used = parse_nvidia_smi_line(
        "0, GPU-aaaa, Tesla T4, 7.5, 470.199.02, 16384, [Not Supported]", 0);
    check(not_supported_used.vram_used_gb < 0.0,
          "[Not Supported] memory.used falls back to the -1 sentinel");

    auto truncated = parse_nvidia_smi_line("0, GPU-aaaa, Tesla T4", 3);
    check(truncated.compute_cap.empty(),
          "truncated row (missing trailing fields) is rejected");

    auto empty_row = parse_nvidia_smi_line("   ", 2);
    check(empty_row.compute_cap.empty() && empty_row.name.empty(),
          "empty row is rejected");

    auto unparseable_index = parse_nvidia_smi_line(
        "n/a, GPU-bbbb, Some GPU, 8.6, 550.00, 8192, 256", 5);
    check(unparseable_index.index == 5,
          "unparseable index field falls back to the caller-supplied ordinal");

    // --- ctx-size scope-warning classification ---

    CtxMemoryScope single_gpu_scope;
    single_gpu_scope.is_gpu = true;
    single_gpu_scope.device_named = true;
    single_gpu_scope.reachable_gpu_count = 1;
    check(classify_ctx_scope(single_gpu_scope) == CtxScopeWarning::None,
          "single-GPU host stays silent even when unscoped");

    CtxMemoryScope unscoped_scope;
    unscoped_scope.is_gpu = true;
    unscoped_scope.device_named = true;
    unscoped_scope.per_device = false;
    unscoped_scope.reachable_gpu_count = 2;
    check(classify_ctx_scope(unscoped_scope) == CtxScopeWarning::Unscoped,
          "vendor named without an ordinal on a multi-GPU host is Unscoped");

    CtxMemoryScope per_device_scope = unscoped_scope;
    per_device_scope.per_device = true;
    check(classify_ctx_scope(per_device_scope) == CtxScopeWarning::None,
          "an explicit ordinal on a multi-GPU host is fully scoped");

    CtxMemoryScope ambiguous_scope;
    ambiguous_scope.is_gpu = true;
    ambiguous_scope.device_named = false;
    ambiguous_scope.reachable_gpu_count = 2;
    ambiguous_scope.ambiguous = true;
    check(classify_ctx_scope(ambiguous_scope) == CtxScopeWarning::Ambiguous,
          "no device named on a multi-GPU host is Ambiguous");

    return failures == 0 ? 0 : 1;
}
