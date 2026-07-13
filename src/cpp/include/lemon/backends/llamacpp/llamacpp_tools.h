// llama.cpp auxiliary tools (llama-fit-params, llama-bench) exposed as
// standalone server queries. Parsers are inline so the test binary needs no
// linking; the runners live in llamacpp_tools.cpp.
#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <cctype>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace lemon {
namespace backends {
namespace llamacpp {

using json = nlohmann::ordered_json;
using CancelFlag = std::atomic<bool>;
using ProgressFn = std::function<bool(const std::string& detail)>;

struct FitEstimate {
    std::string backend;
    int fit_target_mib = 1024;
    std::string extra_args;           // probe variant, e.g. "-ctk q8_0 -ctv q8_0" or "--no-mmproj"
    std::string fitted_args;          // stdout line 1, e.g. "-c 32768 -ngl -1"
    int fitted_ctx = 0;
    int fitted_ngl = -1;              // -1 = full offload
    int fitted_ncmoe = 0;
    struct DeviceMem {
        std::string device;
        int model_mib = 0, ctx_mib = 0, compute_mib = 0;
    };
    std::vector<DeviceMem> devices;
    bool fits_fully = false;
    bool ok = false;
    std::string error;

    int total_mib() const {
        int t = 0;
        for (const auto& d : devices)
            if (d.device.rfind("Host", 0) != 0) t += d.model_mib + d.ctx_mib + d.compute_mib;
        return t;
    }

    json to_json() const {
        json devs = json::array();
        for (const auto& d : devices)
            devs.push_back({{"device", d.device}, {"model_mib", d.model_mib},
                            {"ctx_mib", d.ctx_mib}, {"compute_mib", d.compute_mib}});
        return {{"backend", backend}, {"fit_target_mib", fit_target_mib},
                {"extra_args", extra_args}, {"fitted_args", fitted_args},
                {"fitted_ctx", fitted_ctx}, {"fitted_ngl", fitted_ngl},
                {"fitted_ncmoe", fitted_ncmoe}, {"devices", devs},
                {"fits_fully", fits_fully}, {"ok", ok}, {"error", error}};
    }
};

struct BenchPoint {
    std::string backend;
    json params;                      // varied flags, e.g. {"d":30000,"b":2048,"ub":2048}
    double pp_avg_ts = 0;
    double tg_avg_ts = 0;
    int n_depth = 0;
    bool ok = false;
    std::string error;

    json to_json() const {
        return {{"backend", backend}, {"params", params}, {"pp_avg_ts", pp_avg_ts},
                {"tg_avg_ts", tg_avg_ts}, {"n_depth", n_depth}, {"ok", ok}, {"error", error}};
    }
};

// llama-fit-params stdout: the first line starting with '-' carries the fitted
// args ("-c 32768 -ngl -1"); with -fitp on, subsequent "<dev> <model> <ctx>
// <compute>" rows follow (MiB). Anything else is log noise.
inline FitEstimate parse_fit_params_output(const std::string& output) {
    FitEstimate fit;
    std::istringstream in(output);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        if (fit.fitted_args.empty() && line[0] == '-') {
            fit.fitted_args = line;
            std::istringstream args(line);
            std::string tok;
            while (args >> tok) {
                if (tok == "-c" || tok == "--ctx-size") args >> fit.fitted_ctx;
                else if (tok == "-ngl" || tok == "--n-gpu-layers") args >> fit.fitted_ngl;
                else if (tok == "-ncmoe" || tok == "--n-cpu-moe") args >> fit.fitted_ncmoe;
            }
            continue;
        }
        FitEstimate::DeviceMem d;
        char dev[128] = {};
        if (std::sscanf(line.c_str(), "%127s %d %d %d", dev, &d.model_mib, &d.ctx_mib,
                        &d.compute_mib) == 4 && d.model_mib >= 0) {
            d.device = dev;
            fit.devices.push_back(d);
        }
    }
    // The tool prints the args line only when it had to ADJUST something; a
    // bare memory table means everything fits at the requested (or model
    // default, i.e. full trained) settings. fitted_ctx stays 0 = "no cap".
    fit.ok = !fit.fitted_args.empty() || !fit.devices.empty();
    fit.fits_fully = fit.ok && fit.fitted_ngl == -1 && fit.fitted_ncmoe == 0;
    if (!fit.ok) fit.error = "no fitted-args line or memory table in llama-fit-params output";
    return fit;
}

// llama-bench -o json: array of test objects. Rows with n_prompt>0/n_gen==0
// are prompt-processing, n_gen>0/n_prompt==0 are generation; keyed by n_depth.
// The captured stream interleaves stderr logs (subprocess pipes are merged),
// so the JSON array is extracted between the bare "[" and "]" lines first.
inline std::vector<BenchPoint> parse_llama_bench_json(const std::string& output,
                                                      const std::string& backend) {
    std::vector<BenchPoint> points;
    std::string json_text;
    {
        std::istringstream in(output);
        std::string line;
        bool inside = false;
        while (std::getline(in, line)) {
            std::string trimmed = line;
            while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == ' '))
                trimmed.pop_back();
            if (!inside && trimmed == "[") inside = true;
            if (inside) json_text += line + "\n";
            if (inside && trimmed == "]") break;
        }
        if (json_text.empty()) json_text = output;
    }
    json tests;
    try {
        tests = json::parse(json_text);
    } catch (const std::exception& e) {
        BenchPoint p;
        p.backend = backend;
        p.error = std::string("llama-bench JSON parse failed: ") + e.what();
        points.push_back(p);
        return points;
    }
    if (!tests.is_array()) return points;

    auto point_for_depth = [&points, &backend](int depth) -> BenchPoint& {
        for (auto& p : points)
            if (p.n_depth == depth) return p;
        BenchPoint p;
        p.backend = backend;
        p.n_depth = depth;
        p.params = {{"d", depth}};
        points.push_back(p);
        return points.back();
    };

    for (const auto& t : tests) {
        if (!t.is_object()) continue;
        const int depth = t.value("n_depth", 0);
        const int n_prompt = t.value("n_prompt", 0);
        const int n_gen = t.value("n_gen", 0);
        const double avg = t.value("avg_ts", 0.0);
        BenchPoint& p = point_for_depth(depth);
        if (n_prompt > 0 && n_gen == 0) p.pp_avg_ts = avg;
        else if (n_gen > 0 && n_prompt == 0) p.tg_avg_ts = avg;
        p.ok = p.pp_avg_ts > 0 || p.tg_avg_ts > 0;
    }
    return points;
}

