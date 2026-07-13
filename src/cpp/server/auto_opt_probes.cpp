#include "lemon/auto_opt_probes.h"
#include "lemon/auto_opt_engine.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/process_manager.h"

#include <lemon/utils/aixlog.hpp>

#include "lemon/utils/path_utils.h"

#include <chrono>
#include <filesystem>
#include <cstdlib>
#include <thread>

namespace lemon {
namespace autoopt {

using lemon::backends::BackendUtils;
using lemon::utils::HttpClient;
using lemon::utils::ProcessManager;

namespace {

std::map<std::string, std::string> hf_headers() {
    std::map<std::string, std::string> headers;
    const char* hf_token = std::getenv("HF_TOKEN");
    if (hf_token && hf_token[0]) {
        headers["Authorization"] = "Bearer " + std::string(hf_token);
    }
    return headers;
}

// "https://huggingface.co/Qwen/Qwen3-32B" -> "Qwen/Qwen3-32B"
std::string repo_id_from_url(const std::string& url_or_id) {
    const std::string marker = "huggingface.co/";
    auto pos = url_or_id.find(marker);
    std::string id = pos == std::string::npos ? url_or_id : url_or_id.substr(pos + marker.size());
    while (!id.empty() && id.back() == '/') id.pop_back();
    return id;
}

}  // namespace

std::string RealMeasurementProvider::tool_path(const std::string& backend,
                                               const std::string& tool) const {
    const auto* spec = lemon::backends::spec_for("llamacpp");
    if (!spec) return "";
    return BackendUtils::get_backend_tool_path(*spec, backend, tool);
}

std::vector<std::pair<std::string, std::string>> RealMeasurementProvider::tool_env(
    const std::string& backend, const std::string& exe) const {
    return BackendUtils::get_backend_env(backend, exe);
}

FitEstimate RealMeasurementProvider::fit_params(const std::string& backend,
                                                const std::string& gguf_path,
                                                const std::vector<std::string>& extra_args,
                                                int fit_target_mib, CancelFlag& cancel) {
    FitEstimate fit;
    fit.backend = backend;
    fit.fit_target_mib = fit_target_mib;
    for (const auto& a : extra_args) fit.extra_args += (fit.extra_args.empty() ? "" : " ") + a;

    const std::string exe = tool_path(backend, "llama-fit-params");
    if (exe.empty()) {
        fit.error = "llama-fit-params not found for backend " + backend;
        return fit;
    }

    std::vector<std::string> args = {"-m", gguf_path, "-fitp", "on", "--fit-target",
                                     std::to_string(fit_target_mib)};
    args.insert(args.end(), extra_args.begin(), extra_args.end());

    std::string output;
    const int rc = ProcessManager::run_process_with_output(
        exe, args,
        [&output, &cancel](const std::string& line) {
            output += line + "\n";
            return !cancel.load();
        },
        "", 120, tool_env(backend, exe));

    if (cancel.load()) {
        fit.error = "cancelled";
        return fit;
    }
    if (rc != 0) {
        fit.error = "llama-fit-params exited with " + std::to_string(rc);
        return fit;
    }
    FitEstimate parsed = parse_fit_params_output(output);
    parsed.backend = backend;
    parsed.fit_target_mib = fit_target_mib;
    parsed.extra_args = fit.extra_args;
    return parsed;
}

std::vector<BenchPoint> RealMeasurementProvider::llama_bench(const std::string& backend,
                                                             const std::string& gguf_path,
                                                             const json& params,
                                                             CancelFlag& cancel,
                                                             ProgressFn progress) {
    const std::string exe = tool_path(backend, "llama-bench");
    if (exe.empty()) {
        BenchPoint p;
        p.backend = backend;
        p.error = "llama-bench not found for backend " + backend;
        return {p};
    }

    std::string depths = "0";
    if (params.contains("d")) {
        if (params["d"].is_array()) {
            depths.clear();
            for (const auto& d : params["d"])
                depths += (depths.empty() ? "" : ",") + std::to_string(d.get<int>());
        } else {
            depths = std::to_string(params["d"].get<int>());
        }
    }

    std::vector<std::string> args = {"-m",   gguf_path, "-fa", "1",  "-r", "2",
                                     "-o",   "json",    "-oe", "none",     "-p",
                                     "2048", "-n",      "32",  "-d", depths};
    if (params.contains("b")) {
        args.push_back("-b");
        args.push_back(std::to_string(params["b"].get<int>()));
        args.push_back("-ub");
        args.push_back(std::to_string(params.value("ub", params["b"].get<int>())));
        args[13] = "0";   // -n 0: batch-ladder points only measure prefill
    }
    if (params.contains("ctk")) {
        args.push_back("-ctk");
        args.push_back(params["ctk"].get<std::string>());
        args.push_back("-ctv");
        args.push_back(params.value("ctv", params["ctk"].get<std::string>()));
    }

    double gb = 0;
    {
        std::error_code ec;
        auto sz = std::filesystem::file_size(lemon::utils::path_from_utf8(gguf_path), ec);
        if (!ec) gb = (double)sz / (1024.0 * 1024.0 * 1024.0);
    }
    const int timeout = std::min(900, 120 + (int)(gb * 30));

    std::string json_out;
    const int rc = ProcessManager::run_process_with_output(
        exe, args,
        [&json_out, &cancel, &progress](const std::string& line) {
            json_out += line + "\n";
            if (progress && line.find("main:") != std::string::npos) progress(line);
            return !cancel.load();
        },
        "", timeout, tool_env(backend, exe));

    if (cancel.load()) return {};
    if (rc != 0) {
        BenchPoint p;
        p.backend = backend;
        p.error = "llama-bench exited with " + std::to_string(rc);
        return {p};
    }
    auto points = parse_llama_bench_json(json_out, backend);
    for (auto& p : points) {
        if (params.contains("b")) {
            p.params["ladder"] = true;
            p.params["b"] = params["b"];
            p.params["ub"] = params.value("ub", params["b"].get<int>());
        }
        if (params.contains("ctk")) p.params["ctk"] = params["ctk"];
    }
    return points;
}

std::optional<double> RealMeasurementProvider::server_decode_probe(
    const std::string& backend, const std::string& gguf_path,
    const std::vector<std::string>& args, CancelFlag& cancel) {
    const auto* spec = lemon::backends::spec_for("llamacpp");
    if (!spec) return std::nullopt;
    std::string exe;
    try {
        exe = BackendUtils::get_backend_binary_path(*spec, backend);
    } catch (const std::exception&) {
        return std::nullopt;
    }

    const int port = ProcessManager::find_free_port(18500);
    std::vector<std::string> full_args = {"-m",   gguf_path,          "--port",
                                          std::to_string(port),       "--no-webui",
                                          "-c",   "8192",             "--host",
                                          "127.0.0.1"};
    full_args.insert(full_args.end(), args.begin(), args.end());

    auto handle = ProcessManager::start_process(exe, full_args, "", false, false,
                                                tool_env(backend, exe));
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    auto cleanup = [&handle]() { ProcessManager::stop_process(handle); };

    bool healthy = false;
    for (int i = 0; i < 300; ++i) {
        if (cancel.load() || !ProcessManager::is_running(handle)) break;
        auto r = HttpClient::get(base + "/health", {}, 2);
        if (r.status_code == 200) {
            healthy = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (!healthy) {
        cleanup();
        return std::nullopt;
    }

    // ~500 tokens of neutral prompt; greedy decode for run-to-run stability.
    std::string prompt;
    for (int i = 0; i < 120; ++i) prompt += "The quick brown fox jumps over the lazy dog. ";
    const json body = {{"prompt", prompt}, {"n_predict", 128}, {"temperature", 0}};

    std::optional<double> tok_s;
    for (int run = 0; run < 2; ++run) {
        if (cancel.load()) break;
        auto r = HttpClient::post(base + "/completion", body.dump(),
                                  {{"Content-Type", "application/json"}}, 120);
        if (r.status_code != 200) break;
        try {
            auto j = json::parse(r.body);
            if (j.contains("timings") && j["timings"].contains("predicted_per_second"))
                tok_s = j["timings"]["predicted_per_second"].get<double>();
        } catch (const std::exception&) {
            break;
        }
    }
    cleanup();
    return tok_s;
}

std::optional<json> RealMeasurementProvider::fetch_generation_config(const std::string& repo) {
    if (repo.empty()) return std::nullopt;
    const std::string url =
        "https://huggingface.co/" + repo + "/resolve/main/generation_config.json";
    auto r = HttpClient::get(url, hf_headers(), 20);
    if (r.status_code != 200) return std::nullopt;
    try {
        return json::parse(r.body);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string resolve_base_model_repo(const std::string& gguf_base_model_repo,
                                    const std::string& checkpoint) {
    if (!gguf_base_model_repo.empty()) return repo_id_from_url(gguf_base_model_repo);
    // checkpoints may carry a ":variant" suffix (repo:quant)
    std::string repo = checkpoint.substr(0, checkpoint.find(':'));
    if (repo.empty() || repo.find('/') == std::string::npos) return "";
    const std::string url = "https://huggingface.co/api/models/" + repo;
    auto r = HttpClient::get(url, hf_headers(), 20);
    if (r.status_code != 200) return "";
    try {
        auto j = json::parse(r.body);
        if (j.contains("cardData") && j["cardData"].contains("base_model")) {
            const auto& bm = j["cardData"]["base_model"];
            if (bm.is_string()) return bm.get<std::string>();
            if (bm.is_array() && !bm.empty() && bm[0].is_string())
                return bm[0].get<std::string>();
        }
    } catch (const std::exception&) {
    }
    return "";
}

}  // namespace autoopt
}  // namespace lemon
