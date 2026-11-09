#include "lemon/backends/strata/strata.h"
#include "lemon/backends/strata/strata_server.h"
#include "lemon/system_info.h"
#include "lemon/utils/custom_args.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
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
    check("Windows RDNA4 archive",
          strata::engine_asset("windows-x86_64", "rocm", "gfx1201") ==
              "strata-engine-*-windows-x86_64-hip-rx9070.tar.gz");
    check("Windows Halo uses RDNA3 archive",
          strata::engine_asset("windows-x86_64", "rocm", "gfx1151") ==
              "strata-engine-*-windows-x86_64-hip-rdna3.tar.gz");
    check("Linux Halo has its own archive",
          strata::engine_asset("linux-x86_64", "rocm", "gfx1151") ==
              "strata-engine-*-linux-x86_64-hip-halo.tar.gz");
    check("Linux RDNA3 archive",
          strata::engine_asset("linux-x86_64", "rocm", "gfx1101") ==
              "strata-engine-*-linux-x86_64-hip-rx7900.tar.gz");
    check("Windows name detection selects the qualified RDNA4 archive",
          strata::engine_asset("windows-x86_64", "rocm",
                              strata::ops()->device_family("gfx120X", "AMD Radeon RX 9070 XT")) ==
              "strata-engine-*-windows-x86_64-hip-rx9070.tar.gz");
    for (const auto& device : {
             std::pair{"AMD Radeon RX 7900 XTX", "gfx1100"},
             std::pair{"AMD Radeon RX 7800 XT", "gfx1101"},
             std::pair{"AMD Radeon RX 7600 XT", "gfx1102"},
             std::pair{"AMD Radeon RX 9060 XT", "gfx1200"},
             std::pair{"AMD Radeon AI PRO R9700", "gfx1201"}}) {
        const auto family = lemon::identify_rocm_arch_from_name(device.first);
        check("Strata refines the detected family to a supported architecture",
              strata::ops()->device_family(family, device.first) == device.second);
        for (const auto& registration : lemon::backends::all_registrations()) {
            if (registration.descriptor->recipe == "strata") continue;
            check("Existing backend preserves its detected GPU family",
                  registration.ops->device_family(family, device.first) == family);
            check("Existing installer does not opt into auxiliary archives",
                  !registration.spec || !registration.spec->additional_assets_fn);
        }
    }
    check("Reject unqualified RDNA3 integrated GPUs", rejects([] {
        strata::engine_asset("linux-x86_64", "rocm", "gfx1103");
    }));
    check("Reject an unresolved architecture family", rejects([] {
        strata::engine_asset("windows-x86_64", "rocm", "gfx110X");
    }));
    check("Preserve unresolved marketing-name fallback",
          strata::ops()->device_family("gfx110X", "AMD Radeon PRO W7800") == "gfx110X");
    check("Shared RDNA3 detection is unchanged",
          lemon::identify_rocm_arch_from_name("AMD Radeon RX 7900 XTX") == "gfx110X");
    check("Shared RDNA4 detection is unchanged",
          lemon::identify_rocm_arch_from_name("AMD Radeon RX 9070 XT") == "gfx120X");
    check("Strata does not reinterpret an unsupported concrete architecture",
          strata::ops()->device_family("gfx1103", "AMD Radeon 780M") == "gfx1103");
    check("CUDA release archive does not hardcode a component version",
          strata::engine_asset("linux-x86_64", "cuda", "") ==
              "strata-engine-*-linux-x86_64-cuda-cuda13.tar.gz");
    check("Reject unsupported architectures", rejects([] {
        strata::engine_asset("linux-x86_64", "rocm", "gfx942");
    }));
    check("Reject experimental RDNA2", rejects([] {
        strata::engine_asset("windows-x86_64", "rocm", "gfx1030");
    }));
    check("Reject Metal for Qwen3.6", rejects([] {
        strata::engine_asset("macos-arm64", "metal", "");
    }));
    check("Reject unsupported variants", rejects([] {
        strata::engine_asset("linux-x86_64", "sycl", "");
    }));
    for (const std::string quant : {"UD-Q4_K_XL", "UD-Q6_K_XL", "Q8_0"}) {
        check("Accept shared upstream GGUF",
              strata::supported_checkpoint("unsloth/Qwen3.6-35B-A3B-MTP-GGUF:Qwen3.6-35B-A3B-" + quant + ".gguf"));
    }
    check("Reject unqualified quantization",
          !strata::supported_checkpoint("repo/model:Qwen3.6-35B-A3B-Q2_K.gguf"));
    check("Reject a different family",
          !strata::supported_checkpoint("repo/model:Qwen3.5-35B-A3B-UD-Q4_K_XL.gguf"));
    check("Reject suffix matching another filename",
          !strata::supported_checkpoint("NotQwen3.6-35B-A3B-Q8_0.gguf"));
    const auto config = strata::build_config("shared-model", "shared.gguf", "engine",
                                             "tokenizer", "rocm", 4096);
    check("Engine opens the original shared GGUF",
          config.at("args") == lemon::json::array(
              {"--native", "shared.gguf", "--max-context", "4096",
               "--prefill", "auto", "--mtp", "1"}));
    check("Correct engine architecture and protocol",
          config.at("arch") == "qwen35moe" && config.at("backend") == "hip");
    check("No exposed upstream listener or desktop launch",
          config.at("host") == "127.0.0.1" && config.at("open_browser") == false);
    check("Forwarded model name is accepted", config.at("model_name") == "shared-model");
    check("Reject unresolved auto context", rejects([] {
        strata::build_config("m", "m.gguf", "engine", "tokenizer", "cuda", -1);
    }));
    const auto tuned = strata::build_config("shared-model", "shared.gguf", "engine", "tokenizer",
                                            "cuda", 8192,
                                            "--mtp=0 --chunk 512 --no-graph --batch-mtp --gpu 1 "
                                            "--expert-vram 8 --expert-slots 256 --cpu-threads 4 "
                                            "--hybrid-rows 128 --draft-vocab 1000 --no-warmup", 4);
    const auto args = tuned.at("args").get<std::vector<std::string>>();
    const auto arg_map = lemon::utils::build_custom_args_map(args);
    check("Parallel slots reach the Python server", tuned.at("parallel") == 4);
    check("Explicit zero disables MTP", arg_map.at("--mtp")[0][0] == "0");
    check("Quoted engine options preserve their values",
          strata::build_config("m", "m.gguf", "engine", "tokenizer", "cuda", 4096,
                               "--mtp \"0\" --prefill 'auto'").at("args") ==
              lemon::json::array({"--native", "m.gguf", "--max-context", "4096",
                                 "--mtp", "0", "--prefill", "auto"}));
    check("Prefill alias suppresses the auto default",
          !arg_map.count("--prefill") && arg_map.at("--chunk")[0][0] == "512");
    check("Engine tuning and toggles are forwarded",
          arg_map.at("--expert-vram")[0][0] == "8" && arg_map.at("--no-warmup")[0].empty());
    for (const std::string invalid : {
             "--model other.gguf", "--native=other.gguf", "--ctx 256", "--max-context=512",
             "--serve", "--batch 8", "--slots=2", "--vision", "--tokens 1",
             "--mtp 8", "--mtp nope", "--mtp -1", "--mtp", "--prefill 4097", "--chunk -1"}) {
        check("Reject managed modes and invalid native tuning", rejects([&] {
            strata::build_config("m", "m.gguf", "engine", "tokenizer", "cuda", 4096, invalid);
        }));
    }
    for (const int parallel : {0, 9}) {
        check("Reject invalid parallel slot counts", rejects([&] {
            strata::build_config("m", "m.gguf", "engine", "tokenizer", "cuda", 4096, "", parallel);
        }));
    }
    check("Reject contexts below the engine minimum", rejects([] {
        strata::build_config("m", "m.gguf", "engine", "tokenizer", "cuda", 128);
    }));
    lemon::ModelInfo unsupported;
    unsupported.checkpoints["main"] = "repo/model:other.gguf";
    check("Model admission rejects unsupported GGUFs",
          !strata::ops()->validate_load(unsupported).empty());
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("strata-contract-" + std::to_string(std::random_device{}()));
    const lemon::backends::BackendAsset asset{
        "control.zip", "control", {"python", "serve/server.py", "tools/strata_tokenizer.py"}};
    fs::create_directories(root / "control" / "serve");
    fs::create_directories(root / "control" / "tools");
    std::ofstream(root / "control" / "python") << "python";
    check("Bundled interpreter alone is not a complete installation", !asset.is_complete(root.string()));
    std::ofstream(root / "control" / "serve" / "server.py") << "server";
    check("Missing tokenizer requires installation repair", !asset.is_complete(root.string()));
    std::ofstream(root / "control" / "tools" / "strata_tokenizer.py") << "tokenizer";
    check("Complete auxiliary archive is accepted", asset.is_complete(root.string()));
    fs::remove(root / "control" / "serve" / "server.py");
    check("Missing server requires installation repair", !asset.is_complete(root.string()));
    fs::remove_all(root);
    return failures ? 1 : 0;
}
