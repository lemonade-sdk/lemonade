#include "lemon/backends/llamacpp/llamacpp_tools.h"

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"

#include <algorithm>
#include <filesystem>

namespace lemon {
namespace backends {
namespace llamacpp {

using lemon::backends::BackendUtils;
using lemon::utils::ProcessManager;

namespace {

std::string tool_path(const std::string& backend, const std::string& tool) {
    const auto* spec = lemon::backends::spec_for("llamacpp");
    if (!spec) return "";
    return BackendUtils::get_backend_tool_path(*spec, backend, tool);
}

std::vector<std::pair<std::string, std::string>> tool_env(const std::string& backend,
                                                          const std::string& exe) {
    return BackendUtils::get_backend_env(backend, exe);
}

}  // namespace

FitEstimate run_fit_params(const std::string& backend, const std::string& gguf_path,
                           const std::vector<std::string>& extra_args, int fit_target_mib,
                           CancelFlag& cancel) {
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
        "", 120, /*capture_stderr=*/true, tool_env(backend, exe));

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

std::vector<BenchPoint> run_llama_bench(const std::string& backend,
                                        const std::string& gguf_path, const json& params,
                                        CancelFlag& cancel, ProgressFn progress) {
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
            if (progress && line.find("main:") != std::string::npos) {
                if (!progress(line)) return false;
            }
            return !cancel.load();
        },
        "", timeout, /*capture_stderr=*/true, tool_env(backend, exe));

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
            p.params["b"] = params["b"];
            p.params["ub"] = params.value("ub", params["b"].get<int>());
        }
        if (params.contains("ctk")) p.params["ctk"] = params["ctk"];
    }
    return points;
}

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
