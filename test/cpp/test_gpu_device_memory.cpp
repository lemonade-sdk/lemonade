// Standalone test for GPU device-string parsing and memory-pool selection.
//
// Compile: g++ -std=c++17 -I src/cpp/include test/cpp/test_gpu_device_memory.cpp -o test_gpu_device_memory

#include "lemon/gpu_device_memory.h"
#include "lemon/nvidia_smi_parse.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using lemon::classify_ctx_scope;
using lemon::CtxMemoryScope;
using lemon::CtxScopeWarning;
using lemon::GpuMemoryCandidate;
using lemon::GpuMemoryPool;
using lemon::GpuVendor;
using lemon::parse_gpu_device_selection;
using lemon::parse_nvidia_smi_row;
using lemon::select_gpu_memory_pool;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static bool approx_eq(double a, double b, double tol = 0.001) {
    return std::fabs(a - b) < tol;
}

static GpuMemoryCandidate make_candidate(int index, double total, double used,
                                         const char* label = "AMD dGPU") {
    GpuMemoryCandidate c;
    c.index = index;
    c.total_gb = total;
    c.used_gb = used;
    c.label = label;
    return c;
}

// The 2x dGPU topology from issue #3224: one card loaded, one mostly free.
static std::vector<GpuMemoryCandidate> two_amd_dgpus() {
    return {make_candidate(0, 31.9, 11.2), make_candidate(1, 31.9, 27.8)};
}

static void test_parse_single_device() {
    auto sel = parse_gpu_device_selection("ROCm1");
    check("ROCm1 parses as AMD", sel.vendor == GpuVendor::AMD && !sel.malformed);
    check("ROCm1 yields ordinal 1", sel.ordinals.size() == 1 && sel.ordinals[0] == 1);

    auto cuda = parse_gpu_device_selection("CUDA0");
    check("CUDA0 parses as NVIDIA", cuda.vendor == GpuVendor::NVIDIA && !cuda.malformed);
    check("CUDA0 yields ordinal 0", cuda.ordinals.size() == 1 && cuda.ordinals[0] == 0);

    auto vk = parse_gpu_device_selection("Vulkan0");
    check("Vulkan0 parses as Vulkan", vk.vendor == GpuVendor::Vulkan && !vk.malformed);
}

static void test_parse_device_list() {
    auto sel = parse_gpu_device_selection("CUDA0,CUDA1");
    check("CUDA0,CUDA1 is well-formed", !sel.malformed && sel.vendor == GpuVendor::NVIDIA);
    check("CUDA0,CUDA1 yields two ordinals",
          sel.ordinals.size() == 2 && sel.ordinals[0] == 0 && sel.ordinals[1] == 1);

    auto spaced = parse_gpu_device_selection("ROCm0, ROCm2");
    check("whitespace after comma is tolerated",
          !spaced.malformed && spaced.ordinals.size() == 2 && spaced.ordinals[1] == 2);
}

static void test_parse_rejects_malformed() {
    check("empty string is not malformed, just unselected",
          !parse_gpu_device_selection("").malformed &&
              parse_gpu_device_selection("").ordinals.empty());
    check("bare vendor with no ordinal is malformed",
          parse_gpu_device_selection("ROCm").malformed);
    check("unknown vendor is malformed", parse_gpu_device_selection("Metal0").malformed);
    check("non-numeric ordinal is malformed", parse_gpu_device_selection("ROCmX").malformed);
    check("trailing comma is malformed", parse_gpu_device_selection("ROCm0,").malformed);
    check("mixed vendors are malformed", parse_gpu_device_selection("ROCm0,CUDA1").malformed);
    check("malformed selections carry no ordinals",
          parse_gpu_device_selection("ROCm0,CUDA1").ordinals.empty());
}

