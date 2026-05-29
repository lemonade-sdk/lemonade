#include "lemon/prometheus_metrics.h"

#include "lemon/version.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include <httplib.h>

#ifdef _WIN32
    #include <windows.h>
#elif defined(__linux__)
    #include <fstream>
#endif

namespace lemon {
namespace {

using json = nlohmann::json;

std::string prometheus_escape_label_value(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        if (ch == '\\') {
            escaped += "\\\\";
        } else if (ch == '"') {
            escaped += "\\\"";
        } else if (ch == '\n') {
            escaped += "\\n";
        } else {
            escaped += ch;
        }
    }
    return escaped;
}

bool json_number_as_double(const json& value, double& out) {
    if (!value.is_number()) {
        return false;
    }
    out = value.get<double>();
    return std::isfinite(out);
}

std::string format_prometheus_double(double value) {
    std::ostringstream oss;
    oss << std::setprecision(17) << value;
    return oss.str();
}

std::string sanitize_prometheus_metric_name(const std::string& name) {
    std::string sanitized;
    sanitized.reserve(name.size());
    for (char ch : name) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) || ch == '_') {
            sanitized += ch;
        } else {
            sanitized += '_';
        }
    }
    if (sanitized.empty() || std::isdigit(static_cast<unsigned char>(sanitized[0]))) {
        sanitized.insert(sanitized.begin(), '_');
    }
    return sanitized;
}

std::string normalize_llamacpp_metric_name(const std::string& name) {
    std::string sanitized = sanitize_prometheus_metric_name(name);
    const std::string prefix = "llamacpp_";
    if (sanitized.rfind(prefix, 0) == 0) {
        sanitized = sanitized.substr(prefix.size());
    }
    return "lemonade_llamacpp_" + sanitized;
}

std::string append_prometheus_labels(const std::string& existing_labels,
                                     const std::map<std::string, std::string>& labels) {
    std::ostringstream oss;
    bool first = existing_labels.empty();
    if (!existing_labels.empty()) {
        oss << existing_labels;
    }
    for (const auto& [key, value] : labels) {
        if (!first) {
            oss << ",";
        }
        first = false;
        oss << key << "=\"" << prometheus_escape_label_value(value) << "\"";
    }
    return oss.str();
}

class PrometheusBuilder {
public:
    void describe(const std::string& name, const std::string& help, const std::string& type) {
        if (!described_.insert(name).second) {
            return;
        }
        out_ << "# HELP " << name << " " << help << "\n";
        out_ << "# TYPE " << name << " " << type << "\n";
    }

    void sample(const std::string& name,
                const std::map<std::string, std::string>& labels,
                double value) {
        if (!std::isfinite(value)) {
            return;
        }
        out_ << name;
        if (!labels.empty()) {
            out_ << "{";
            bool first = true;
            for (const auto& [key, label_value] : labels) {
                if (!first) {
                    out_ << ",";
                }
                first = false;
                out_ << key << "=\"" << prometheus_escape_label_value(label_value) << "\"";
            }
            out_ << "}";
        }
        out_ << " " << format_prometheus_double(value) << "\n";
    }

    void sample_uint(const std::string& name,
                     const std::map<std::string, std::string>& labels,
                     uint64_t value) {
        out_ << name;
        if (!labels.empty()) {
            out_ << "{";
            bool first = true;
            for (const auto& [key, label_value] : labels) {
                if (!first) {
                    out_ << ",";
                }
                first = false;
                out_ << key << "=\"" << prometheus_escape_label_value(label_value) << "\"";
            }
            out_ << "}";
        }
        out_ << " " << value << "\n";
    }

    void append_raw_line(const std::string& line) {
        out_ << line << "\n";
    }

    std::string str() const {
        return out_.str();
    }

private:
    std::ostringstream out_;
    std::set<std::string> described_;
};

