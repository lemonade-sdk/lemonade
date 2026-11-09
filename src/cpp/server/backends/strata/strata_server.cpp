#include "lemon/backends/strata/strata_server.h"

#include "lemon/backend_manager.h"
#include "lemon/backends/llamacpp/llamacpp_server.h"
#include "lemon/backends/strata/strata.h"
#include "lemon/gguf_reader.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/recipe_arg_resolver.h"

#include <lemon/utils/aixlog.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <random>
#include <regex>
#include <stdexcept>

namespace fs = std::filesystem;

namespace lemon {
namespace backends {
namespace {

std::string platform() {
#if defined(_WIN32) && defined(_M_X64)
    return "windows-x86_64";
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x86_64";
#else
    throw std::runtime_error("Strata Qwen3.6 supports Windows/Linux x86-64 only");
#endif
}

std::string python_binary() {
#ifdef _WIN32
    return "python/python.exe";
#else
    return "python/bin/python3";
#endif
}

class StrataOps : public BackendOps {
public:
    std::string device_family(const std::string& family, const std::string& name) const override {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (family == "gfx120X") {
            if (lower.find("9060") != std::string::npos) return "gfx1200";
            if (lower.find("9070") != std::string::npos ||
                lower.find("r9700") != std::string::npos) return "gfx1201";
        }
        if (family == "gfx110X") {
            std::smatch match;
            if (std::regex_search(lower, match, std::regex(R"(rx\s*(7600|7700|7800|7900)(\s|$))"))) {
                if (match[1] == "7600") return "gfx1102";
                if (match[1] == "7900") return "gfx1100";
                return "gfx1101";
            }
        }
        return family;
    }

    void populate_metadata(ModelInfo& info, const BackendOpsContext& ctx) const override {
        llamacpp::ops()->populate_metadata(info, ctx);
    }

    std::string resolve_checkpoint_path(const ModelInfo& info,
                                        const CheckpointResolveContext& ctx) const override {
        return llamacpp::ops()->resolve_checkpoint_path(info, ctx);
    }

    std::string find_imported_checkpoint(const std::string& dir) const override {
        return llamacpp::ops()->find_imported_checkpoint(dir);
    }

    std::string validate_registration_checkpoint(const std::string& checkpoint) const override {
        const auto error = llamacpp::ops()->validate_registration_checkpoint(checkpoint);
        if (!error.empty()) return error;
        return strata::supported_checkpoint(checkpoint) ? "" :
            "Strata supports Qwen3.6-35B-A3B UD-Q4_K_XL, UD-Q6_K_XL and Q8_0 GGUFs only";
    }

    std::string validate_checkpoint_file(const std::string& path) const override {
        return llamacpp::ops()->validate_checkpoint_file(path);
    }

    std::string validate_load(const ModelInfo& info) const override {
        if (!strata::supported_checkpoint(info.checkpoint())) {
            return "Unsupported Strata checkpoint: " + info.checkpoint();
        }
        return "";
    }