static void test_selects_named_device() {
    const auto amd = two_amd_dgpus();
    const std::vector<GpuMemoryCandidate> none;

    auto rocm0 = select_gpu_memory_pool(amd, none, "rocm-stable", "ROCm0");
    check("ROCm0 resolves", rocm0.resolved);
    check("ROCm0 reports its own free VRAM", approx_eq(rocm0.free_gb(), 20.7));
    check("ROCm0 is not ambiguous", !rocm0.ambiguous);

    auto rocm1 = select_gpu_memory_pool(amd, none, "rocm-stable", "ROCm1");
    check("ROCm1 resolves", rocm1.resolved);
    check("ROCm1 reports its own free VRAM", approx_eq(rocm1.free_gb(), 4.1));
}

// The regression behind #3224: selection must key off the reported ordinal, not the
// position of the entry in the vector, since KFD enumeration order is not sorted.
static void test_ordinal_beats_vector_position() {
    std::vector<GpuMemoryCandidate> reversed = {make_candidate(1, 31.9, 27.8),
                                                make_candidate(0, 31.9, 11.2)};
    const std::vector<GpuMemoryCandidate> none;

    auto pool = select_gpu_memory_pool(reversed, none, "rocm-stable", "ROCm0");
    check("ROCm0 follows the ordinal under reversed enumeration",
          pool.resolved && approx_eq(pool.free_gb(), 20.7));
}

static void test_unknown_ordinal_is_unresolved() {
    const auto amd = two_amd_dgpus();
    const std::vector<GpuMemoryCandidate> none;

    auto pool = select_gpu_memory_pool(amd, none, "rocm-stable", "ROCm7");
    check("a device that does not exist is unresolved, not redirected", !pool.resolved);

    auto bad = select_gpu_memory_pool(amd, none, "rocm-stable", "ROCm");
    check("a malformed device string is unresolved", !bad.resolved);
}

static void test_device_list_takes_most_constrained() {
    const std::vector<GpuMemoryCandidate> nvidia = {
        make_candidate(0, 24.0, 2.0, "NVIDIA"), make_candidate(1, 24.0, 18.0, "NVIDIA")};
    const std::vector<GpuMemoryCandidate> none;

    auto pool = select_gpu_memory_pool(none, nvidia, "cuda", "CUDA0,CUDA1");
    check("a device list sizes against the tightest card",
          pool.resolved && approx_eq(pool.free_gb(), 6.0));
    check("an explicit list is not ambiguous", !pool.ambiguous);
}

static void test_vulkan_is_unresolved() {
    const auto amd = two_amd_dgpus();
    const std::vector<GpuMemoryCandidate> none;

    // Vulkan enumerates across vendors, so its ordinal is not attributable to a pool.
    check("Vulkan0 is unresolved",
          !select_gpu_memory_pool(amd, none, "vulkan", "Vulkan0").resolved);
    check("the vulkan backend alone is unresolved",
          !select_gpu_memory_pool(amd, none, "vulkan", "").resolved);
}

static void test_backend_implies_vendor() {
    const std::vector<GpuMemoryCandidate> amd = {make_candidate(0, 31.9, 11.2)};
    const std::vector<GpuMemoryCandidate> nvidia = {make_candidate(0, 24.0, 2.0, "NVIDIA")};

    auto rocm = select_gpu_memory_pool(amd, nvidia, "rocm-nightly", "");
    check("the rocm backend selects the AMD pool on a hybrid host",
          rocm.resolved && approx_eq(rocm.free_gb(), 20.7));

    auto cuda = select_gpu_memory_pool(amd, nvidia, "cuda", "");
    check("the cuda backend selects the NVIDIA pool on a hybrid host",
          cuda.resolved && approx_eq(cuda.free_gb(), 22.0));
}

static void test_unnamed_multi_gpu_is_ambiguous() {
    const auto amd = two_amd_dgpus();
    const std::vector<GpuMemoryCandidate> none;

    auto pool = select_gpu_memory_pool(amd, none, "rocm-stable", "");
    check("an unnamed target picks the most constrained GPU",
          pool.resolved && approx_eq(pool.free_gb(), 4.1));
    check("an unnamed multi-GPU target is flagged ambiguous", pool.ambiguous);

    const std::vector<GpuMemoryCandidate> single = {make_candidate(0, 31.9, 11.2)};
    auto only = select_gpu_memory_pool(single, none, "rocm-stable", "");
    check("a single-GPU host is unambiguous", only.resolved && !only.ambiguous);
    check("a single-GPU host reports that GPU's free VRAM", approx_eq(only.free_gb(), 20.7));
}

