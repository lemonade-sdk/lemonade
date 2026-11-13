#include "lemon/backends/strata/strata_server.h"
#include "lemon/models/store_builder.h"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
int failures = 0;

void check(const char* message, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failures;
}

}  // namespace

int main() {
    namespace strata = lemon::backends::strata;
#if defined(_WIN32) || (defined(__linux__) && defined(__aarch64__))
    const std::string unsupported_backend = "sycl";
#elif defined(__APPLE__)
    const std::string unsupported_backend = "cuda";
#else
    const std::string unsupported_backend = "metal";
#endif
    const auto install = strata::ops()->check_install(unsupported_backend, true);
    const auto state = strata::ops()->classify_unavailable(unsupported_backend, install.error, "install");
    check("Unsupported Strata platforms remain unavailable even with a binary on disk",
          !install.installed && state && state->state == "unsupported" && !state->message.empty());
    const lemon::json release_assets = {
        {"strata-engine-9.8.7-windows-x86_64-hip-rx9070.tar.gz", "sha256:" + std::string(64, 'a')}};
    check("Windows name detection selects the qualified RDNA4 archive",
          strata::engine_asset("windows-x86_64", "rocm",
                              strata::ops()->device_family("gfx120X", "AMD Radeon RX 9070 XT"),
                              release_assets) ==
              "strata-engine-9.8.7-windows-x86_64-hip-rx9070.tar.gz");
    try {
        strata::engine_asset("windows-x86_64", "cuda", "", release_assets);
        check("Missing Strata release assets fail explicitly", false);
    } catch (const std::invalid_argument&) {
        check("Missing Strata release assets fail explicitly", true);
    }
    auto ambiguous_assets = release_assets;
    ambiguous_assets["strata-engine-9.8.8-windows-x86_64-hip-rx9070.tar.gz"] = "sha256:" + std::string(64, 'b');
    try {
        strata::engine_asset("windows-x86_64", "rocm", "gfx1201", ambiguous_assets);
        check("Ambiguous Strata release assets fail explicitly", false);
    } catch (const std::invalid_argument&) {
        check("Ambiguous Strata release assets fail explicitly", true);
    }
    const std::string flash_checkpoint =
        "ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF:Q2_0/"
        "Qwen3.8-Flash-Next-GSQ-RCO-Q2_0-00001-of-00002.gguf";
    check("Flash-Next uses the Qwen engine catalog",
          strata::checkpoint_selection(flash_checkpoint).at("family") == "qwen");
    check("Sharded registrations require the complete catalog entry",
          !strata::ops()->validate_registration_checkpoint(flash_checkpoint).empty());
    lemon::ModelInfo flash;
    flash.checkpoints["main"] = flash_checkpoint;
    const lemon::json discrete_gpu = {{"available", true}, {"integrated", false}};
    const lemon::json apu = {{"available", true}, {"integrated", true}};
    const lemon::json discrete_devices = {{"amd_gpu", lemon::json::array({discrete_gpu})}};
    const double discrete_pool = strata::streaming_memory_pool_gb(discrete_devices, 128.0, 24.0);
    check("A 24 GB discrete GPU cannot fit a 118 GB resident model using host RAM",
          discrete_pool == 24.0 &&
          lemon::models::streaming_model_exceeds_pool(
              lemon::models::streaming_working_set_gb(0.0, 118.0), discrete_pool));
    check("Discrete GPUs preserve the caller's GTT opt-in pool",
          strata::streaming_memory_pool_gb(discrete_devices, 128.0, 120.0) == 120.0);
    const double apu_pool = strata::streaming_memory_pool_gb(
        {{"amd_gpu", lemon::json::array({apu})}}, 128.0, 24.0);
    check("Integrated GPUs retain system RAM without architecture identifiers",
          apu_pool == 128.0 && !lemon::models::streaming_model_exceeds_pool(118.0, apu_pool));
    check("Hybrid hosts prefer discrete GPUs regardless of detection order",
          strata::streaming_memory_pool_gb(
              {{"amd_gpu", lemon::json::array({apu, discrete_gpu})}}, 128.0, 24.0) == 24.0 &&
          strata::streaming_memory_pool_gb(
              {{"amd_gpu", lemon::json::array({discrete_gpu, apu})}}, 128.0, 24.0) == 24.0);
    check("Unavailable or unidentified integrated GPUs do not expose host RAM",
          strata::streaming_memory_pool_gb(
              {{"amd_gpu", lemon::json::array({{{"available", false}, {"integrated", true}}})}},
              128.0, 24.0) == 24.0 &&
          strata::streaming_memory_pool_gb(
              {{"amd_gpu", lemon::json::array({{{"available", true}}})}}, 128.0, 24.0) == 24.0);
    check("Apple Silicon retains its unified system RAM pool",
          strata::streaming_memory_pool_gb(
              {{"metal", {{"available", true}}}}, 128.0, 24.0) == 128.0);
    check("Strata downloads retain the upstream model revision",
          strata::ops()->checkpoint_revision(flash, "main").size() == 40 &&
          strata::ops()->checkpoint_revision(flash, "shard_2") ==
              strata::ops()->checkpoint_revision(flash, "main"));
    try {
        strata::validate_options(8192, "--mtp=0 --chunk 512 --no-graph", 4, true);
        check("GGUF options accept disabled MTP and parallel slots", true);
    } catch (const std::invalid_argument&) {
        check("GGUF options accept disabled MTP and parallel slots", false);
    }
    try {
        strata::validate_options(262144, "--mtp custom-pack", 1, false);
        check("Ready-pack options accept a custom MTP pack", true);
    } catch (const std::invalid_argument&) {
        check("Ready-pack options accept a custom MTP pack", false);
    }
    return failures ? 1 : 0;
}
