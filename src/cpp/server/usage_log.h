#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace lemon::usage {

// Token counts use -1 for "not reported by the backend".
struct UsageEvent {
    int64_t end_unix_ms = 0;
    std::string request;
    std::string model;
    std::string backend;
    std::string device;
    bool stream = false;
    std::string status;
    int64_t duration_ms = 0;
    int input_tokens = -1;
    int output_tokens = -1;
    int cached_tokens = -1;
    bool output_estimated = false;
    std::string session_id;
    std::string client_ip;
    std::string input;
    std::string output;
};

nlohmann::ordered_json make_record(const UsageEvent& event, bool include_content);

struct UsageLogOptions {
    std::string dir;
    uint64_t max_total_bytes = 0;
    int max_days = 0;
    uint64_t max_file_bytes = 10 * 1024 * 1024;
};

// Appends JSON lines to usage-YYYY-MM-DD[.N].jsonl on a background thread.
// Options are re-read for every batch so config changes apply without restart.
class UsageLog {
public:
    explicit UsageLog(std::function<UsageLogOptions()> options, size_t queue_capacity = 10000);
    ~UsageLog();

    void start();
    void push(nlohmann::ordered_json record);
    void flush();
    bool failed() const;

private:
    void run();
    void write(const UsageLogOptions& opts, const nlohmann::ordered_json& record);
    void prune(const UsageLogOptions& opts);

    std::function<UsageLogOptions()> options_;
    size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable cv_idle_;
    std::deque<nlohmann::ordered_json> queue_;
    uint64_t dropped_ = 0;
    bool busy_ = false;
    bool stop_ = false;
    bool failed_ = false;
    std::thread worker_;

    std::ofstream file_;
    std::string file_path_;
    std::string file_dir_;
    std::string file_date_;
    int file_part_ = 0;
    uint64_t file_bytes_ = 0;
};

bool enabled();
void record(const UsageEvent& event);
nlohmann::json status();
void shutdown();

} // namespace lemon::usage
