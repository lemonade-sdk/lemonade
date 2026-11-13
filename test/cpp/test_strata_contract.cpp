#include "lemon/backends/strata/strata_server.h"

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
    check("Windows name detection selects the qualified RDNA4 archive",
          strata::engine_asset("windows-x86_64", "rocm",
                              strata::ops()->device_family("gfx120X", "AMD Radeon RX 9070 XT")) ==
              "strata-engine-*-windows-x86_64-hip-rx9070.tar.gz");
    const std::string flash_checkpoint =
        "ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF:Q2_0/"
        "Qwen3.8-Flash-Next-GSQ-RCO-Q2_0-00001-of-00002.gguf";
    check("Flash-Next uses the Qwen engine catalog",
          strata::checkpoint_selection(flash_checkpoint).at("family") == "qwen");
    check("Sharded registrations require the complete catalog entry",
          !strata::ops()->validate_registration_checkpoint(flash_checkpoint).empty());
    lemon::ModelInfo flash;
    flash.checkpoints["main"] = flash_checkpoint;
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
