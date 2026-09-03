#include "lemon/runtime_config.h"
#include "lemon/backends/backend_descriptor_registry.h"
#include "lemon/system_info.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/rate_limit_utils.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace fs = std::filesystem;

namespace lemon {

const std::vector<std::string> RuntimeConfig::valid_log_levels_ = {
    "trace", "debug", "info", "warning", "error", "fatal", "none"
};

// Global instance pointer (set once at startup, read from any thread after)
static std::atomic<RuntimeConfig*> s_global_instance{nullptr};

void RuntimeConfig::set_global(RuntimeConfig* instance) {
    s_global_instance.store(instance, std::memory_order_release);
}

RuntimeConfig* RuntimeConfig::global() {
    return s_global_instance.load(std::memory_order_acquire);
}

// A valid config.json backend section is the config_section of any descriptor
// that runs a local subprocess (binary != ""). Cloud has no binary, so it is not
// a backend section.
static bool is_backend_name(const std::string& key) {
    for (const auto* desc : lemon::backends::all_descriptors()) {
        if (!desc->binary.empty() && desc->effective_config_section() == key) {
            return true;
        }
    }
    return false;
}

// A config section has a selectable "backend" key iff its descriptor opts in.
static bool has_backend_selection(const std::string& config_section) {
    for (const auto* desc : lemon::backends::all_descriptors()) {
        if (desc->selectable_backend && desc->effective_config_section() == config_section) {
            return true;
        }
    }
    return false;
}

static void validate_extra_models_dir_access(const std::string& raw_dir) {
    if (raw_dir.empty()) {
        return;
    }

    const fs::path dir = utils::path_from_utf8(raw_dir);
    std::error_code status_ec;
    const fs::file_status status = fs::status(dir, status_ec);

    // Keep the existing runtime semantics for paths that do not exist yet:
    // DirectoryWatcher may observe the directory if it is created shortly after
    // configuration. Permission and I/O failures, however, must not be accepted
    // as a successful config update.
    if (status_ec) {
        if (status_ec == std::errc::no_such_file_or_directory) {
            return;
        }
        throw std::invalid_argument(
            "'extra_models_dir' is not accessible by the Lemonade server: " +
            raw_dir + " (" + status_ec.message() + ")");
    }
    if (!fs::exists(status)) {
        return;
    }
    if (!fs::is_directory(status)) {
        throw std::invalid_argument(
            "'extra_models_dir' must reference a directory: " + raw_dir);
    }

    // status() can succeed when the process can traverse the path but cannot
    // enumerate the directory. Probe enumeration so the GUI can report a
    // permission error before RuntimeConfig::set applies either directory key.
    std::error_code read_ec;
    fs::directory_iterator{dir, read_ec};
    if (read_ec) {
        throw std::invalid_argument(
            "'extra_models_dir' is not readable by the Lemonade server: " +
            raw_dir + " (" + read_ec.message() + ")");
    }
}

static std::pair<json, std::string> normalize_config_set_changes(const json& changes) {
    json normalized = changes;
    std::string message;

    if (normalized.contains("rocm")) {
        if (normalized.contains("rocm_channel") && normalized["rocm_channel"] != normalized["rocm"]) {
            throw std::invalid_argument(
                "Ambiguous ROCm channel settings: use only 'rocm_channel'");
        }
        normalized["rocm_channel"] = normalized["rocm"];
        normalized.erase("rocm");
    }

    if (normalized.contains("rocm_channel") && normalized["rocm_channel"].is_string() &&
        normalized["rocm_channel"].get<std::string>() == "preview") {
        normalized["rocm_channel"] = "stable";
        message = "rocm_channel=preview is deprecated; using rocm_channel=stable";
        LOG(WARNING) << message << std::endl;
    }

    // Reject conflicting broadcast options
    if (normalized.contains("broadcast") && normalized.contains("no_broadcast")) {
        throw std::invalid_argument("Cannot specify both 'broadcast' and 'no_broadcast'");
    }

    if (normalized.contains("broadcast") && !normalized["broadcast"].is_boolean()) {
        throw std::invalid_argument("'broadcast' must be a boolean");
    }

    // Migrate legacy no_broadcast to broadcast
    if (normalized.contains("no_broadcast")) {
        if (!normalized["no_broadcast"].is_boolean()) {
            throw std::invalid_argument("'no_broadcast' must be a boolean");
        }
        normalized["broadcast"] = !normalized["no_broadcast"].get<bool>();
        normalized.erase("no_broadcast");
    }

    // Promote flat backend keys (e.g. "vllm_args", "llamacpp_backend") into
    // their nested form ("vllm": {"args": ...}). Backend CLI flags and docs use
    // the flat underscore form, but config.json stores backend options nested.
    // Only keys whose prefix (before the first underscore) is a known backend
    // are promoted; no top-level config key starts with a backend name, so this
    // mapping is unambiguous. See issue #1824.
    std::vector<std::string> flat_backend_keys;
    for (auto& [key, value] : normalized.items()) {
        size_t underscore = key.find('_');
        if (underscore == std::string::npos) continue;
        if (is_backend_name(key.substr(0, underscore))) {
            flat_backend_keys.push_back(key);
        }
    }
    for (const auto& key : flat_backend_keys) {
        size_t underscore = key.find('_');
        std::string backend = key.substr(0, underscore);
        std::string field = key.substr(underscore + 1);
        json value = normalized[key];

        if (!normalized.contains(backend)) {
            normalized[backend] = json::object();
        } else if (!normalized[backend].is_object()) {
            throw std::invalid_argument(
                "Ambiguous config: '" + key + "' conflicts with non-object '" + backend + "'");
        }
        if (normalized[backend].contains(field) && normalized[backend][field] != value) {
            throw std::invalid_argument(
                "Ambiguous config: both '" + key + "' and '" + backend + "." + field +
                "' were provided with different values");
        }
        normalized[backend][field] = value;
        normalized.erase(key);
    }

    return {normalized, message};
}

std::string RuntimeConfig::config_section_to_recipe(const std::string& config_section) {
    for (const auto* desc : lemon::backends::all_descriptors()) {
        if (desc->effective_config_section() == config_section) {
            return desc->recipe;
        }
    }
    return config_section;
}

std::string RuntimeConfig::recipe_to_config_section(const std::string& recipe) {
    if (const auto* desc = lemon::backends::descriptor_for(recipe)) {
        return desc->effective_config_section();
    }
    return recipe;
}

void RuntimeConfig::validate_backend_choice(const std::string& config_section,
                                             const std::string& value) {
    if (value == "auto") return;

    if (!has_backend_selection(config_section)) {
        throw std::invalid_argument(
            "'" + config_section + "' does not have a selectable backend");
    }

    std::string recipe = config_section_to_recipe(config_section);

    if (value == "system") {
        const auto* desc = lemon::backends::descriptor_for(recipe);
        const std::string current_os = get_current_os();
        if (desc) {
            auto supported = std::find_if(
                desc->support.begin(), desc->support.end(),
                [&](const BackendSupport& row) {
                    return row.backend == value && row.supported_os.count(current_os) > 0;
                });
            if (supported != desc->support.end()) return;
        }
    }

    auto result = SystemInfo::get_supported_backends(recipe);

    if (std::find(result.backends.begin(), result.backends.end(), value)
        == result.backends.end()) {
        std::string allowed = "auto";
        for (const auto& b : result.backends) {
            allowed += ", " + b;
        }
        throw std::invalid_argument(
            "'" + config_section + ".backend' must be one of: " + allowed);
    }
}

static void validate_path(const std::string& config_section,
                            const std::string& key,
                            const std::string& value,
                            bool is_bin=false) {
    if (utils::looks_like_path(value)) {
        if (!fs::exists(value)) {
            throw std::invalid_argument(
                "'" + config_section + "." + key + "' path does not exist: " + value
                + (is_bin ? (". Use \"builtin\", \"latest\", a version tag (e.g. \"b8664\"),"
                  " or a path to a pre-downloaded binary.") : ""));
        }
        return;
    }
}

void RuntimeConfig::validate_bin_path(const std::string& config_section,
                                       const std::string& key,
                                       const std::string& value) {
    // Reserved keywords:
    //   ""        — alias for "builtin"
    //   "builtin" — use the version pinned by lemonade in backend_versions.json
    //   "latest"  — resolve to the most-recent upstream release at lemond start
    if (value.empty() || value == "builtin" || value == "latest") return;

    // Absolute-path values are treated as user-supplied binary directories and
    // must exist. Relative-looking values intentionally fall through to the
    // version-tag branch so backend pins are not interpreted relative to
    // lemond's launch directory.
    validate_path(config_section, key, value, true);
    // Anything else is treated as an upstream release tag (e.g. "b8664",
    // "v1.8.2") and accepted verbatim. The download step surfaces a clear
    // error if the tag does not exist on GitHub.
}

RuntimeConfig::RuntimeConfig(const json& config)
    : config_(config) {
    if (config_.contains("broadcast") && !config_["broadcast"].is_boolean()) {
        throw std::invalid_argument("'broadcast' must be a boolean");
    }
    // Migrate legacy no_broadcast if present
    if (config_.contains("no_broadcast")) {
        if (!config_["no_broadcast"].is_boolean()) {
            throw std::invalid_argument("'no_broadcast' must be a boolean");
        }
        if (!config_.contains("broadcast")) {
            config_["broadcast"] = !config_["no_broadcast"].get<bool>();
        }
        config_.erase("no_broadcast");
    }

    // Validate logging settings on startup
    if (config_.contains("log_max_file_size_mb")) {
        validate("log_max_file_size_mb", config_["log_max_file_size_mb"]);
    }
    if (config_.contains("log_max_files")) {
        validate("log_max_files", config_["log_max_files"]);
    }
    if (config_.contains("log_file")) {
        validate("log_file", config_["log_file"]);
    }
    if (config_.contains("log_level")) {
        validate("log_level", config_["log_level"]);
    }

    // In CI mode, override log level to debug for easier diagnostics
    const char* ci_mode = std::getenv("LEMONADE_CI_MODE");
    if (ci_mode && (std::string(ci_mode) == "1" || std::string(ci_mode) == "true" ||
                    std::string(ci_mode) == "True" || std::string(ci_mode) == "TRUE")) {
        config_["log_level"] = "debug";
    }
}

int RuntimeConfig::port() const {
    std::shared_lock lock(mutex_);
    if (port_override_.has_value()) {
        return *port_override_;
    }
    return config_["port"].get<int>();
}

void RuntimeConfig::set_port_override(std::optional<int> override_val) {
    std::unique_lock lock(mutex_);
    port_override_ = override_val;
}

std::string RuntimeConfig::host() const {
    std::shared_lock lock(mutex_);
    if (host_override_.has_value()) {
        return *host_override_;
    }
    return config_["host"].get<std::string>();
}

void RuntimeConfig::set_host_override(std::optional<std::string> override_val) {
    std::unique_lock lock(mutex_);
    host_override_ = override_val;
}

int RuntimeConfig::websocket_port() const {
    std::shared_lock lock(mutex_);
    const auto& val = config_["websocket_port"];
    if (val.is_string() && val.get<std::string>() == "auto") {
        return 0;  // OS auto-assign
    }
    return val.get<int>();
}

std::string RuntimeConfig::log_level() const {
    std::shared_lock lock(mutex_);
    return config_["log_level"].get<std::string>();
}

std::string RuntimeConfig::log_file() const {
    {
        std::shared_lock lock(mutex_);
        if (log_file_override_.has_value()) {
            return *log_file_override_;
        }
    }
    return get_string_opt(nullptr, {"log_file"}, "auto");
}

void RuntimeConfig::set_log_file_override(std::optional<std::string> override_val) {
    std::unique_lock lock(mutex_);
    log_file_override_ = std::move(override_val);
}

int RuntimeConfig::log_max_file_size_mb() const {
    {
        std::shared_lock lock(mutex_);
        if (log_max_file_size_mb_override_.has_value()) {
            return *log_max_file_size_mb_override_;
        }
    }
    return get_int_opt(nullptr, {"log_max_file_size_mb"}, 10);
}

void RuntimeConfig::set_log_max_file_size_mb_override(std::optional<int> override_val) {
    std::unique_lock lock(mutex_);
    log_max_file_size_mb_override_ = override_val;
}

int RuntimeConfig::log_max_files() const {
    {
        std::shared_lock lock(mutex_);
        if (log_max_files_override_.has_value()) {
            return *log_max_files_override_;
        }
    }
    return get_int_opt(nullptr, {"log_max_files"}, 5);
}

void RuntimeConfig::set_log_max_files_override(std::optional<int> override_val) {
    std::unique_lock lock(mutex_);
    log_max_files_override_ = override_val;
}

std::string RuntimeConfig::extra_models_dir() const {
    std::shared_lock lock(mutex_);
    return config_["extra_models_dir"].get<std::string>();
}

bool RuntimeConfig::broadcast() const {
    std::shared_lock lock(mutex_);
    if (broadcast_override_.has_value()) {
        return *broadcast_override_;
    }
    if (config_.contains("broadcast") && config_["broadcast"].is_boolean()) {
        return config_["broadcast"].get<bool>();
    }
    if (config_.contains("no_broadcast") && config_["no_broadcast"].is_boolean()) {
        return !config_["no_broadcast"].get<bool>();
    }
    return true;
}

void RuntimeConfig::set_broadcast_override(std::optional<bool> override_val) {
    std::unique_lock lock(mutex_);
    broadcast_override_ = override_val;
}

long RuntimeConfig::global_timeout() const {
    std::shared_lock lock(mutex_);
    return config_["global_timeout"].get<long>();
}

int RuntimeConfig::max_loaded_models() const {
    std::shared_lock lock(mutex_);
    return config_["max_loaded_models"].get<int>();
}

int64_t RuntimeConfig::download_rate_limit_bytes_per_second() const {
    std::shared_lock lock(mutex_);
    if (!config_.contains("download_rate_limit") || !config_["download_rate_limit"].is_string()) {
        return 0;
    }
    const int64_t parsed = utils::parse_rate_limit_to_bytes(config_["download_rate_limit"].get<std::string>());
    if (parsed < 0) {
        LOG(WARNING, "RuntimeConfig") << "Invalid download_rate_limit value in config, treating as unlimited" << std::endl;
        return 0;
    }
    return parsed;
}

std::string RuntimeConfig::allowed_origins_unlocked() const {
    if (allowed_origins_override_.has_value()) {
        return *allowed_origins_override_;
    }
    if (const char* env = std::getenv("LEMONADE_ALLOWED_ORIGINS")) {
        return std::string(env);
    }
    if (config_.contains("allowed_origins") && config_["allowed_origins"].is_string()) {
        return config_["allowed_origins"].get<std::string>();
    }
    return "";
}

std::string RuntimeConfig::allowed_origins() const {
    std::shared_lock lock(mutex_);
    return allowed_origins_unlocked();
}

std::string RuntimeConfig::models_dir() const {
    std::shared_lock lock(mutex_);
    return config_["models_dir"].get<std::string>();
}

int RuntimeConfig::ctx_size() const {
    std::shared_lock lock(mutex_);
    return config_["ctx_size"].get<int>();
}

bool RuntimeConfig::auto_evict() const {
    std::shared_lock lock(mutex_);
    if (config_.contains("auto_evict")) {
        return config_["auto_evict"].get<bool>();
    }
    return false;
}

bool RuntimeConfig::inhibit_suspend() const {
    std::shared_lock lock(mutex_);
    if (config_.contains("inhibit_suspend")) {
        return config_["inhibit_suspend"].get<bool>();
    }
    return true;
}

double RuntimeConfig::auto_evict_threshold_pct() const {
    std::shared_lock lock(mutex_);
    if (config_.contains("auto_evict_threshold_pct")) {
        return config_["auto_evict_threshold_pct"].get<double>();
    }
    // Default: start yielding VRAM at 90% global usage so other GPU apps
    // (ComfyUI, Blender, games) can coexist before memory is fully exhausted.
    return 0.90;
}

bool RuntimeConfig::offline() const {
    std::shared_lock lock(mutex_);
    return config_["offline"].get<bool>();
}

bool RuntimeConfig::auto_check_model_updates() const {
    std::shared_lock lock(mutex_);
    if (config_.contains("auto_check_model_updates")) {
        return config_["auto_check_model_updates"].get<bool>();
    }
    return true;
}

bool RuntimeConfig::auto_update_models() const {
    std::shared_lock lock(mutex_);
    if (config_.contains("auto_update_models")) {
        return config_["auto_update_models"].get<bool>();
    }
    return false;
}

bool RuntimeConfig::no_fetch_executables() const {
    std::shared_lock lock(mutex_);
    return config_["no_fetch_executables"].get<bool>();
}

bool RuntimeConfig::disable_model_filtering() const {
    std::shared_lock lock(mutex_);
    return config_["disable_model_filtering"].get<bool>();
}

bool RuntimeConfig::enable_dgpu_gtt() const {
    std::shared_lock lock(mutex_);
    return config_["enable_dgpu_gtt"].get<bool>();
}

std::string RuntimeConfig::default_model_source() const {
    std::shared_lock lock(mutex_);
    return config_.value("default_model_source", std::string("huggingface"));
}

std::string RuntimeConfig::rocm_channel() const {
    std::shared_lock lock(mutex_);
    return config_["rocm_channel"].get<std::string>();
}

std::string RuntimeConfig::rocm_channel_for_recipe(const std::string& recipe) const {
    std::string channel = rocm_channel();
    // Clamp to a channel the backend actually publishes. A backend that lists
    // only {"stable"} (e.g. sd-cpp, which has no nightly artifacts) falls back to
    // its first channel when "nightly" is requested.
    const auto* desc = lemon::backends::descriptor_for(recipe);
    if (desc && !desc->rocm_channels.empty()) {
        const auto& channels = desc->rocm_channels;
        if (std::find(channels.begin(), channels.end(), channel) == channels.end()) {
            return channels.front();
        }
    }
    return channel;
}

std::string RuntimeConfig::rocm_install_method() const {
    return get_string_opt("LEMONADE_ROCM_INSTALL_METHOD", {"rocm_install_method"}, "auto");
}

bool RuntimeConfig::telemetry_enabled() const {
    return get_bool_opt(nullptr, {"telemetry", "enabled"}, false);
}

bool RuntimeConfig::telemetry_hide_inputs() const {
    return get_bool_opt(nullptr, {"telemetry", "hide_inputs"}, false);
}

bool RuntimeConfig::telemetry_hide_outputs() const {
    return get_bool_opt(nullptr, {"telemetry", "hide_outputs"}, false);
}

bool RuntimeConfig::telemetry_hide_thinking() const {
    return get_bool_opt(nullptr, {"telemetry", "hide_thinking"}, false);
}

bool RuntimeConfig::telemetry_trust_incoming_trace_context() const {
    return get_bool_opt(nullptr, {"telemetry", "trust_incoming_trace_context"}, false);
}

int RuntimeConfig::telemetry_max_queue_capacity() const {
    return get_int_opt(nullptr, {"telemetry", "max_queue_capacity"}, 1000);
}

int RuntimeConfig::telemetry_max_attribute_length() const {
    return get_int_opt(nullptr, {"telemetry", "max_attribute_length"}, 4096);
}

std::string RuntimeConfig::telemetry_otlp_endpoint() const {
    return get_string_opt(nullptr, {"telemetry", "otlp", "endpoint"}, "http://localhost:4318/v1/traces");
}

std::string RuntimeConfig::telemetry_otlp_protocol() const {
    return get_string_opt(nullptr, {"telemetry", "otlp", "protocol"}, "http/protobuf");
}

std::vector<std::string> RuntimeConfig::telemetry_otlp_semantics() const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> semantics;
    if (config_.contains("telemetry") && config_["telemetry"].is_object() &&
        config_["telemetry"].contains("otlp") && config_["telemetry"]["otlp"].is_object() &&
        config_["telemetry"]["otlp"].contains("semantics") && config_["telemetry"]["otlp"]["semantics"].is_array()) {
        for (const auto& item : config_["telemetry"]["otlp"]["semantics"]) {
            if (item.is_string()) {
                semantics.push_back(item.get<std::string>());
            }
        }
    }
    return semantics;
}