bool parse_backend_port(const std::string& backend_url, int& port) {
    const std::string host = "127.0.0.1:";
    size_t host_pos = backend_url.find(host);
    if (host_pos == std::string::npos) {
        return false;
    }
    size_t port_start = host_pos + host.size();
    size_t port_end = port_start;
    while (port_end < backend_url.size() && std::isdigit(static_cast<unsigned char>(backend_url[port_end]))) {
        port_end++;
    }
    if (port_end == port_start) {
        return false;
    }
    try {
        port = std::stoi(backend_url.substr(port_start, port_end - port_start));
        return port > 0;
    } catch (...) {
        return false;
    }
}

std::string rewrite_llamacpp_help_or_type_line(const std::string& line,
                                               std::set<std::string>& described_backend_metrics) {
    const bool is_help = line.rfind("# HELP ", 0) == 0;
    const bool is_type = line.rfind("# TYPE ", 0) == 0;
    if (!is_help && !is_type) {
        return "";
    }

    size_t name_start = 7;
    size_t name_end = line.find(' ', name_start);
    if (name_end == std::string::npos) {
        return "";
    }

    std::string normalized_name = normalize_llamacpp_metric_name(line.substr(name_start, name_end - name_start));
    std::string key = std::string(is_help ? "HELP:" : "TYPE:") + normalized_name;
    if (!described_backend_metrics.insert(key).second) {
        return "";
    }
    return line.substr(0, name_start) + normalized_name + line.substr(name_end);
}

std::string rewrite_llamacpp_sample_line(const std::string& line,
                                         const std::map<std::string, std::string>& labels) {
    if (line.empty() || line[0] == '#') {
        return "";
    }

    size_t name_end = line.find_first_of("{ \t");
    if (name_end == std::string::npos || name_end == 0) {
        return "";
    }

    std::string metric_name = normalize_llamacpp_metric_name(line.substr(0, name_end));
    std::string label_text;
    size_t value_start = name_end;
    if (line[name_end] == '{') {
        size_t label_end = line.find('}', name_end + 1);
        if (label_end == std::string::npos) {
            return "";
        }
        label_text = line.substr(name_end + 1, label_end - name_end - 1);
        value_start = label_end + 1;
    }

    std::string merged_labels = append_prometheus_labels(label_text, labels);
    if (!merged_labels.empty()) {
        metric_name += "{" + merged_labels + "}";
    }

    return metric_name + line.substr(value_start);
}

double get_memory_usage_gb() {
#ifdef _WIN32
    MEMORYSTATUSEX mem_info;
    mem_info.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&mem_info)) {
        double used_gb = (mem_info.ullTotalPhys - mem_info.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
        return std::round(used_gb * 10.0) / 10.0;
    }
#elif defined(__linux__)
    std::ifstream meminfo("/proc/meminfo");
    if (meminfo.is_open()) {
        std::string line;
        long long total_kb = 0;
        long long available_kb = 0;
        while (std::getline(meminfo, line)) {
            if (line.find("MemTotal:") == 0) {
                sscanf(line.c_str(), "MemTotal: %lld kB", &total_kb);
            } else if (line.find("MemAvailable:") == 0) {
                sscanf(line.c_str(), "MemAvailable: %lld kB", &available_kb);
                break;
            }
        }
        if (total_kb > 0 && available_kb >= 0) {
            double used_gb = (total_kb - available_kb) / (1024.0 * 1024.0);
            return std::round(used_gb * 10.0) / 10.0;
        }
    }
#endif
    return -1.0;
}

void append_llamacpp_backend_metrics(PrometheusBuilder& metrics,
                                     const json& model,
                                     const std::map<std::string, std::string>& labels,
                                     std::set<std::string>& described_backend_metrics) {
    if (model.value("recipe", "") != "llamacpp") {
        return;
    }

    int backend_port = 0;
    if (!parse_backend_port(model.value("backend_url", ""), backend_port)) {
        return;
    }

    try {
        httplib::Client backend_client("127.0.0.1", backend_port);
        backend_client.set_connection_timeout(1);
        backend_client.set_read_timeout(1);
        if (auto backend_res = backend_client.Get("/metrics")) {
            if (backend_res->status == 200) {
                std::istringstream backend_metrics(backend_res->body);
                std::string line;
                while (std::getline(backend_metrics, line)) {
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    std::string rewritten = rewrite_llamacpp_help_or_type_line(line, described_backend_metrics);
                    if (rewritten.empty()) {
                        rewritten = rewrite_llamacpp_sample_line(line, labels);
                    }
                    if (!rewritten.empty()) {
                        metrics.append_raw_line(rewritten);
                    }
                }
            }
        }
    } catch (...) {
        // Backend metrics are best effort; skip unavailable backends.
    }
}

} // namespace