// A platform that reports capacity but not per-device usage cannot answer "how much
// is free"; treating the missing counter as zero would overstate headroom.
static void test_missing_usage_is_unresolved() {
    const std::vector<GpuMemoryCandidate> amd = {make_candidate(0, 31.9, -1.0)};
    const std::vector<GpuMemoryCandidate> none;

    check("a card with no usage reading is unresolved",
          !select_gpu_memory_pool(amd, none, "rocm-stable", "ROCm0").resolved);

    std::vector<GpuMemoryCandidate> mixed = {make_candidate(0, 31.9, 11.2),
                                             make_candidate(1, 31.9, -1.0)};
    check("a list containing an unreadable card is unresolved",
          !select_gpu_memory_pool(mixed, none, "rocm-stable", "ROCm0,ROCm1").resolved);
    check("a readable card in a mixed list still resolves on its own",
          select_gpu_memory_pool(mixed, none, "rocm-stable", "ROCm0").resolved);
}

// Inputs that must not hang, crash, or silently resolve to a device.
static void test_parse_hostile_inputs() {
    check("leading comma is malformed", parse_gpu_device_selection(",ROCm0").malformed);
    check("double comma is malformed", parse_gpu_device_selection("ROCm0,,ROCm1").malformed);
    check("whitespace-only is malformed", parse_gpu_device_selection("  ").malformed);
    check("lone comma is malformed", parse_gpu_device_selection(",").malformed);
    check("negative ordinal is malformed", parse_gpu_device_selection("ROCm-1").malformed);
    check("space before ordinal is malformed", parse_gpu_device_selection("ROCm 1").malformed);

    // Wider than int64; std::stoi throws and the token must be rejected, not truncated.
    auto overflow = parse_gpu_device_selection("ROCm999999999999999999999");
    check("an out-of-range ordinal is malformed", overflow.malformed && overflow.ordinals.empty());

    auto zeros = parse_gpu_device_selection("ROCm00");
    check("a leading-zero ordinal parses as its numeric value",
          !zeros.malformed && zeros.ordinals.size() == 1 && zeros.ordinals[0] == 0);

    auto lower = parse_gpu_device_selection("rocm1");
    check("the vendor prefix is case-insensitive",
          !lower.malformed && lower.vendor == GpuVendor::AMD && lower.ordinals[0] == 1);

    const auto amd = two_amd_dgpus();
    const std::vector<GpuMemoryCandidate> none;
    check("a repeated ordinal resolves to that one device",
          approx_eq(select_gpu_memory_pool(amd, none, "rocm-stable", "ROCm0,ROCm0").free_gb(), 20.7));
}

// The headroom figure is only trustworthy when it came from the device the model
// lands on; every other combination has to announce itself.
static void test_ctx_scope_warnings() {
    CtxMemoryScope scoped;
    scoped.is_gpu = true;
    scoped.per_device = true;
    scoped.device_named = true;
    check("a device-scoped reading warns about nothing",
          classify_ctx_scope(scoped) == CtxScopeWarning::None);

    CtxMemoryScope unscoped;
    unscoped.is_gpu = true;
    unscoped.per_device = false;
    unscoped.device_named = true;
    check("a named device that could not be scoped warns",
          classify_ctx_scope(unscoped) == CtxScopeWarning::Unscoped);

    CtxMemoryScope ambiguous;
    ambiguous.is_gpu = true;
    ambiguous.per_device = true;
    ambiguous.ambiguous = true;
    check("an unnamed multi-GPU target warns as ambiguous",
          classify_ctx_scope(ambiguous) == CtxScopeWarning::Ambiguous);

    // The aggregate path is the pre-existing behavior on CPU/NPU, not a degradation.
    CtxMemoryScope cpu;
    check("a CPU/NPU placement is silent", classify_ctx_scope(cpu) == CtxScopeWarning::None);

    CtxMemoryScope unnamed_fallback;
    unnamed_fallback.is_gpu = true;
    unnamed_fallback.per_device = false;
    check("an unnamed GPU target on the aggregate path stays silent",
          classify_ctx_scope(unnamed_fallback) == CtxScopeWarning::None);
}