    InstallCheck check_install(const std::string& backend, bool binary_found) const override {
        const fs::path control = fs::path(BackendUtils::get_install_directory("strata", backend)) / "control";
        const bool complete = binary_found && strata::additional_assets(backend).front().is_complete(
            utils::path_to_utf8(control.parent_path()));
        return {complete, complete ? "" : "Strata engine or bundled Python server is missing"};
    }
};

}  // namespace

namespace strata {

std::string engine_asset(const std::string& target, const std::string& backend,
                         const std::string& arch) {
    if (target != "windows-x86_64" && target != "linux-x86_64") {
        throw std::invalid_argument("Unsupported Strata Qwen3.6 platform: " + target);
    }
    std::string variant;
    if (backend == "cuda") {
        variant = "cuda-cuda13";
    } else if (backend == "rocm") {
        if (arch == "gfx1200" || arch == "gfx1201") variant = "hip-rx9070";
        else if (arch == "gfx1100" || arch == "gfx1101" || arch == "gfx1102") {
            variant = target == "windows-x86_64" ? "hip-rdna3" : "hip-rx7900";
        } else if (arch == "gfx1151") {
            variant = target == "windows-x86_64" ? "hip-rdna3" : "hip-halo";
        } else {
            throw std::invalid_argument("Unsupported Strata AMD GPU: " + arch);
        }
    } else {
        throw std::invalid_argument("Unsupported Strata backend: " + backend);
    }
    return "strata-engine-*-" + target + "-" + variant + ".tar.gz";
}

std::vector<BackendAsset> additional_assets(const std::string&) {
    const auto target = platform();
    return {{"strata-control-*-" + target +
             (target == "windows-x86_64" ? ".zip" : ".tar.gz"), "control",
             {python_binary(), "serve/server.py", "tools/strata_tokenizer.py"}}};
}

bool supported_checkpoint(const std::string& checkpoint) {
    for (const std::string quant : {"UD-Q4_K_XL", "UD-Q6_K_XL", "Q8_0"}) {
        const std::string filename = "Qwen3.6-35B-A3B-" + quant + ".gguf";
        if (checkpoint == filename ||
            (checkpoint.size() > filename.size() &&
             checkpoint.compare(checkpoint.size() - filename.size(), filename.size(), filename) == 0 &&
             std::string(":/\\").find(checkpoint[checkpoint.size() - filename.size() - 1]) != std::string::npos)) {
            return true;
        }
    }
    return false;
}

json build_config(const std::string& model_name, const std::string& gguf,
                  const std::string& executable, const std::string& tokenizer,
                  const std::string& backend, int ctx_size,
                  const std::string& custom_args, int parallel) {
    if (ctx_size < 256) throw std::invalid_argument("Strata context size must be at least 256");
    if (parallel < 1 || parallel > 8) {
        throw std::invalid_argument("Strata parallel slots must be between 1 and 8");
    }
    const auto error = utils::validate_custom_args(custom_args, {
        "--native", "--model", "--max-context", "--ctx", "--serve", "--batch", "--slots",
        "--tokens", "--force", "--emb", "--vision", "--bench-pp", "--bench-tg",
        "--max-new", "--n", "--greedy",
        "--dump-full-logits", "--help", "-h",
    });
    if (!error.empty()) throw std::invalid_argument(error);
    const auto custom_map = utils::build_custom_args_map(utils::parse_custom_args(custom_args));
    for (const auto& [flag, occurrences] : custom_map) {
        if (flag != "--mtp" && flag != "--prefill" && flag != "--chunk") continue;
        const int maximum = flag == "--mtp" ? 7 : 4096;
        for (const auto& values : occurrences) {
            if (values.size() == 1 && flag != "--mtp" && values[0] == "auto") continue;
            if (values.size() != 1 || values[0].empty() ||
                values[0].find_first_not_of("0123456789") != std::string::npos ||
                values[0].size() > 4 || std::stoi(values[0]) > maximum) {
                throw std::invalid_argument("Strata " + flag + " must be between 0 and " +
                                            std::to_string(maximum) +
                                            (flag == "--mtp" ? "" : " (or auto)"));
            }
        }
    }
    auto args = std::vector<std::string>{"--native", gguf, "--max-context", std::to_string(ctx_size)};
    const auto extra = utils::parse_custom_args(utils::append_runtime_arg_defaults(
        utils::map_to_args_string(utils::build_custom_args_map(utils::parse_custom_args(custom_args, true))),
        {{"--prefill auto", "--prefill", {"--chunk"}}, {"--mtp 2", "--mtp"}}));
    args.insert(args.end(), extra.begin(), extra.end());
    const fs::path engine_dir = utils::path_from_utf8(executable).parent_path();
    return {
        {"exe", executable},
        {"args", args},
        {"cwd", utils::path_to_utf8(engine_dir)},
        {"tokenizer", tokenizer},
        {"log", utils::path_to_utf8(utils::path_from_utf8(tokenizer).parent_path() / "engine.log")},
        {"model_name", model_name},
        {"arch", "qwen35moe"},
        {"backend", backend == "rocm" ? "hip" : "cuda"},
        {"lib_dirs", {utils::path_to_utf8(engine_dir),
                      utils::path_to_utf8(engine_dir / "rocm" / "bin")}},
        {"host", "127.0.0.1"},
        {"open_browser", false},
        {"parallel", parallel},
    };
}

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<StrataServer>(ctx);
}

const BackendSpec* spec() {
    static const BackendSpec spec = [] {
        BackendSpec result(descriptor.recipe, descriptor.binary, StrataServer::get_install_params);
        result.additional_assets_fn = additional_assets;
        return result;
    }();
    return &spec;
}

const BackendOps* ops() { return single_ops<StrataOps>(); }

}  // namespace strata

StrataServer::StrataServer(const std::string& log_level, ModelManager* model_manager,
                           BackendManager* backend_manager)
    : WrappedServer("strata", log_level, model_manager, backend_manager) {}

StrataServer::~StrataServer() { unload(); }

InstallParams StrataServer::get_install_params(const std::string& backend, const std::string&) {
    std::string arch;
    if (backend == "rocm") {
        arch = SystemInfo::get_rocm_arch();
        if (arch == "gfx110X" || arch == "gfx120X") {
            auto devices = SystemInfoCache::get_system_info_with_cache().at("devices").at("amd_gpu");
            for (auto& gpu : devices) {
                gpu["family"] = strata::ops()->device_family(
                    gpu.value("family", ""), gpu.value("name", ""));
            }
            arch = SystemInfo::select_rocm_arch(devices);
        }
    }
    return {"Niko1221/Strata", strata::engine_asset(platform(), backend, arch), ""};
}

void StrataServer::load(const std::string& model_name, const ModelInfo& model_info,
                        const RecipeOptions& options, bool /*do_not_upgrade*/) {
    const auto error = strata::ops()->validate_load(model_info);
    if (!error.empty()) throw std::invalid_argument(error);
    GgufMetadata metadata;
    const std::string gguf = model_info.resolved_path();
    read_gguf_metadata(metadata, gguf);
    if (metadata.architecture != "qwen35moe") {
        throw std::invalid_argument("Strata requires a Qwen3.6 qwen35moe GGUF");
    }

    std::string backend = options.get_option("strata_backend");
    if (backend.empty() || backend == "auto") {
        const auto supported = SystemInfo::get_supported_backends("strata");
        if (supported.backends.empty()) {
            throw std::runtime_error("No supported Strata GPU: " + supported.not_supported_error);
        }
        backend = supported.backends.front();
    }
    RuntimeConfig::validate_backend_choice("strata", backend);
    backend_manager_->install_backend("strata", backend);
    const std::string executable = BackendUtils::get_backend_binary_path(*strata::spec(), backend);
    const fs::path install = utils::path_from_utf8(BackendUtils::get_install_directory("strata", backend));
    const fs::path control = install / "control";
    const std::string python = utils::path_to_utf8(control / python_binary());
    if (!fs::is_regular_file(utils::path_from_utf8(python))) {
        throw std::runtime_error("Strata's bundled Python is missing");
    }

    unload();
    const fs::path runtime = utils::path_from_utf8(utils::get_runtime_dir()) /
        ("strata-" + std::to_string(get_instance_id()) + "-" +
         std::to_string(std::random_device{}()));
    if (!fs::create_directory(runtime)) {
        throw std::runtime_error("Cannot create Strata runtime directory");
    }
    runtime_dir_ = utils::path_to_utf8(runtime);
    const int result = utils::ProcessManager::run_process_with_output(
        python, {"-E", "-s", utils::path_to_utf8(control / "tools" / "strata_tokenizer.py"),
                 "--gguf", gguf, "--out", runtime_dir_},
        [](const std::string& line) { LOG(INFO, "Strata") << line << std::endl; return true; },
        control.string(), 120);
    if (result != 0) throw std::runtime_error("Strata tokenizer extraction failed: " + std::to_string(result));

    const fs::path config_path = utils::path_from_utf8(runtime_dir_) / "config.json";
    const int ctx_size = options.get_option("ctx_size");
    utils::JsonUtils::save_to_file(
        strata::build_config(model_name, gguf, executable,
                             utils::path_to_utf8(utils::path_from_utf8(runtime_dir_) / "tokenizer"),
                             backend, ctx_size, options.get_option("strata_args"),
                             options.get_option("strata_parallel")), config_path.string());
    ServerCommand command;
    command.program = python;
    command.port = choose_port();
    command.args = {"-E", "-s", utils::path_to_utf8(control / "serve" / "server.py"),
                    "--engine", "strata", "--config", config_path.string(),
                    "--host", "127.0.0.1", "--port", std::to_string(command.port),
                    "--no-api-key", "--offline"};
    start_server(std::make_unique<NativeProcess>(
        ProcessOutput{log_level_ == "info" || is_debug(), true}, control.string()), command);
}

void StrataServer::unload() {
    stop_server();
    if (!runtime_dir_.empty()) {
        std::error_code ec;
        fs::remove_all(utils::path_from_utf8(runtime_dir_), ec);
        if (ec) LOG(WARNING, "Strata") << "Cannot remove runtime directory: " << ec.message() << std::endl;
        runtime_dir_.clear();
    }
}

json StrataServer::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", request);
}

json StrataServer::responses(const json& request) {
    return forward_request("/v1/responses", request);
}

}  // namespace backends
}  // namespace lemon
