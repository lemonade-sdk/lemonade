#include "lemon/backends/strata/strata_server.h"

#include <cstdio>
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
    strata::validate_options(8192, "--mtp=0 --chunk 512 --no-graph", 4, true);
    strata::validate_options(262144, "--mtp custom-pack", 1, false);
    return failures ? 1 : 0;
}
