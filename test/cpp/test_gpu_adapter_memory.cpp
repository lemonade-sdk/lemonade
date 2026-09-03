// Standalone test for single-physical-GPU dedicated VRAM attribution.
//
// Compile: g++ -std=c++17 -I src/cpp/include test/cpp/test_gpu_adapter_memory.cpp -o test_gpu_adapter_memory

#include "lemon/gpu_adapter_memory.h"
#include <cmath>
#include <cstdio>
#include <vector>

using lemon::GpuAdapterMemory;
using lemon::single_physical_gpu_dedicated_gb;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static bool approx_eq(double a, double b, double tol = 0.001) {
    return std::fabs(a - b) < tol;
}

static constexpr uint64_t GIB = 1024ull * 1024ull * 1024ull;

static void test_single_adapter() {
    std::vector<GpuAdapterMemory> adapters{{2 * GIB}};
    check("single adapter reports its dedicated usage",
          approx_eq(single_physical_gpu_dedicated_gb(1, adapters), 2.0));
}

static void test_software_adapter_is_ignored() {
    std::vector<GpuAdapterMemory> adapters{{0}, {1126723584}};
    check("largest row wins over an idle software adapter",
          approx_eq(single_physical_gpu_dedicated_gb(1, adapters), 1.049, 0.001));
}

static void test_rows_are_not_summed() {
    std::vector<GpuAdapterMemory> adapters{{3 * GIB}, {1 * GIB}};
    check("rows are not summed",
          approx_eq(single_physical_gpu_dedicated_gb(1, adapters), 3.0));
}

static void test_multi_gpu_is_unattributable() {
    std::vector<GpuAdapterMemory> adapters{{2 * GIB}, {4 * GIB}};
    check("two physical GPUs report no usage",
          single_physical_gpu_dedicated_gb(2, adapters) < 0.0);
}

static void test_no_adapters() {
    std::vector<GpuAdapterMemory> adapters;
    check("no adapter rows report no usage",
          single_physical_gpu_dedicated_gb(1, adapters) < 0.0);
}

static void test_idle_gpu_reports_zero() {
    std::vector<GpuAdapterMemory> adapters{{0}};
    check("an idle GPU reports zero, not unavailable",
          approx_eq(single_physical_gpu_dedicated_gb(1, adapters), 0.0));
}

int main() {
    test_single_adapter();
    test_software_adapter_is_ignored();
    test_rows_are_not_summed();
    test_multi_gpu_is_unattributable();
    test_no_adapters();
    test_idle_gpu_reports_zero();

    if (g_failures == 0) {
        std::printf("\nAll gpu_adapter_memory tests passed\n");
        return 0;
    }
    std::printf("\n%d gpu_adapter_memory test(s) FAILED\n", g_failures);
    return 1;
}
