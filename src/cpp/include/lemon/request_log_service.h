#pragma once

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace lemon {

struct RequestLogEntry {
    std::string client_ip;
    std::string forwarded_for;
    std::string method;
    std::string path;
    std::string query_string;
    int status_code = 0;
    int duration_ms = 0;
    std::string user_agent;
    std::string endpoint_type;
    std::string model;
    std::string keep_alive;
    std::optional<bool> stream;
    int request_body_bytes = 0;
    int response_body_bytes = 0;
    int prompt_chars = 0;
    int messages_chars = 0;
    std::string redacted_body_json;
    bool has_redacted_body = false;
    std::string error;
    std::string request_body;
};

class RequestLogService {
public:
    static std::unique_ptr<RequestLogService> from_env();

    RequestLogService(bool enabled,
                      std::string database_url,
                      int retention_days,
                      bool log_prompts,
                      bool database_available);

    ~RequestLogService();

    RequestLogService(const RequestLogService&) = delete;
    RequestLogService& operator=(const RequestLogService&) = delete;

    bool is_enabled() const { return enabled_; }
    bool is_database_available() const { return database_available_.load(); }

    void start();
    void stop();

    void mark_request_start();
    void log_response(const httplib::Request& req, const httplib::Response& res);

    nlohmann::json get_recent(int limit) const;
    nlohmann::json search(const httplib::Request& req) const;
    nlohmann::json get_stats(const httplib::Request& req) const;

private:
    void writer_loop();
    void purge_loop();
    bool ensure_connection();
    void close_connection();
    bool init_schema();
    bool insert_entries(const std::vector<RequestLogEntry>& entries);
    void run_purge();
    nlohmann::json row_to_json(int row) const;

#ifdef LEMONADE_HAVE_REQUEST_LOG
    mutable std::mutex db_mutex_;
    void* pg_conn_ = nullptr; // PGconn*, opaque to avoid header dependency
#endif

    bool enabled_;
    std::string database_url_;
    int retention_days_;
    bool log_prompts_;
    std::atomic<bool> database_available_;

    std::atomic<bool> running_{false};
    std::thread writer_thread_;
    std::thread purge_thread_;

    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<RequestLogEntry> queue_;
    static constexpr size_t kMaxQueueSize = 10000;

    std::atomic<int64_t> last_drop_warning_ms_{0};
};

void request_log_mark_start();
int64_t request_log_elapsed_ms();

} // namespace lemon