std::map<std::string, std::string> RuntimeConfig::telemetry_otlp_headers() const {
    std::map<std::string, std::string> headers;
    std::shared_lock lock(mutex_);
    if (config_.contains("telemetry") && config_["telemetry"].is_object() &&
        config_["telemetry"].contains("otlp") && config_["telemetry"]["otlp"].is_object() &&
        config_["telemetry"]["otlp"].contains("headers") && config_["telemetry"]["otlp"]["headers"].is_object()) {
        const auto& headers_json = config_["telemetry"]["otlp"]["headers"];
        for (auto& [key, value] : headers_json.items()) {
            if (value.is_string()) {
                headers[key] = value.get<std::string>();
            }
        }
    }
    return headers;
}

int RuntimeConfig::telemetry_otlp_max_retries() const {
    return get_int_opt(nullptr, {"telemetry", "otlp", "max_retries"}, 0);
}

double RuntimeConfig::telemetry_otlp_retry_backoff_base_s() const {
    return get_double_opt(nullptr, {"telemetry", "otlp", "retry_backoff_base_s"}, 5.0);
}

int RuntimeConfig::telemetry_otlp_send_batch_size() const {
    return get_int_opt(nullptr, {"telemetry", "otlp", "send_batch_size"}, 100);
}

double RuntimeConfig::telemetry_otlp_batch_timeout_s() const {
    return get_double_opt(nullptr, {"telemetry", "otlp", "batch_timeout_s"}, 1.0);
}

