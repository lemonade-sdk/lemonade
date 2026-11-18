#include "usage_log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "lemon/runtime_config.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/path_utils.h"
#include "lemon/version.h"

namespace fs = std::filesystem;

namespace lemon::usage {

namespace {

// A failed or interrupted write can leave half a record at the end of the file.
// Appending after it would make the next record unreadable too.
void trim_partial_record(const fs::path& path) {
    std::error_code ec;
    uint64_t size = fs::file_size(path, ec);
    if (ec || size == 0) return;
    uint64_t keep = 0;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return;
        char buf[4096];
        uint64_t end = size;
        bool found = false;
        while (end > 0 && !found) {
            uint64_t start = end > sizeof(buf) ? end - sizeof(buf) : 0;
            in.seekg(static_cast<std::streamoff>(start));
            in.read(buf, static_cast<std::streamsize>(end - start));
            if (!in) return;
            for (uint64_t i = end - start; i > 0; --i) {
                if (buf[i - 1] == '\n') {
                    keep = start + i;
                    found = true;
                    break;
                }
            }
            end = start;
        }
    }
    if (keep == size) return;
    LOG(WARNING, "UsageLog") << "Removing an incomplete record from the end of " << utils::path_to_utf8(path)
                             << std::endl;
    fs::resize_file(path, keep, ec);
}

std::string local_hostname() {
#ifdef _WIN32
    char buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD len = sizeof(buf);
    if (GetComputerNameA(buf, &len)) return std::string(buf, len);
#else
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) return buf;
#endif
    return "unknown";
}

std::tm utc_tm(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return tm;
}

std::string format_utc(std::time_t t, const char* fmt) {
    std::tm tm = utc_tm(t);
    char buf[32];
    std::strftime(buf, sizeof(buf), fmt, &tm);
    return buf;
}

std::string iso_timestamp(int64_t unix_ms) {
    char ms[8];
    std::snprintf(ms, sizeof(ms), ".%03dZ", static_cast<int>(unix_ms % 1000));
    return format_utc(static_cast<std::time_t>(unix_ms / 1000), "%Y-%m-%dT%H:%M:%S") + ms;
}

std::string utc_date_days_ago(int days) {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) - std::time_t(days) * 86400;
    // gmtime_s aborts the process on negative input, and max_days is user-set.
    return format_utc(std::max<std::time_t>(t, 0), "%Y-%m-%d");
}

nlohmann::ordered_json count_or_null(int n) {
    return n >= 0 ? nlohmann::ordered_json(n) : nlohmann::ordered_json(nullptr);
}

nlohmann::ordered_json string_or_null(const std::string& s) {
    return s.empty() ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(s);
}

nlohmann::ordered_json ms_or_null(double ms) {
    return ms > 0 ? nlohmann::ordered_json(std::llround(ms)) : nlohmann::ordered_json(nullptr);
}

nlohmann::ordered_json rate_or_null(double rate) {
    return rate > 0 ? nlohmann::ordered_json(std::round(rate * 10) / 10) : nlohmann::ordered_json(nullptr);
}

std::string file_name(const std::string& date) {
    return "usage-" + date + ".jsonl";
}

struct LogFile {
    fs::path path;
    std::string date;
    uint64_t size;
};

// Oldest first.
std::vector<LogFile> list_log_files(const std::string& dir) {
    static const std::regex pattern(R"(usage-(\d{4}-\d{2}-\d{2})\.jsonl)");
    std::vector<LogFile> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(utils::path_from_utf8(dir), ec)) {
        std::smatch m;
        std::string name = entry.path().filename().string();
        if (!entry.is_regular_file(ec) || !std::regex_match(name, m, pattern)) continue;
        // Not entry.file_size(): NTFS directory entries report stale sizes for recently written files.
        uint64_t size = fs::file_size(entry.path(), ec);
        files.push_back({entry.path(), m[1], ec ? 0 : size});
    }
    std::sort(files.begin(), files.end(), [](const LogFile& a, const LogFile& b) { return a.date < b.date; });
    return files;
}

} // namespace

