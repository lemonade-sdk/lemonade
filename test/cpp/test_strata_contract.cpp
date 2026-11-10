#include "lemon/backends/strata/strata_server.h"

#include <array>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>

namespace {
int failures = 0;

void check(const char* message, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failures;
}

bool rejects(const std::function<void()>& operation) {
    try {
        operation();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

}  // namespace

int main() {
    namespace strata = lemon::backends::strata;
    for (const auto& test : {
             std::array{"windows-x86_64", "rocm", "gfx1201", "hip-rx9070"},
             std::array{"windows-x86_64", "rocm", "gfx1151", "hip-rdna3"},
             std::array{"linux-x86_64", "rocm", "gfx1151", "hip-halo"},
             std::array{"linux-x86_64", "rocm", "gfx1101", "hip-rx7900"},
             std::array{"linux-x86_64", "cuda", "", "cuda-cuda13"},
             std::array{"macos-arm64", "metal", "", "metal"},
             std::array{"linux-x86_64", "sycl", "", "sycl"},
             std::array{"linux-aarch64", "cuda", "", "cuda-cuda13"}}) {
        const std::string expected = "strata-engine-*-" + std::string(test[0]) +
            "-" + test[3] + ".tar.gz";
        check(expected.c_str(), strata::engine_asset(test[0], test[1], test[2]) == expected);
    }
    for (const auto& test : {
             std::array{"linux-x86_64", "rocm", "gfx1103"},
             std::array{"windows-x86_64", "rocm", "gfx110X"},
             std::array{"linux-x86_64", "rocm", "gfx942"},
             std::array{"windows-x86_64", "rocm", "gfx1030"},
             std::array{"windows-x86_64", "sycl", ""}}) {
        const std::string label = "Reject unsupported " + std::string(test[0]) +
            "/" + test[1] + "/" + test[2];
        check(label.c_str(), rejects([&] { strata::engine_asset(test[0], test[1], test[2]); }));
    }
    check("Windows name detection selects the qualified RDNA4 archive",
          strata::engine_asset("windows-x86_64", "rocm",
                              strata::ops()->device_family("gfx120X", "AMD Radeon RX 9070 XT")) ==
              "strata-engine-*-windows-x86_64-hip-rx9070.tar.gz");
    for (const auto& backend : {"cuda", "rocm", "metal", "sycl"}) {
        const auto assets = strata::additional_assets(backend);
        check("Only Control accompanies the Qwen engine",
              assets.size() == 1 && assets.front().filename.find("strata-control-") == 0);
    }
    check("DeepSeek is not a Strata model",
          strata::checkpoint_selection("unsloth/DeepSeek-V4.1-Flash-1017-GGUF:UD-Q8_K_XL.gguf").empty());
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
    check("Other engines retain default revision handling",
          lemon::backends::ops_for("llamacpp")->checkpoint_revision(flash, "main").empty());
    for (const std::string quant : {"UD-Q4_K_XL", "UD-Q6_K_XL", "Q8_0"}) {
        check("Accept shared upstream GGUF",
              !strata::checkpoint_selection("unsloth/Qwen3.6-35B-A3B-MTP-GGUF:Qwen3.6-35B-A3B-" + quant + ".gguf").empty());
    }
    check("Reject unqualified quantization",
          strata::checkpoint_selection("repo/model:Qwen3.6-35B-A3B-Q2_K.gguf").empty());
    check("Reject a different family",
          strata::checkpoint_selection("repo/model:Qwen3.5-35B-A3B-UD-Q4_K_XL.gguf").empty());
    check("Reject suffix matching another filename",
          strata::checkpoint_selection("NotQwen3.6-35B-A3B-Q8_0.gguf").empty());
    strata::validate_options(8192, "--mtp=0 --chunk 512 --no-graph --batch-mtp --gpu 1 "
                                    "--expert-vram 8 --expert-slots 256 --cpu-threads 4 "
                                    "--hybrid-rows 128 --draft-vocab 1000 --no-warmup", 4, true);
    strata::validate_options(4096, "--mtp \"0\" --prefill 'auto'", 1, true);
    strata::validate_options(262144, "--mtp custom-pack", 1, false);
    for (const std::string invalid : {
             "--model other.gguf", "--native=other.gguf", "--ctx 256", "--max-context=512",
             "--pack other", "--serve", "--batch 8", "--slots=2", "--vision", "--tokens 1",
             "--mtp 8", "--mtp nope", "--mtp -1", "--mtp", "--prefill 4097", "--chunk -1"}) {
        check("Reject managed modes and invalid native tuning", rejects([&] {
            strata::validate_options(4096, invalid, 1, true);
        }));
    }
    for (const int parallel : {0, 9}) {
        check("Reject invalid parallel slot counts", rejects([&] {
            strata::validate_options(4096, "", parallel, true);
        }));
    }
    for (const int context : {-1, 128}) {
        check("Reject contexts below the engine minimum", rejects([&] {
            strata::validate_options(context, "", 1, true);
        }));
    }
    lemon::ModelInfo unsupported;
    unsupported.checkpoints["main"] = "repo/model:other.gguf";
    check("Model admission rejects unsupported GGUFs",
          !strata::ops()->validate_load(unsupported).empty());
    return failures ? 1 : 0;
}
