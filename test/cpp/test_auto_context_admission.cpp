#include <lemon/auto_context_budget.h>
#include <cstdio>

// Release (NDEBUG) must execute the assertions too.
#define CHECK(expr) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr); \
        ++failures; \
    } \
} while (false)
using namespace lemon;
int main() {
    int failures = 0;
    CHECK(auto_context_cpu_offload_weights_fit(4, 100, 70, false, false, true));
    CHECK(!auto_context_cpu_offload_weights_fit(4, 20, 70, false, false, true));
    CHECK(!auto_context_cpu_offload_weights_fit(4, -1, 70, false, false, true));
    CHECK(auto_context_cpu_offload_weights_fit(64, 120, 70, false, true, true));
    CHECK(!auto_context_cpu_offload_weights_fit(64, 60, 70, false, true, true));
    CHECK(!auto_context_cpu_offload_weights_fit(64, 120, 70, false, true, false));
    CHECK(!auto_context_cpu_offload_weights_fit(0, 0, 70, false, false, true));
    // Direct weights fit, but the estimated KV cache holds <4096 tokens.
    CHECK(auto_context_minimum_admission(0, 4096, true, false, false, false) == 4096);
    CHECK(auto_context_minimum_admission(4095, 4096, true, false, false, false) == 4096);
    CHECK(auto_context_minimum_admission(4095, 4096, true, false, false, true) == 0);
    CHECK(auto_context_minimum_admission(0, 4096, false, false, false, false) == 0);
    CHECK(auto_context_minimum_admission(4096, 4096, true, false, false, true) == 4096);
    CHECK(auto_context_minimum_admission(8192, 4096, true, false, false, true) == 8192);
    CHECK(auto_context_minimum_admission(0, 4096, false, true, false, true) == 4096);
    CHECK(auto_context_minimum_admission(0, 4096, false, false, true, false) == 4096);
    CHECK(auto_context_minimum_admission(0, 4096, false, false, true, true) == 0);
    CHECK(auto_context_tokens_from_kv_gib(0.0, 1024.0) == 0);
    CHECK(auto_context_tokens_from_kv_gib(1.0, 1024.0 * 1024.0) == 1024);
    CHECK(auto_context_tokens_from_kv_gib(1.0e100, 1.0) == INT64_MAX);
    // An explicit backend KV budget is authoritative even if the model fits.
    CHECK(auto_context_minimum_admission(0, 4096, true, false, false, false, true) == 0);
    CHECK(auto_context_minimum_admission(4095, 4096, true, false, false, false, true) == 0);
    CHECK(auto_context_minimum_admission(8192, 4096, true, false, false, false, true) == 8192);
    // Normal mode retains the 4k best-effort estimate for heuristic reserves.
    CHECK(auto_context_minimum_admission(4095, 4096, true, false, false, false) == 4096);
    // If the model supports only 2048, 3000 estimated tokens are sufficient.
    CHECK(auto_context_minimum_admission(3000, 2048, true, false, false, true) == 3000);
    CHECK(auto_context_clamp_tokens(3000, 2048, 32768) == 2048);

    // Bounded pure SWA has zero remaining per-token KV slope after charging
    // its minimum window as fixed memory; do not account it a second time.
    CHECK(auto_context_cpu_offload_fallback(0, 64, 116, 63.4, 0.0, false, true, true) == 4096);
    CHECK(auto_context_cpu_offload_fallback(0, 64, 67, 63.4, 0.0, false, true, true) == 0);
    CHECK(auto_context_cpu_offload_fallback(0, 4, 100, 70, 0.0, false, false, true) == 4096);
    CHECK(auto_context_cpu_offload_fallback(0, 4, 100, 70, -1.0, false, false, true) == 0);

    // Only adaptive llama.cpp placement permits assumed CPU/GPU splitting.
    CHECK(auto_context_llamacpp_can_cpu_offload("", nullptr, nullptr));
    CHECK(auto_context_llamacpp_can_cpu_offload("--fit on --gpu-layers auto", nullptr, nullptr));
    CHECK(!auto_context_llamacpp_can_cpu_offload("--fit off", nullptr, nullptr));
    CHECK(!auto_context_llamacpp_can_cpu_offload("-fit=off", nullptr, nullptr));
    CHECK(!auto_context_llamacpp_can_cpu_offload("-ngl all", nullptr, nullptr));
    CHECK(!auto_context_llamacpp_can_cpu_offload("--gpu-layers=all", nullptr, nullptr));
    CHECK(!auto_context_llamacpp_can_cpu_offload("--n-gpu-layers 35", nullptr, nullptr));
    CHECK(!auto_context_llamacpp_can_cpu_offload("--fit --gpu-layers all", nullptr, nullptr));
    CHECK(auto_context_llamacpp_can_cpu_offload("--fit-target 1024 --no-mmap", nullptr, nullptr));
    CHECK(!auto_context_llamacpp_can_cpu_offload("", "off", nullptr));
    CHECK(!auto_context_llamacpp_can_cpu_offload("", nullptr, "all"));
    CHECK(!auto_context_llamacpp_can_cpu_offload("", nullptr, "35"));
    return failures ? 1 : 0;
}