nlohmann::ordered_json make_record(const UsageEvent& e, bool include_content) {
    static const std::string host = local_hostname();
    nlohmann::ordered_json r = {
        {"ts", iso_timestamp(e.end_unix_ms)},
        {"host", host},
        {"lemonade_version", LEMON_VERSION_STRING},
        {"request", e.request},
        {"model", e.model},
        {"backend", string_or_null(e.backend)},
        {"device", string_or_null(e.device)},
        {"stream", e.stream},
        {"status", e.status},
        {"duration_ms", e.duration_ms},
        {"ttft_ms", ms_or_null(e.ttft_ms)},
        {"tokens_per_second", rate_or_null(e.tokens_per_second)},
        {"input_tokens", count_or_null(e.input_tokens)},
        {"output_tokens", count_or_null(e.output_tokens)},
        {"cached_tokens", count_or_null(e.cached_tokens)},
        {"tokens_reported", !e.output_estimated && (e.input_tokens >= 0 || e.output_tokens >= 0)},
        {"tokens_estimated", e.output_estimated},
        {"session_id", string_or_null(e.session_id)},
        {"client_ip", string_or_null(e.client_ip)},
    };
    if (include_content) {
        r["input"] = e.input;
        r["output"] = e.output;
    }
    return r;
}

UsageLog::UsageLog(std::function<UsageLogOptions()> options, size_t queue_capacity,
                   std::chrono::milliseconds retention_interval, std::chrono::milliseconds retry_interval)
    : options_(std::move(options)), capacity_(queue_capacity), retention_interval_(retention_interval),
      retry_interval_(retry_interval) {}

UsageLog::~UsageLog() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void UsageLog::start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        busy_ = true;
    }
    worker_ = std::thread(&UsageLog::run, this);
}

void UsageLog::push(nlohmann::ordered_json record) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failed_) return;
        if (queue_.size() >= capacity_) {
            ++dropped_;
            return;
        }
        queue_.push_back(std::move(record));
    }
    cv_.notify_one();
}

void UsageLog::flush() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_idle_.wait(lock, [this] { return queue_.empty() && dropped_ == 0 && !busy_; });
}

bool UsageLog::failed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failed_;
}

uint64_t UsageLog::disk_usage_bytes() const {
    return disk_bytes_;
}

void UsageLog::run() {
    // Apply retention and seed disk_usage_bytes() before the first record arrives.
    try {
        prune(options_());
    } catch (const std::exception& e) {
        LOG(WARNING, "UsageLog") << "Usage log retention check failed: " << e.what() << std::endl;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        busy_ = false;
    }
    cv_idle_.notify_all();

    for (;;) {
        std::deque<nlohmann::ordered_json> batch;
        uint64_t dropped = 0;
        bool active = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // The timeout runs age retention while no requests arrive, and
            // retries a failed log, which may have a new path or free disk by then.
            bool woken = cv_.wait_for(lock, failed_ ? retry_interval_ : retention_interval_,
                                      [this] { return stop_ || !queue_.empty() || dropped_ > 0; });
            if (stop_ && queue_.empty() && dropped_ == 0) return;
            if (failed_ && !woken) {
                failed_ = false;
                LOG(INFO, "UsageLog") << "Retrying the usage log after a write failure" << std::endl;
            }
            batch.swap(queue_);
            dropped = std::exchange(dropped_, 0);
            active = !failed_;
            busy_ = true;
        }

        if (active) {
            try {
                auto opts = options_();
                for (const auto& record : batch) write(opts, record);
                if (dropped > 0) write(opts, {{"dropped", dropped}});
                if (file_.is_open()) {
                    file_.flush();
                    if (!file_) throw std::runtime_error("cannot write " + file_path_);
                }
                prune(opts);
            } catch (const std::exception& e) {
                LOG(WARNING, "UsageLog") << "Usage log paused after write failure: " << e.what() << std::endl;
                file_.close();
                std::lock_guard<std::mutex> lock(mutex_);
                failed_ = true;
                queue_.clear();
                dropped_ = 0;
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            busy_ = false;
        }
        cv_idle_.notify_all();
    }
}