static void extract_header_strings(const json& val, std::vector<std::string>& headers) {
    if (val.is_array()) {
        for (const auto& item : val) {
            if (item.is_string()) {
                std::string s = item.get<std::string>();
                if (!s.empty()) headers.push_back(s);
            }
        }
    } else if (val.is_string()) {
        std::string s = val.get<std::string>();
        if (!s.empty()) headers.push_back(s);
    }
}

std::vector<std::string> RuntimeConfig::telemetry_session_headers_id() const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> headers;
    if (config_.contains("telemetry") && config_["telemetry"].is_object() &&
        config_["telemetry"].contains("session") && config_["telemetry"]["session"].is_object()) {
        const auto& sess = config_["telemetry"]["session"];
        if (sess.contains("headers") && sess["headers"].is_object() && sess["headers"].contains("id")) {
            extract_header_strings(sess["headers"]["id"], headers);
        }
    }
    return headers;
}

std::vector<std::string> RuntimeConfig::telemetry_session_headers_client() const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> headers;
    if (config_.contains("telemetry") && config_["telemetry"].is_object() &&
        config_["telemetry"].contains("session") && config_["telemetry"]["session"].is_object()) {
        const auto& sess = config_["telemetry"]["session"];
        if (sess.contains("headers") && sess["headers"].is_object() && sess["headers"].contains("client")) {
            extract_header_strings(sess["headers"]["client"], headers);
        }
    }
    return headers;
}