std::string build_prometheus_metrics(Router& router, const SystemMetrics& system_metrics) {
    PrometheusBuilder metrics;

    metrics.describe("lemonade_server_up", "Whether the Lemonade server is running.", "gauge");
    metrics.sample("lemonade_server_up", {}, 1.0);

    metrics.describe("lemonade_server_info", "Lemonade server build information.", "gauge");
    metrics.sample("lemonade_server_info", {{"version", LEMON_VERSION_STRING}}, 1.0);

    json snapshot = router.get_metrics_snapshot();
    const json loaded_models = snapshot.value("loaded_models", json::array());
    const json model_metrics = snapshot.value("model_metrics", json::array());

    metrics.describe("lemonade_loaded_models", "Number of models currently loaded in Lemonade.", "gauge");
    metrics.sample("lemonade_loaded_models", {}, static_cast<double>(loaded_models.size()));

    metrics.describe("lemonade_model_info", "Metadata for each Lemonade model observed by this process.", "gauge");
    metrics.describe("lemonade_model_loaded", "Whether this model is currently loaded in Lemonade.", "gauge");
    metrics.describe("lemonade_model_input_tokens", "Latest input token count reported by a model.", "gauge");
    metrics.describe("lemonade_model_output_tokens", "Latest output token count reported by a model.", "gauge");
    metrics.describe("lemonade_model_prompt_tokens", "Latest prompt token count reported by a model.", "gauge");
    metrics.describe("lemonade_model_time_to_first_token_seconds", "Latest time to first token reported by a model.", "gauge");
    metrics.describe("lemonade_model_tokens_per_second", "Latest generation throughput reported by a model.", "gauge");
    metrics.describe("lemonade_model_requests_total", "Cumulative inference requests observed for a model.", "counter");
    metrics.describe("lemonade_model_input_tokens_total", "Cumulative input tokens observed for a model.", "counter");
    metrics.describe("lemonade_model_output_tokens_total", "Cumulative output tokens observed for a model.", "counter");
    metrics.describe("lemonade_model_prompt_tokens_total", "Cumulative prompt tokens observed for a model.", "counter");

    for (const auto& model : model_metrics) {
        std::map<std::string, std::string> labels = {
            {"model_name", model.value("model_name", "")},
            {"checkpoint", model.value("checkpoint", "")},
            {"type", model.value("type", "")},
            {"device", model.value("device", "")},
            {"recipe", model.value("recipe", "")}
        };

        metrics.sample("lemonade_model_info", labels, 1.0);
        metrics.sample("lemonade_model_loaded", labels, model.value("loaded", false) ? 1.0 : 0.0);

        const json telemetry = model.value("telemetry", json::object());
        double metric_value = 0.0;
        if (json_number_as_double(telemetry.value("input_tokens", json()), metric_value)) {
            metrics.sample("lemonade_model_input_tokens", labels, metric_value);
        }
        if (json_number_as_double(telemetry.value("output_tokens", json()), metric_value)) {
            metrics.sample("lemonade_model_output_tokens", labels, metric_value);
        }
        if (json_number_as_double(telemetry.value("prompt_tokens", json()), metric_value)) {
            metrics.sample("lemonade_model_prompt_tokens", labels, metric_value);
        }
        if (json_number_as_double(telemetry.value("time_to_first_token", json()), metric_value)) {
            metrics.sample("lemonade_model_time_to_first_token_seconds", labels, metric_value);
        }
        if (json_number_as_double(telemetry.value("tokens_per_second", json()), metric_value)) {
            metrics.sample("lemonade_model_tokens_per_second", labels, metric_value);
        }

        metrics.sample_uint("lemonade_model_requests_total", labels,
                            telemetry.value("request_count_total", 0ULL));
        metrics.sample_uint("lemonade_model_input_tokens_total", labels,
                            telemetry.value("input_tokens_total", 0ULL));
        metrics.sample_uint("lemonade_model_output_tokens_total", labels,
                            telemetry.value("output_tokens_total", 0ULL));
        metrics.sample_uint("lemonade_model_prompt_tokens_total", labels,
                            telemetry.value("prompt_tokens_total", 0ULL));
    }

    std::set<std::string> described_backend_metrics;
    for (const auto& model : loaded_models) {
        std::map<std::string, std::string> labels = {
            {"model_name", model.value("model_name", "")},
            {"checkpoint", model.value("checkpoint", "")},
            {"type", model.value("type", "")},
            {"device", model.value("device", "")},
            {"recipe", model.value("recipe", "")}
        };
        append_llamacpp_backend_metrics(metrics, model, labels, described_backend_metrics);
    }

    json max_models = router.get_max_model_limits();
    metrics.describe("lemonade_max_loaded_models", "Configured loaded model limit per model type.", "gauge");
    for (auto it = max_models.begin(); it != max_models.end(); ++it) {
        double metric_value = 0.0;
        if (json_number_as_double(it.value(), metric_value)) {
            metrics.sample("lemonade_max_loaded_models", {{"type", it.key()}}, metric_value);
        }
    }

    const json totals = snapshot.value("totals", json::object());
    metrics.describe("lemonade_requests_total", "Cumulative inference requests observed by Lemonade.", "counter");
    metrics.describe("lemonade_input_tokens_total", "Cumulative input tokens observed by Lemonade.", "counter");
    metrics.describe("lemonade_output_tokens_total", "Cumulative output tokens observed by Lemonade.", "counter");
    metrics.describe("lemonade_prompt_tokens_total", "Cumulative prompt tokens observed by Lemonade.", "counter");
    metrics.sample_uint("lemonade_requests_total", {}, totals.value("requests", 0ULL));
    metrics.sample_uint("lemonade_input_tokens_total", {}, totals.value("input_tokens", 0ULL));
    metrics.sample_uint("lemonade_output_tokens_total", {}, totals.value("output_tokens", 0ULL));
    metrics.sample_uint("lemonade_prompt_tokens_total", {}, totals.value("prompt_tokens", 0ULL));

    metrics.describe("lemonade_cpu_usage_percent", "System CPU utilization percentage.", "gauge");
    if (system_metrics.cpu_percent >= 0 && std::isfinite(system_metrics.cpu_percent)) {
        metrics.sample("lemonade_cpu_usage_percent", {}, system_metrics.cpu_percent);
    }

#ifndef __APPLE__
    metrics.describe("lemonade_memory_used_gb", "System memory usage in GiB.", "gauge");
    double memory_gb = get_memory_usage_gb();
    if (memory_gb >= 0 && std::isfinite(memory_gb)) {
        metrics.sample("lemonade_memory_used_gb", {}, memory_gb);
    }
#endif

    metrics.describe("lemonade_gpu_usage_percent", "GPU utilization percentage.", "gauge");
    if (system_metrics.gpu_percent >= 0 && std::isfinite(system_metrics.gpu_percent)) {
        metrics.sample("lemonade_gpu_usage_percent", {}, system_metrics.gpu_percent);
    }

    metrics.describe("lemonade_vram_used_gb", "GPU memory usage in GiB.", "gauge");
    if (system_metrics.vram_gb >= 0 && std::isfinite(system_metrics.vram_gb)) {
        metrics.sample("lemonade_vram_used_gb", {}, system_metrics.vram_gb);
    }

    metrics.describe("lemonade_npu_usage_percent", "NPU utilization percentage.", "gauge");
    if (system_metrics.npu_percent >= 0 && std::isfinite(system_metrics.npu_percent)) {
        metrics.sample("lemonade_npu_usage_percent", {}, system_metrics.npu_percent);
    }

    return metrics.str();
}

} // namespace lemon
