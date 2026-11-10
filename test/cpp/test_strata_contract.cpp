#include "lemon/backends/strata/strata.h"
#include "lemon/backends/strata/strata_server.h"
#include "lemon/system_info.h"
#include "lemon/utils/custom_args.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

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

class FixtureStrataServer : public lemon::backends::StrataServer {
public:
    explicit FixtureStrataServer(int port) : StrataServer("error", nullptr, nullptr) {
        port_ = port;
    }

    bool is_backend_alive() const override { return true; }
};

void test_slots() {
    check("Strata implements the router's slots interface",
          std::is_base_of<lemon::ISlotsServer, lemon::backends::StrataServer>::value);
    httplib::Server backend;
    lemon::json slots = lemon::json::array({
        {{"id", 0}, {"n_ctx", 262144}, {"is_processing", false}, {"n_prompt_tokens", 64}},
        {{"id", 1}, {"n_ctx", 262144}, {"is_processing", true}, {"n_prompt_tokens", 512}},
    });
    lemon::json metrics = {{"live", {{"slots", lemon::json::array({
        {{"slot", 1}, {"state", "decoding"}, {"generated", 37}},
        {{"slot", 0}, {"state", "idle"}, {"held_tokens", 64}},
    })}}}};
    std::mutex telemetry_mutex;
    int metrics_status = 200;
    backend.Get("/slots", [&](const httplib::Request&, httplib::Response& response) {
        std::lock_guard<std::mutex> lock(telemetry_mutex);
        response.set_content(slots.dump(), "application/json");
    });
    backend.Get("/metrics", [&](const httplib::Request&, httplib::Response& response) {
        std::lock_guard<std::mutex> lock(telemetry_mutex);
        response.status = metrics_status;
        response.set_content(metrics.dump(), "application/json");
    });
    std::atomic<bool> action_query_ok{false};
    std::atomic<bool> action_body_ok{false};
    backend.Post("/slots/7", [&](const httplib::Request& request, httplib::Response& response) {
        action_query_ok = request.get_param_value("action") == "restore";
        action_body_ok = lemon::json::parse(request.body) == lemon::json{{"filename", "session.bin"}};
        response.status = 501;
        response.set_content(
            R"({"error":{"message":"slot save/restore is disabled","type":"invalid_request_error"}})",
            "application/json");
    });
    const int port = backend.bind_to_any_port("127.0.0.1");
    check("Slot fixture binds loopback", port > 0);
    if (port <= 0) return;
    std::thread listener([&] { backend.listen_after_bind(); });
    backend.wait_until_ready();
    {
        FixtureStrataServer server(port);
        auto expected = slots;
        expected[0]["state"] = "idle";
        expected[1]["state"] = "decoding";
        expected[1]["n_decoded"] = 37;
        check("Parallel counters match slot IDs without changing upstream KV token counts",
              server.get_slots() == expected);
        {
            std::lock_guard<std::mutex> lock(telemetry_mutex);
            slots = lemon::json::array({slots[0]});
            metrics = {{"live", {{"state", "reading"}, {"generated", 0},
                                 {"prompt_read", 8192}, {"prefill_tok_s_mean", 568.2}}}};
        }
        const auto prefill = server.get_slots();
        check("Solo prefill exposes the engine rate and a zero decoded counter",
              prefill[0]["is_processing"] == true && prefill[0]["n_decoded"] == 0 &&
              prefill[0]["timings"]["prompt_per_second"] == 568.2);
        check("Cached prefixes are not counted as newly processed prompt tokens",
              !prefill[0].contains("n_prompt_tokens_processed") && prefill[0]["n_prompt_tokens"] == 64);
        {
            std::lock_guard<std::mutex> lock(telemetry_mutex);
            metrics = {{"live", {{"state", "generating"}, {"generated", 73},
                                 {"prefill_tok_s_mean", 568.2}}}};
        }
        const auto decoding = server.get_slots();
        check("Solo decoding exposes generated tokens without stale prefill rates",
              decoding[0]["is_processing"] == true && decoding[0]["n_decoded"] == 73 &&
              !decoding[0].contains("timings"));
        {
            std::lock_guard<std::mutex> lock(telemetry_mutex);
            metrics = {{"live", {{"state", "idle"}, {"generated", nullptr},
                                 {"prefill_tok_s_mean", nullptr}}}};
        }
        const auto idle = server.get_slots();
        check("Idle slots retain KV counts without fabricated throughput",
              idle[0]["is_processing"] == false && idle[0]["n_prompt_tokens"] == 64 &&
              !idle[0].contains("n_decoded") && !idle[0].contains("timings"));
        {
            std::lock_guard<std::mutex> lock(telemetry_mutex);
            metrics_status = 503;
            metrics = {{"error", {{"message", "metrics unavailable"}}}};
        }
        check("Metrics failures remain visible errors", server.get_slots().contains("error"));
        {
            std::lock_guard<std::mutex> lock(telemetry_mutex);
            metrics_status = 200;
            metrics = {{"live", nullptr}};
        }
        check("Malformed live telemetry is rejected", server.get_slots().contains("error"));
        {
            std::lock_guard<std::mutex> lock(telemetry_mutex);
            metrics = {{"live", {{"slots", lemon::json::array()}}}};
        }
        check("Missing per-slot telemetry is not replaced with aggregate counters",
              server.get_slots().contains("error"));
        {
            std::lock_guard<std::mutex> lock(telemetry_mutex);
            slots = lemon::json::array();
        }
        check("An unloaded engine still returns an empty slot list",
              server.get_slots() == lemon::json::array());
        const auto response = server.slots_action(7, "restore", {{"filename", "session.bin"}});
        check("Slot action query is forwarded", action_query_ok);
        check("Slot action body is forwarded", action_body_ok);
        check("Unsupported upstream slot actions remain explicit errors",
              response.contains("error") &&
              response.dump().find("slot save/restore is disabled") != std::string::npos);
    }
    backend.stop();
    listener.join();
}
}  // namespace

