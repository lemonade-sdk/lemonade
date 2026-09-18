#pragma once

#include <functional>
#include <map>
#include <string>
#include <unordered_map>

#include "router.h"

namespace lemon {

struct SystemMetrics {
    double cpu_percent = -1.0;
    double gpu_percent = -1.0;
    double vram_gb = -1.0;
    double npu_percent = -1.0;
};

using MetricsAliasMap = std::map<std::string, std::string>;

// `resolve` returns "" when the alias does not resolve; those entries are dropped.
MetricsAliasMap build_metrics_alias_map(
    const std::unordered_map<std::string, std::string>& raw_aliases,
    const std::function<std::string(const std::string&)>& resolve);

std::string build_prometheus_metrics(Router& router, const SystemMetrics& system_metrics,
                                     const MetricsAliasMap& aliases = {});

} // namespace lemon