json RuntimeConfig::backend_config(const std::string& backend_name) const {
    std::shared_lock lock(mutex_);
    if (config_.contains(backend_name) && config_[backend_name].is_object()) {
        return config_[backend_name];
    }
    return json::object();
}

std::string RuntimeConfig::backend_string(const std::string& backend,
                                           const std::string& key) const {
    std::shared_lock lock(mutex_);
    if (config_.contains(backend) && config_[backend].contains(key)) {
        return config_[backend][key].get<std::string>();
    }
    return "";
}

bool RuntimeConfig::backend_bool(const std::string& backend,
                                  const std::string& key) const {
    std::shared_lock lock(mutex_);
    if (config_.contains(backend) && config_[backend].contains(key)) {
        return config_[backend][key].get<bool>();
    }
    return false;
}

int RuntimeConfig::backend_int(const std::string& backend,
                                const std::string& key) const {
    std::shared_lock lock(mutex_);
    if (config_.contains(backend) && config_[backend].contains(key)) {
        return config_[backend][key].get<int>();
    }
    return 0;
}

double RuntimeConfig::backend_double(const std::string& backend,
                                      const std::string& key) const {
    std::shared_lock lock(mutex_);
    if (config_.contains(backend) && config_[backend].contains(key)) {
        return config_[backend][key].get<double>();
    }
    return 0.0;
}

json RuntimeConfig::recipe_options(const std::string& backend) const {
    std::shared_lock lock(mutex_);
    json result = json::object();

    // "auto" in config.json means auto-detect; the flat recipe_options format
    // uses "" (empty string) for auto-detect, so we translate here.
    auto resolve_auto = [](const json& val) -> json {
        if (val.is_string() && val.get<std::string>() == "auto") return "";
        return val;
    };

    auto ends_with = [](const std::string& s, const std::string& suf) {
        return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
    };

    const std::string backend_args = backend + "_args";

    // Translate each backend's nested config.json section into the flat
    // recipe_options format, driven by the descriptor's option list. The flat
    // key is the descriptor option name; the config.json key is derived from the
    // option's role (its name suffix):
    //   *_backend -> "backend"   *_args -> variant "<backend>_args" then "args"
    //   *_device  -> "device"    everything else -> the option name verbatim
    //                            (sd-cpp's steps/cfg_scale/width/height/…)
    for (const auto* desc : lemon::backends::all_descriptors()) {
        const std::string section = desc->effective_config_section();
        if (!config_.contains(section) || !config_[section].is_object()) {
            continue;
        }
        const auto& cfg = config_[section];
        for (const auto& opt : desc->options) {
            if (ends_with(opt.name, "_backend")) {
                if (cfg.contains("backend")) {
                    result[opt.name] = resolve_auto(cfg["backend"]);
                }
            } else if (ends_with(opt.name, "_args")) {
                if (cfg.contains(backend_args) && cfg[backend_args] != "") {
                    result[opt.name] = cfg[backend_args];
                } else if (cfg.contains("args")) {
                    result[opt.name] = cfg["args"];
                }
            } else {
                const std::string ckey = ends_with(opt.name, "_device") ? "device" : opt.name;
                if (cfg.contains(ckey)) {
                    result[opt.name] = cfg[ckey];
                }
            }
        }
    }

    if (config_.contains("ctx_size")) result["ctx_size"] = config_["ctx_size"];

    return result;
}