static void test_nvidia_smi_row_parsing() {
    auto row = parse_nvidia_smi_row("0, GPU-abc123, NVIDIA GeForce RTX 3090, 8.6, 550.54.14, 24576, 2048", 99);
    check("a well-formed row is valid", row.valid);
    check("the row keeps its own index", row.index == 0);
    check("the uuid is extracted", row.uuid == "GPU-abc123");
    check("the name is extracted", row.name == "NVIDIA GeForce RTX 3090");
    check("total VRAM converts MiB to GB", approx_eq(row.vram_gb, 24.0));
    check("used VRAM converts MiB to GB", approx_eq(row.vram_used_gb, 2.0));

    // Regression guard: peeling four fields from the right must not eat a comma
    // that belongs to the GPU name.
    auto comma_name = parse_nvidia_smi_row("1, GPU-xyz, NVIDIA T400, 4GB, 7.5, 535.183.01, 4096, 512", 99);
    check("a comma inside the GPU name survives parsing",
          comma_name.valid && comma_name.name == "NVIDIA T400, 4GB");
    check("a comma-named GPU still reports used VRAM", approx_eq(comma_name.vram_used_gb, 0.5));

    // Drivers that cannot report a field emit "[Not Supported]"; treating that as 0
    // would present a busy card as empty.
    auto unsupported = parse_nvidia_smi_row("0, GPU-abc, Tesla K80, 3.7, 470.256.02, 11441, [Not Supported]", 99);
    check("an unsupported used-memory field leaves the unknown sentinel",
          unsupported.valid && unsupported.vram_used_gb < 0);
    check("an unsupported used-memory field still yields total VRAM",
          approx_eq(unsupported.vram_gb, 11.173, 0.01));

    check("a truncated row is rejected",
          !parse_nvidia_smi_row("0, GPU-abc, Tesla K80", 99).valid);
    check("an empty line is rejected", !parse_nvidia_smi_row("", 99).valid);

    auto no_index = parse_nvidia_smi_row("notanumber, GPU-abc, Card, 8.6, 550.54.14, 24576, 2048", 7);
    check("an unparseable index falls back to the caller's ordinal",
          no_index.valid && no_index.index == 7);
}

static void test_no_gpus_is_unresolved() {
    const std::vector<GpuMemoryCandidate> none;
    check("a host with no GPUs is unresolved",
          !select_gpu_memory_pool(none, none, "rocm-stable", "").resolved);
    check("a named device on a host with no GPUs is unresolved",
          !select_gpu_memory_pool(none, none, "rocm-stable", "ROCm0").resolved);
}

int main() {
    test_parse_single_device();
    test_parse_device_list();
    test_parse_rejects_malformed();
    test_selects_named_device();
    test_ordinal_beats_vector_position();
    test_unknown_ordinal_is_unresolved();
    test_device_list_takes_most_constrained();
    test_vulkan_is_unresolved();
    test_backend_implies_vendor();
    test_unnamed_multi_gpu_is_ambiguous();
    test_missing_usage_is_unresolved();
    test_no_gpus_is_unresolved();
    test_parse_hostile_inputs();
    test_ctx_scope_warnings();
    test_nvidia_smi_row_parsing();

    if (g_failures == 0) {
        std::printf("\nAll GPU device memory tests passed.\n");
    } else {
        std::printf("\n%d test(s) FAILED.\n", g_failures);
    }
    return g_failures;
}