// ── Request validation ─────────────────────────────────────────────────
//
// Both tool endpoints take a small, closed parameter set; requests are
// validated exhaustively (unknown keys, types, ranges) before anything
// reaches a subprocess command line. Validators return "" when the body is
// valid, else a client-facing error message.

namespace tools_detail {

inline const std::set<std::string>& kv_cache_types() {
    static const std::set<std::string> types = {"f32",  "f16",  "bf16", "q8_0",   "q5_1",
                                                "q5_0", "q4_1", "q4_0", "iq4_nl"};
    return types;
}

inline bool valid_backend_name(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return false;
    }
    return true;
}

inline std::string check_common(const json& body, const std::set<std::string>& allowed) {
    if (!body.is_object()) return "request body must be a JSON object";
    for (const auto& [key, value] : body.items()) {
        (void)value;
        if (!allowed.count(key)) {
            std::string keys;
            for (const auto& k : allowed) keys += (keys.empty() ? "" : ", ") + k;
            return "unexpected field '" + key + "'; allowed fields: " + keys;
        }
    }
    if (!body.contains("model") || !body["model"].is_string()
        || body["model"].get<std::string>().empty()) {
        return "'model' is required and must be a non-empty string";
    }
    if (!body.contains("backend") || !body["backend"].is_string()
        || !valid_backend_name(body["backend"].get<std::string>())) {
        return "'backend' is required and must be an installed llamacpp backend name";
    }
    return "";
}

inline bool int_in_range(const json& v, long long lo, long long hi) {
    if (!v.is_number_integer()) return false;
    const long long n = v.get<long long>();
    return n >= lo && n <= hi;
}

}  // namespace tools_detail

inline std::string validate_fit_params_request(const json& body) {
    static const std::set<std::string> allowed = {"model", "backend", "args", "fit_target_mib"};
    std::string err = tools_detail::check_common(body, allowed);
    if (!err.empty()) return err;
    if (body.contains("args")) {
        const auto& args = body["args"];
        if (args.is_array()) {
            for (const auto& a : args)
                if (!a.is_string()) return "'args' array entries must be strings";
        } else if (!args.is_string()) {
            return "'args' must be a string or an array of strings";
        }
    }
    if (body.contains("fit_target_mib")
        && !tools_detail::int_in_range(body["fit_target_mib"], 0, 10000000)) {
        return "'fit_target_mib' must be an integer between 0 and 10000000";
    }
    return "";
}

inline std::string validate_bench_request(const json& body) {
    static const std::set<std::string> allowed = {"model", "backend", "d", "b", "ub",
                                                  "ctk",   "ctv"};
    std::string err = tools_detail::check_common(body, allowed);
    if (!err.empty()) return err;
    if (body.contains("d")) {
        const auto& d = body["d"];
        if (d.is_array()) {
            if (d.empty()) return "'d' array must not be empty";
            for (const auto& v : d)
                if (!tools_detail::int_in_range(v, 0, 4194304))
                    return "'d' entries must be integers between 0 and 4194304";
        } else if (!tools_detail::int_in_range(d, 0, 4194304)) {
            return "'d' must be an integer between 0 and 4194304 or an array of such";
        }
    }
    for (const char* key : {"b", "ub"}) {
        if (body.contains(key) && !tools_detail::int_in_range(body[key], 1, 65536))
            return std::string("'") + key + "' must be an integer between 1 and 65536";
    }
    if (body.contains("ub") && !body.contains("b")) {
        return "'ub' requires 'b'";
    }
    for (const char* key : {"ctk", "ctv"}) {
        if (!body.contains(key)) continue;
        if (!body[key].is_string() || !tools_detail::kv_cache_types().count(body[key].get<std::string>())) {
            std::string types;
            for (const auto& t : tools_detail::kv_cache_types())
                types += (types.empty() ? "" : ", ") + t;
            return std::string("'") + key + "' must be one of: " + types;
        }
    }
    if (body.contains("ctv") && !body.contains("ctk")) {
        return "'ctv' requires 'ctk'";
    }
    return "";
}

// Runs llama-fit-params for `backend` against a local GGUF. Soft-fails into
// the returned struct (ok=false + error); never throws.
FitEstimate run_fit_params(const std::string& backend, const std::string& gguf_path,
                           const std::vector<std::string>& extra_args, int fit_target_mib,
                           CancelFlag& cancel);

// One llama-bench invocation; `params` may carry {"d": int|[ints]}, {"b": N,
// "ub": N}, {"ctk": "q8_0", "ctv": "q8_0"}. Returns one BenchPoint per depth.
// `progress` receives raw tool progress lines; returning false cancels.
std::vector<BenchPoint> run_llama_bench(const std::string& backend,
                                        const std::string& gguf_path, const json& params,
                                        CancelFlag& cancel, ProgressFn progress);

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