void RuntimeConfig::validate(const std::string& key, const json& value) const {
    if (key == "port") {
        if (!value.is_number_integer()) {
            throw std::invalid_argument("'port' must be an integer");
        }
        int64_t p = value.get<int64_t>();
        if (p < 1 || p > 65535) {
            throw std::invalid_argument("'port' must be between 1 and 65535");
        }
    } else if (key == "host") {
        if (!value.is_string()) {
            throw std::invalid_argument("'host' must be a string");
        }
    } else if (key == "websocket_port") {
        if (value.is_string()) {
            if (value.get<std::string>() != "auto") {
                throw std::invalid_argument(
                    "'websocket_port' must be \"auto\" or an integer 0-65535");
            }
        } else if (value.is_number_integer()) {
            int64_t p = value.get<int64_t>();
            if (p < 0 || p > 65535) {
                throw std::invalid_argument(
                    "'websocket_port' must be between 0 and 65535");
            }
        } else {
            throw std::invalid_argument(
                "'websocket_port' must be \"auto\" or an integer 0-65535");
        }
    } else if (key == "log_level") {
        if (!value.is_string()) {
            throw std::invalid_argument("'log_level' must be a string");
        }
        std::string level = value.get<std::string>();
        if (std::find(valid_log_levels_.begin(), valid_log_levels_.end(), level)
            == valid_log_levels_.end()) {
            throw std::invalid_argument(
                "'log_level' must be one of: trace, debug, info, warning, error, fatal, none");
        }
    } else if (key == "log_file") {
        if (!value.is_string()) {
            throw std::invalid_argument("'log_file' must be a string");
        }
        std::string mode = value.get<std::string>();
        if (mode != "auto" && mode != "enabled" && mode != "disabled" && !mode.empty()) {
            std::error_code ec;
            fs::path p = utils::path_from_utf8(mode);
            if (fs::exists(p, ec) && fs::is_directory(p, ec)) {
                throw std::invalid_argument("'log_file' path cannot be a directory: " + mode);
            }
        }
    } else if (key == "log_max_file_size_mb") {
        if (!value.is_number_integer()) {
            throw std::invalid_argument("'log_max_file_size_mb' must be an integer");
        }
        int64_t sz = value.get<int64_t>();
        if (sz < 1 || sz > 2048) {
            throw std::invalid_argument("'log_max_file_size_mb' must be between 1 and 2048");
        }
    } else if (key == "log_max_files") {
        if (!value.is_number_integer()) {
            throw std::invalid_argument("'log_max_files' must be an integer");
        }
        int64_t n = value.get<int64_t>();
        if (n < 0 || n > 100) {
            throw std::invalid_argument("'log_max_files' must be between 0 and 100");
        }
    } else if (key == "extra_models_dir" || key == "models_dir") {
        if (!value.is_string()) {
            throw std::invalid_argument("'" + key + "' must be a string");
        }
        if (key == "extra_models_dir") {
            validate_extra_models_dir_access(value.get<std::string>());
        }
    } else if (key == "default_model_source") {
        if (!value.is_string()) {
            throw std::invalid_argument("'default_model_source' must be a string");
        }
        const std::string source = value.get<std::string>();
        if (source != "huggingface" && source != "modelscope") {
            throw std::invalid_argument(
                "'default_model_source' must be either 'huggingface', or 'modelscope'");
        }
    } else if (key == "download_rate_limit") {
        if (!value.is_string()) {
            throw std::invalid_argument("'download_rate_limit' must be a byte rate string");
        }
        if (utils::parse_rate_limit_to_bytes(value.get<std::string>()) < 0) {
            throw std::invalid_argument(
                "'download_rate_limit' must be a byte rate like \"512\", \"100K\", \"10M\", etc. "
                "Use \"\" for unlimited download speed");
        }
    } else if (key == "allowed_origins") {
        if (!value.is_string()) {
            throw std::invalid_argument("'allowed_origins' must be a string");
        }
    } else if (key == "broadcast" || key == "no_broadcast" || key == "offline" ||
               key == "auto_check_model_updates" ||
               key == "auto_update_models" ||
               key == "no_fetch_executables" ||
               key == "disable_model_filtering" || key == "enable_dgpu_gtt") {
        if (!value.is_boolean()) {
            throw std::invalid_argument("'" + key + "' must be a boolean");
        }
    } else if (key == "global_timeout") {
        if (!value.is_number_integer()) {
            throw std::invalid_argument("'global_timeout' must be an integer");
        }
        if (value.get<long>() <= 0) {
            throw std::invalid_argument("'global_timeout' must be positive");
        }
    } else if (key == "max_loaded_models") {
        if (!value.is_number_integer()) {
            throw std::invalid_argument("'max_loaded_models' must be an integer");
        }
        int m = value.get<int>();
        if (m < -1 || m == 0) {
            throw std::invalid_argument(
                "'max_loaded_models' must be -1 (unlimited) or a positive integer");
        }
    } else if (key == "ctx_size") {
        if (!value.is_number_integer()) {
            throw std::invalid_argument("'ctx_size' must be an integer");
        }
        if (value.get<int>() < -1) {
            throw std::invalid_argument("'ctx_size' must be >= -1");
        }
    } else if (key == "auto_evict") {
        if (!value.is_boolean()) {
            throw std::invalid_argument("'auto_evict' must be a boolean");
        }
    } else if (key == "inhibit_suspend") {
        if (!value.is_boolean()) {
            throw std::invalid_argument("'inhibit_suspend' must be a boolean");
        }
    } else if (key == "auto_evict_threshold_pct") {
        if (!value.is_number()) {
            throw std::invalid_argument("'auto_evict_threshold_pct' must be a number");
        }
        if (value.get<double>() <= 0.0 || value.get<double>() > 1.0) {
            throw std::invalid_argument("'auto_evict_threshold_pct' must be between 0.0 and 1.0");
        }
    } else if (key == "config_version") {
        if (!value.is_number_integer()) {
            throw std::invalid_argument("'config_version' must be an integer");
        }
        if (value.get<int>() < 1) {
            throw std::invalid_argument("'config_version' must be >= 1");
        }
    } else if (key == "rocm_channel") {
        if (!value.is_string()) {
            throw std::invalid_argument("'rocm_channel' must be a string");
        }
        std::string channel = value.get<std::string>();
        if (channel != "stable" && channel != "nightly") {
            throw std::invalid_argument("'rocm_channel' must be either 'stable', or 'nightly'");
        }
    } else if (key == "rocm_install_method") {
        if (!value.is_string()) {
            throw std::invalid_argument("'rocm_install_method' must be a string");
        }
        std::string method = value.get<std::string>();
        if (method != "auto" && method != "wheel" && method != "tarball") {
            throw std::invalid_argument(
                "'rocm_install_method' must be 'auto', 'wheel', or 'tarball'");
        }
    } else if (key == "telemetry") {
        if (!value.is_object()) {
            throw std::invalid_argument("'telemetry' must be an object");
        }
        static const std::unordered_set<std::string> valid_telemetry_keys = {
            "enabled", "hide_inputs", "hide_outputs", "hide_thinking", "trust_incoming_trace_context",
            "max_queue_capacity", "max_attribute_length", "otlp", "session"
        };
        for (auto& [t_key, t_val] : value.items()) {
            if (valid_telemetry_keys.find(t_key) == valid_telemetry_keys.end()) {
                throw std::invalid_argument("Unknown config key: 'telemetry." + t_key + "'");
            }
        }
        if (value.contains("enabled") && !value["enabled"].is_boolean()) {
            throw std::invalid_argument("'telemetry.enabled' must be a boolean");
        }
        if (value.contains("hide_inputs") && !value["hide_inputs"].is_boolean()) {
            throw std::invalid_argument("'telemetry.hide_inputs' must be a boolean");
        }
        if (value.contains("hide_outputs") && !value["hide_outputs"].is_boolean()) {
            throw std::invalid_argument("'telemetry.hide_outputs' must be a boolean");
        }
        if (value.contains("hide_thinking") && !value["hide_thinking"].is_boolean()) {
            throw std::invalid_argument("'telemetry.hide_thinking' must be a boolean");
        }
        if (value.contains("trust_incoming_trace_context") && !value["trust_incoming_trace_context"].is_boolean()) {
            throw std::invalid_argument("'telemetry.trust_incoming_trace_context' must be a boolean");
        }
        if (value.contains("max_queue_capacity")) {
            if (!value["max_queue_capacity"].is_number_integer()) {
                throw std::invalid_argument("'telemetry.max_queue_capacity' must be an integer");
            }
            if (value["max_queue_capacity"].get<int>() <= 0) {
                throw std::invalid_argument("'telemetry.max_queue_capacity' must be > 0");
            }
        }
        if (value.contains("max_attribute_length")) {
            if (!value["max_attribute_length"].is_number_integer()) {
                throw std::invalid_argument("'telemetry.max_attribute_length' must be an integer");
            }
            if (value["max_attribute_length"].get<int>() <= 0) {
                throw std::invalid_argument("'telemetry.max_attribute_length' must be > 0");
            }
        }
        if (value.contains("otlp")) {
            const auto& otlp = value["otlp"];
            if (!otlp.is_object()) {
                throw std::invalid_argument("'telemetry.otlp' must be an object");
            }
            static const std::unordered_set<std::string> valid_otlp_keys = {
                "endpoint", "protocol", "semantics", "headers", "max_retries",
                "retry_backoff_base_s", "send_batch_size", "batch_timeout_s"
            };
            for (auto& [otlp_key, otlp_val] : otlp.items()) {
                if (valid_otlp_keys.find(otlp_key) == valid_otlp_keys.end()) {
                    throw std::invalid_argument("Unknown config key: 'telemetry.otlp." + otlp_key + "'");
                }
            }
            if (otlp.contains("endpoint") && !otlp["endpoint"].is_string()) {
                throw std::invalid_argument("'telemetry.otlp.endpoint' must be a string");
            }
            if (otlp.contains("protocol")) {
                if (!otlp["protocol"].is_string()) {
                    throw std::invalid_argument("'telemetry.otlp.protocol' must be a string");
                }
                std::string proto = otlp["protocol"].get<std::string>();
                if (proto != "http/protobuf" && proto != "http/json") {
                    throw std::invalid_argument("'telemetry.otlp.protocol' must be either 'http/protobuf', or 'http/json'");
                }
            }
            if (otlp.contains("semantics")) {
                if (!otlp["semantics"].is_array()) {
                    throw std::invalid_argument("'telemetry.otlp.semantics' must be an array");
                }
                for (const auto& item : otlp["semantics"]) {
                    if (!item.is_string()) {
                        throw std::invalid_argument("'telemetry.otlp.semantics' items must be strings");
                    }
                    std::string sem = item.get<std::string>();
                    if (sem != "openinference" && sem != "otel_genai") {
                        throw std::invalid_argument("'telemetry.otlp.semantics' items must be 'openinference' or 'otel_genai'");
                    }
                }
            }
            if (otlp.contains("headers") && !otlp["headers"].is_object()) {
                throw std::invalid_argument("'telemetry.otlp.headers' must be an object");
            }
            if (otlp.contains("max_retries")) {
                if (!otlp["max_retries"].is_number_integer()) {
                    throw std::invalid_argument("'telemetry.otlp.max_retries' must be an integer");
                }
                if (otlp["max_retries"].get<int>() < 0) {
                    throw std::invalid_argument("'telemetry.otlp.max_retries' must be >= 0");
                }
            }
            if (otlp.contains("retry_backoff_base_s")) {
                if (!otlp["retry_backoff_base_s"].is_number()) {
                    throw std::invalid_argument("'telemetry.otlp.retry_backoff_base_s' must be a number");
                }
                double base_val = otlp["retry_backoff_base_s"].get<double>();
                if (base_val < 0.0) {
                    throw std::invalid_argument("'telemetry.otlp.retry_backoff_base_s' must be >= 0");
                }
            }
            if (otlp.contains("send_batch_size")) {
                if (!otlp["send_batch_size"].is_number_integer()) {
                    throw std::invalid_argument("'telemetry.otlp.send_batch_size' must be an integer");
                }
                if (otlp["send_batch_size"].get<int>() < 1) {
                    throw std::invalid_argument("'telemetry.otlp.send_batch_size' must be >= 1");
                }
            }
            if (otlp.contains("batch_timeout_s")) {
                if (!otlp["batch_timeout_s"].is_number()) {
                    throw std::invalid_argument("'telemetry.otlp.batch_timeout_s' must be a number");
                }
                double to_val = otlp["batch_timeout_s"].get<double>();
                if (to_val <= 0.0) {
                    throw std::invalid_argument("'telemetry.otlp.batch_timeout_s' must be positive");
                }
            }
        }
        if (value.contains("session")) {
            const auto& sess = value["session"];
            if (!sess.is_object()) {
                throw std::invalid_argument("'telemetry.session' must be an object");
            }
            static const std::unordered_set<std::string> valid_session_keys = {
                "headers"
            };
            for (auto& [s_key, s_val] : sess.items()) {
                if (valid_session_keys.find(s_key) == valid_session_keys.end()) {
                    throw std::invalid_argument("Unknown config key: 'telemetry.session." + s_key + "'");
                }
            }
            if (sess.contains("headers")) {
                const auto& hdrs = sess["headers"];
                if (!hdrs.is_object()) {
                    throw std::invalid_argument("'telemetry.session.headers' must be an object");
                }
                static const std::unordered_set<std::string> valid_headers_keys = {
                    "id", "client"
                };
                for (auto& [h_key, h_val] : hdrs.items()) {
                    if (valid_headers_keys.find(h_key) == valid_headers_keys.end()) {
                        throw std::invalid_argument("Unknown config key: 'telemetry.session.headers." + h_key + "'");
                    }
                    if (h_val.is_array()) {
                        for (const auto& item : h_val) {
                            if (!item.is_string()) {
                                throw std::invalid_argument("'telemetry.session.headers." + h_key + "' elements must be strings");
                            }
                        }
                    } else if (!h_val.is_string()) {
                        throw std::invalid_argument("'telemetry.session.headers." + h_key + "' must be a string or array of strings");
                    }
                }
            }
        }
    } else if (is_backend_name(key)) {
        if (!value.is_object()) {
            throw std::invalid_argument("'" + key + "' must be an object");
        }
        // Validate each sub-key
        for (auto& [sub_key, sub_value] : value.items()) {
            validate_backend(key, sub_key, sub_value);
        }
    } else {
        throw std::invalid_argument("Unknown config key: '" + key + "'");
    }
}