int main() {
    test_slots();
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
    check("Metal Qwen engine archive",
          strata::engine_asset("macos-arm64", "metal", "") ==
              "strata-engine-*-macos-arm64-metal.tar.gz");
    check("Intel Qwen engine archive",
          strata::engine_asset("linux-x86_64", "sycl", "") ==
              "strata-engine-*-linux-x86_64-sycl.tar.gz");
    check("ARM64 Qwen engine archive",
          strata::engine_asset("linux-aarch64", "cuda", "") ==
              "strata-engine-*-linux-aarch64-cuda-cuda13.tar.gz");
    check("Intel is Linux-only", rejects([] {
        strata::engine_asset("windows-x86_64", "sycl", "");
    }));
    for (const auto& backend : {"cuda", "rocm", "metal", "sycl"}) {
        const auto assets = strata::additional_assets(backend);
        check("Only Control accompanies the Qwen engine",
              assets.size() == 1 && assets.front().filename.find("strata-control-") == 0);
    }
    check("DeepSeek is not a Strata model",
          !strata::supported_checkpoint("unsloth/DeepSeek-V4.1-Flash-1017-GGUF:UD-Q8_K_XL.gguf"));
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
              strata::supported_checkpoint("unsloth/Qwen3.6-35B-A3B-MTP-GGUF:Qwen3.6-35B-A3B-" + quant + ".gguf"));
    }
    check("Reject unqualified quantization",
          !strata::supported_checkpoint("repo/model:Qwen3.6-35B-A3B-Q2_K.gguf"));
    check("Reject a different family",
          !strata::supported_checkpoint("repo/model:Qwen3.5-35B-A3B-UD-Q4_K_XL.gguf"));
    check("Reject suffix matching another filename",
          !strata::supported_checkpoint("NotQwen3.6-35B-A3B-Q8_0.gguf"));
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
