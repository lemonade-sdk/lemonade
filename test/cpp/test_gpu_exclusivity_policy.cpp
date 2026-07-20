#include "lemon/gpu_exclusivity_policy.h"

#include <cstdio>

using lemon::DEVICE_CPU;
using lemon::DEVICE_GPU;
using lemon::IncomingLoadGpuInfo;
using lemon::LoadedGpuServerInfo;
using lemon::SlotPolicy;
using lemon::gpu_exclusivity_eviction_targets;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static void test_vte_evicts_existing_amd_gpu_peer() {
    IncomingLoadGpuInfo incoming{DEVICE_GPU, SlotPolicy::ExclusiveGpu, true};
    std::vector<LoadedGpuServerInfo> loaded = {
        {"llamacpp-rocm-model", DEVICE_GPU, SlotPolicy::Standard, true},
    };
    auto targets = gpu_exclusivity_eviction_targets(incoming, loaded);
    check("VTE load evicts an existing AMD-GPU peer", targets == std::vector<size_t>{0});
}

static void test_amd_gpu_peer_evicts_loaded_vte() {
    IncomingLoadGpuInfo incoming{DEVICE_GPU, SlotPolicy::Standard, true};
    std::vector<LoadedGpuServerInfo> loaded = {
        {"vte-model", DEVICE_GPU, SlotPolicy::ExclusiveGpu, true},
    };
    auto targets = gpu_exclusivity_eviction_targets(incoming, loaded);
    check("a later AMD-GPU load evicts an already-loaded VTE", targets == std::vector<size_t>{0});
}

static void test_cpu_variant_never_touches_vte() {
    IncomingLoadGpuInfo incoming{DEVICE_CPU, SlotPolicy::Standard, false};
    std::vector<LoadedGpuServerInfo> loaded = {
        {"vte-model", DEVICE_GPU, SlotPolicy::ExclusiveGpu, true},
    };
    auto targets = gpu_exclusivity_eviction_targets(incoming, loaded);
    check("a CPU-variant load does not evict VTE", targets.empty());
}

static void test_gpu_variant_with_cpu_default_descriptor_still_detected() {
    // Simulates a backend whose descriptor default_device is CPU but whose
    // resolved backend variant (e.g. whisper-rocm) is GPU/AMD -- the caller is
    // responsible for resolving this before building IncomingLoadGpuInfo; this
    // test proves the decision logic itself treats the *value* correctly,
    // independent of any descriptor constant.
    IncomingLoadGpuInfo incoming{DEVICE_GPU, SlotPolicy::Standard, true};
    std::vector<LoadedGpuServerInfo> loaded = {
        {"vte-model", DEVICE_GPU, SlotPolicy::ExclusiveGpu, true},
    };
    auto targets = gpu_exclusivity_eviction_targets(incoming, loaded);
    check("a GPU/AMD variant resolved from a CPU-default descriptor still evicts VTE",
          targets == std::vector<size_t>{0});
}

static void test_separate_vendor_gpu_does_not_cross_evict() {
    IncomingLoadGpuInfo incoming{DEVICE_GPU, SlotPolicy::ExclusiveGpu, true};
    std::vector<LoadedGpuServerInfo> loaded = {
        {"llamacpp-cuda-model", DEVICE_GPU, SlotPolicy::Standard, false},
    };
    auto targets = gpu_exclusivity_eviction_targets(incoming, loaded);
    check("VTE load does not evict a separate-vendor (CUDA) GPU peer", targets.empty());
}

static void test_separate_vendor_gpu_not_evicted_by() {
    IncomingLoadGpuInfo incoming{DEVICE_GPU, SlotPolicy::Standard, false};
    std::vector<LoadedGpuServerInfo> loaded = {
        {"vte-model", DEVICE_GPU, SlotPolicy::ExclusiveGpu, true},
    };
    auto targets = gpu_exclusivity_eviction_targets(incoming, loaded);
    check("a separate-vendor (CUDA) load does not evict VTE", targets.empty());
}

static void test_no_loaded_servers() {
    IncomingLoadGpuInfo incoming{DEVICE_GPU, SlotPolicy::ExclusiveGpu, true};
    std::vector<LoadedGpuServerInfo> loaded = {};
    auto targets = gpu_exclusivity_eviction_targets(incoming, loaded);
    check("no loaded servers means nothing to evict", targets.empty());
}

int main() {
    test_vte_evicts_existing_amd_gpu_peer();
    test_amd_gpu_peer_evicts_loaded_vte();
    test_cpu_variant_never_touches_vte();
    test_gpu_variant_with_cpu_default_descriptor_still_detected();
    test_separate_vendor_gpu_does_not_cross_evict();
    test_separate_vendor_gpu_not_evicted_by();
    test_no_loaded_servers();

    if (g_failures == 0) {
        std::printf("All GPU exclusivity policy tests passed.\n");
    }
    return g_failures == 0 ? 0 : 1;
}