void RuntimeConfig::validate_backend(const std::string& backend, const std::string& key,
                                      const json& value) const {
    if (key == "backend") {
        if (!value.is_string()) {
            throw std::invalid_argument("'" + backend + "." + key + "' must be a string");
        }
        validate_backend_choice(backend, value.get<std::string>());
    }
    else if (key == "args" || key.find("_args") != std::string::npos) {
        if (!value.is_string()) {
            throw std::invalid_argument("'" + backend + "." + key + "' must be a string");
        }
    }
    else if (key == "device") {
        if (!value.is_string()) {
            throw std::invalid_argument("'" + backend + "." + key + "' must be a string");
        }
    }
    else if (key.find("_bin") != std::string::npos) {
        if (!value.is_string()) {
            throw std::invalid_argument("'" + backend + "." + key + "' must be a string");
        }
        validate_bin_path(backend, key, value.get<std::string>());
    }
    else if (key == "prefer_system") {
        if (!value.is_boolean()) {
            throw std::invalid_argument("'" + backend + "." + key + "' must be a boolean");
        }
    }
    else if (key == "steps" || key == "width" || key == "height") {
        if (!value.is_number_integer()) {
            throw std::invalid_argument("'" + backend + "." + key + "' must be an integer");
        }
        if (value.get<int>() <= 0) {
            throw std::invalid_argument("'" + backend + "." + key + "' must be positive");
        }
    }
    else if (key == "cfg_scale") {
        if (!value.is_number()) {
            throw std::invalid_argument("'" + backend + "." + key + "' must be a number");
        }
        if (value.get<double>() <= 0.0) {
            throw std::invalid_argument("'" + backend + "." + key + "' must be positive");
        }
    }
    else if (key == "lora_dir" || key == "upscaler_dir") {
        if (!value.is_string()) {
            throw std::invalid_argument("'" + backend + "." + key + "' must be a string");
        }
        validate_path(backend, key, value.get<std::string>());
    }
    else {
        throw std::invalid_argument("Unknown key: '" + backend + "." + key + "'");
    }
}