void UsageLog::write(const UsageLogOptions& opts, const nlohmann::ordered_json& record) {
    std::string line = record.dump() + "\n";
    std::string date = record.contains("ts") ? record["ts"].get<std::string>().substr(0, 10)
                     : !file_date_.empty() ? file_date_
                     : utc_date_days_ago(0);

    if (!file_.is_open() || date != file_date_ || opts.dir != file_dir_) {
        file_.close();
        // Records can hold prompts, so what the log creates is readable by its owner only.
        // A directory that already exists keeps its permissions.
        fs::path dir = utils::path_from_utf8(opts.dir);
        std::error_code ec;
        if (fs::create_directories(dir)) {
            fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
        }
        fs::path path = dir / file_name(date);
        bool new_file = !fs::exists(path, ec);
        if (new_file) {
#ifndef _WIN32
            // Create with 0600 so the file is never readable by others, even briefly.
            int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (fd >= 0) ::close(fd);
#endif
        } else {
            trim_partial_record(path);
        }
        file_.open(path, std::ios::binary | std::ios::app);
        if (!file_) throw std::runtime_error("cannot open " + utils::path_to_utf8(path));
        if (new_file) {
            fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
        }
        file_path_ = utils::path_to_utf8(path);
        file_date_ = date;
        file_dir_ = opts.dir;
    }

    file_ << line;
    if (!file_) throw std::runtime_error("cannot write " + file_path_);
}

// Deletes whole days, oldest first. The size cap never deletes the newest day,
// so one heavy day can exceed max_total_bytes on its own.
void UsageLog::prune(const UsageLogOptions& opts) {
    auto files = list_log_files(opts.dir);
    std::string cutoff = opts.max_days > 0 ? utc_date_days_ago(opts.max_days) : "";
    uint64_t total = 0;
    for (const auto& f : files) total += f.size;

    for (size_t i = 0; i < files.size(); ++i) {
        bool newest = i + 1 == files.size();
        bool too_old = files[i].date < cutoff;
        bool over_cap = !newest && opts.max_total_bytes > 0 && total > opts.max_total_bytes;
        if (!too_old && !over_cap) continue;
        if (file_.is_open() && utils::path_to_utf8(files[i].path) == file_path_) {
            file_.close();
            file_date_.clear();
        }
        std::error_code ec;
        if (fs::remove(files[i].path, ec)) total -= files[i].size;
    }
    disk_bytes_ = total;
}

namespace {

UsageLogOptions options_from_config() {
    UsageLogOptions opts;
    auto* config = RuntimeConfig::global();
    if (!config) return opts;
    opts.dir = config->telemetry_usage_log_path();
    if (opts.dir.empty()) opts.dir = utils::path_to_utf8(utils::path_from_utf8(utils::get_config_dir()) / "usage");
    int max_mb = config->telemetry_usage_log_max_size_mb();
    int max_days = config->telemetry_usage_log_max_days();
    opts.max_total_bytes = max_mb > 0 ? static_cast<uint64_t>(max_mb) * 1024 * 1024 : 0;
    opts.max_days = max_days > 0 ? max_days : 0;
    return opts;
}

UsageLog& instance() {
    static UsageLog log(options_from_config);
    static std::once_flag started;
    std::call_once(started, [] { log.start(); });
    return log;
}

bool content_enabled() {
    auto* config = RuntimeConfig::global();
    return config && config->telemetry_usage_log_content() == "full";
}

} // namespace

bool enabled() {
    auto* config = RuntimeConfig::global();
    return config && config->telemetry_usage_log_enabled();
}

void record(const UsageEvent& event) {
    if (enabled()) instance().push(make_record(event, content_enabled()));
}

nlohmann::json status() {
    nlohmann::json s = {{"enabled", enabled()}};
    if (!s["enabled"]) return s;
    s["path"] = options_from_config().dir;
    s["disk_usage_bytes"] = instance().disk_usage_bytes();
    s["failed"] = instance().failed();
    return s;
}

void shutdown() {
    if (enabled()) instance().flush();
}

} // namespace lemon::usage