void RuntimeConfig::apply_changes(const json& changes, json& applied_diff) {
    for (auto& [key, value] : changes.items()) {
        if (value.is_object() && is_backend_name(key)) {
            // Merge nested backend changes; record per-sub-key diffs.
            if (!config_.contains(key)) {
                config_[key] = json::object();
            }
            for (auto& [sub_key, sub_value] : value.items()) {
                if (!config_[key].contains(sub_key) || config_[key][sub_key] != sub_value) {
                    config_[key][sub_key] = sub_value;
                    if (!applied_diff.contains(key)) {
                        applied_diff[key] = json::object();
                    }
                    applied_diff[key][sub_key] = sub_value;
                }
            }
        } else if (key == "telemetry" && value.is_object()) {
            if (!config_.contains("telemetry") || !config_["telemetry"].is_object()) {
                config_["telemetry"] = json::object();
            }
            for (auto& [t_key, t_val] : value.items()) {
                if (t_key == "otlp" && t_val.is_object()) {
                    if (!config_["telemetry"].contains("otlp") || !config_["telemetry"]["otlp"].is_object()) {
                        config_["telemetry"]["otlp"] = json::object();
                    }
                    for (auto& [otlp_key, otlp_val] : t_val.items()) {
                        if (!config_["telemetry"]["otlp"].contains(otlp_key) || config_["telemetry"]["otlp"][otlp_key] != otlp_val) {
                            config_["telemetry"]["otlp"][otlp_key] = otlp_val;
                            if (!applied_diff.contains("telemetry")) {
                                applied_diff["telemetry"] = json::object();
                            }
                            if (!applied_diff["telemetry"].contains("otlp")) {
                                applied_diff["telemetry"]["otlp"] = json::object();
                            }
                            applied_diff["telemetry"]["otlp"][otlp_key] = otlp_val;
                        }
                    }
                } else if (t_key == "session" && t_val.is_object()) {
                    if (!config_["telemetry"].contains("session") || !config_["telemetry"]["session"].is_object()) {
                        config_["telemetry"]["session"] = json::object();
                    }
                    if (t_val.contains("headers") && t_val["headers"].is_object()) {
                        if (!config_["telemetry"]["session"].contains("headers") || !config_["telemetry"]["session"]["headers"].is_object()) {
                            config_["telemetry"]["session"]["headers"] = json::object();
                        }
                        for (auto& [h_key, h_val] : t_val["headers"].items()) {
                            if (!config_["telemetry"]["session"]["headers"].contains(h_key) || config_["telemetry"]["session"]["headers"][h_key] != h_val) {
                                config_["telemetry"]["session"]["headers"][h_key] = h_val;
                                if (!applied_diff.contains("telemetry")) {
                                    applied_diff["telemetry"] = json::object();
                                }
                                if (!applied_diff["telemetry"].contains("session")) {
                                    applied_diff["telemetry"]["session"] = json::object();
                                }
                                if (!applied_diff["telemetry"]["session"].contains("headers")) {
                                    applied_diff["telemetry"]["session"]["headers"] = json::object();
                                }
                                applied_diff["telemetry"]["session"]["headers"][h_key] = h_val;
                            }
                        }
                    }
                } else {
                    if (!config_["telemetry"].contains(t_key) || config_["telemetry"][t_key] != t_val) {
                        config_["telemetry"][t_key] = t_val;
                        if (!applied_diff.contains("telemetry")) {
                            applied_diff["telemetry"] = json::object();
                        }
                        applied_diff["telemetry"][t_key] = t_val;
                    }
                }
            }
        } else if (key == "no_broadcast") {
            bool bcast = !value.get<bool>();
            bool prev_effective_bcast = (broadcast_override_.has_value())
                ? *broadcast_override_
                : (config_.contains("broadcast") && config_["broadcast"].is_boolean() ? config_["broadcast"].get<bool>() : true);
            config_["broadcast"] = bcast;
            broadcast_override_ = std::nullopt;
            if (prev_effective_bcast != bcast) {
                applied_diff["broadcast"] = bcast;
            }
        } else if (key == "broadcast") {
            bool bcast = value.get<bool>();
            bool prev_effective_bcast = (broadcast_override_.has_value())
                ? *broadcast_override_
                : (config_.contains("broadcast") && config_["broadcast"].is_boolean() ? config_["broadcast"].get<bool>() : true);
            config_["broadcast"] = bcast;
            broadcast_override_ = std::nullopt;
            if (prev_effective_bcast != bcast) {
                applied_diff["broadcast"] = bcast;
            }
        } else if (key == "allowed_origins") {
            std::string prev_effective = allowed_origins_unlocked();
            std::string new_origins = value.is_string() ? value.get<std::string>() : "";
            config_["allowed_origins"] = new_origins;
            allowed_origins_override_ = new_origins;
            if (prev_effective != new_origins) {
                applied_diff["allowed_origins"] = new_origins;
            }
        } else {
            if (!config_.contains(key) || config_[key] != value) {
                config_[key] = value;
                applied_diff[key] = value;
            }
        }
    }
}

json RuntimeConfig::set(const json& changes, ConfigSideEffectCallback side_effect_cb) {
    if (!changes.is_object() || changes.empty()) {
        throw std::invalid_argument("Request body must be a non-empty JSON object");
    }

    auto [normalized_changes, message] = normalize_config_set_changes(changes);

    // Validate all keys before acquiring write lock
    for (auto& [key, value] : normalized_changes.items()) {
        validate(key, value);
    }

    json applied_diff = json::object();
    json updated = json::object();

    {
        std::unique_lock lock(mutex_);
        apply_changes(normalized_changes, applied_diff);

        // Build updated response
        for (auto& [key, value] : normalized_changes.items()) {
            updated[key] = value;
        }
    } // Lock released

    // Execute side effects outside the lock
    if (side_effect_cb && !applied_diff.empty()) {
        side_effect_cb(applied_diff);
    }

    json response = {{"status", "success"}, {"updated", updated}};
    if (!message.empty()) {
        response["message"] = message;
    }
    return response;
}

json RuntimeConfig::snapshot() const {
    std::shared_lock lock(mutex_);
    return config_;
}

bool RuntimeConfig::get_bool_opt(const char* env_name, const std::vector<std::string>& path, bool default_val) const {
    if (env_name) {
        if (const char* env = std::getenv(env_name)) {
            std::string s(env);
            return s == "1" || s == "true" || s == "True" || s == "TRUE" || s == "yes" || s == "YES";
        }
    }
    std::shared_lock lock(mutex_);
    const json* current = &config_;
    for (const auto& key : path) {
        if (!current->is_object() || !current->contains(key)) return default_val;
        current = &((*current)[key]);
    }
    return current->is_boolean() ? current->get<bool>() : default_val;
}

int RuntimeConfig::get_int_opt(const char* env_name, const std::vector<std::string>& path, int default_val) const {
    if (env_name) {
        if (const char* env = std::getenv(env_name)) {
            try { return std::stoi(env); } catch (...) {}
        }
    }
    std::shared_lock lock(mutex_);
    const json* current = &config_;
    for (const auto& key : path) {
        if (!current->is_object() || !current->contains(key)) return default_val;
        current = &((*current)[key]);
    }
    try {
        if (current->is_number_integer()) return current->get<int>();
        if (current->is_string()) return std::stoi(current->get<std::string>());
    } catch (...) {}
    return default_val;
}

double RuntimeConfig::get_double_opt(const char* env_name, const std::vector<std::string>& path, double default_val) const {
    if (env_name) {
        if (const char* env = std::getenv(env_name)) {
            try { return std::stod(env); } catch (...) {}
        }
    }
    std::shared_lock lock(mutex_);
    const json* current = &config_;
    for (const auto& key : path) {
        if (!current->is_object() || !current->contains(key)) return default_val;
        current = &((*current)[key]);
    }
    try {
        if (current->is_number()) return current->get<double>();
        if (current->is_string()) return std::stod(current->get<std::string>());
    } catch (...) {}
    return default_val;
}

std::string RuntimeConfig::get_string_opt(const char* env_name, const std::vector<std::string>& path, const std::string& default_val) const {
    if (env_name) {
        if (const char* env = std::getenv(env_name)) {
            return std::string(env);
        }
    }
    std::shared_lock lock(mutex_);
    const json* current = &config_;
    for (const auto& key : path) {
        if (!current->is_object() || !current->contains(key)) return default_val;
        current = &((*current)[key]);
    }
    return current->is_string() ? current->get<std::string>() : default_val;
}

} // namespace lemon
