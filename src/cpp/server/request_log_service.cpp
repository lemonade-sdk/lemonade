#include "lemon/request_log_service.h"

#include "lemon/request_log_parser.h"
#include "lemon/utils/aixlog.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>
#include <vector>

#ifdef LEMONADE_HAVE_REQUEST_LOG
#include <libpq-fe.h>
#endif

namespace lemon {
namespace {

thread_local std::chrono::steady_clock::time_point g_request_log_start;
thread_local bool g_request_log_start_valid = false;

bool parse_bool_env(const char* value, bool default_value) {
    if (!value || !*value) {
        return default_value;
    }
    std::string normalized(value);
    for (char& ch : normalized) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (normalized == "1" || normalized == "true" || normalized == "yes" ||
        normalized == "on") {
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "no" ||
        normalized == "off") {
        return false;
    }
    return default_value;
}

bool encoding_contains(const std::string& encoding, const char* token) {
    return encoding.find(token) != std::string::npos;
}

bool is_gzip_payload(const std::string& body) {
    return body.size() >= 2 && static_cast<unsigned char>(body[0]) == 0x1F &&
           static_cast<unsigned char>(body[1]) == 0x8B;
}

std::string decompress_with(httplib::decompressor& decompressor, const std::string& body) {
    if (!decompressor.is_valid() || body.empty()) {
        return body;
    }
    std::string out;
    const bool ok = decompressor.decompress(
        body.data(), body.size(), [&](const char* data, size_t len) {
            out.append(data, len);
            return true;
        });
    return ok && !out.empty() ? out : body;
}

std::string prepare_response_body_for_logging(const httplib::Response& res) {
    const std::string& body = res.body;
    if (body.empty()) {
        return body;
    }

    std::string encoding;
    if (res.has_header("Content-Encoding")) {
        encoding = res.get_header_value("Content-Encoding");
    }

    if (!encoding.empty()) {
#ifdef CPPHTTPLIB_ZLIB_SUPPORT
        if (encoding_contains(encoding, "gzip") || encoding_contains(encoding, "deflate")) {
            httplib::gzip_decompressor decompressor;
            const std::string decoded = decompress_with(decompressor, body);
            if (decoded != body) {
                return decoded;
            }
        }
#endif
#ifdef CPPHTTPLIB_BROTLI_SUPPORT
        if (encoding_contains(encoding, "br")) {
            httplib::brotli_decompressor decompressor;
            const std::string decoded = decompress_with(decompressor, body);
            if (decoded != body) {
                return decoded;
            }
        }
#endif
#ifdef CPPHTTPLIB_ZSTD_SUPPORT
        if (encoding_contains(encoding, "zstd")) {
            httplib::zstd_decompressor decompressor;
            const std::string decoded = decompress_with(decompressor, body);
            if (decoded != body) {
                return decoded;
            }
        }
#endif
    }

    if (is_gzip_payload(body)) {
#ifdef CPPHTTPLIB_ZLIB_SUPPORT
        httplib::gzip_decompressor decompressor;
        const std::string decoded = decompress_with(decompressor, body);
        if (decoded != body) {
            return decoded;
        }
#endif
    }

    return body;
}

int parse_int_env(const char* value, int default_value) {
    if (!value || !*value) {
        return default_value;
    }
    try {
        return std::stoi(value);
    } catch (...) {
        return default_value;
    }
}

std::string query_param(const httplib::Request& req, const char* key) {
    if (req.has_param(key)) {
        return req.get_param_value(key);
    }
    return {};
}

int clamp_limit(const std::string& raw, int default_value, int max_value) {
    if (raw.empty()) {
        return default_value;
    }
    try {
        const int value = std::stoi(raw);
        if (value < 1) {
            return 1;
        }
        return std::min(value, max_value);
    } catch (...) {
        return default_value;
    }
}

int clamp_offset(const std::string& raw) {
    if (raw.empty()) {
        return 0;
    }
    try {
        const int value = std::stoi(raw);
        return value < 0 ? 0 : value;
    } catch (...) {
        return 0;
    }
}

std::string parse_since_timestamp(const std::string& since) {
    if (since.empty()) {
        return {};
    }

    if (since.size() >= 2) {
        const char unit = since.back();
        try {
            const int amount = std::stoi(since.substr(0, since.size() - 1));
            if (amount <= 0) {
                return {};
            }
            std::chrono::system_clock::time_point cutoff;
            if (unit == 'h' || unit == 'H') {
                cutoff = std::chrono::system_clock::now() - std::chrono::hours(amount);
            } else if (unit == 'd' || unit == 'D') {
                cutoff = std::chrono::system_clock::now() - std::chrono::hours(24 * amount);
            } else {
                return since;
            }
            const auto time = std::chrono::system_clock::to_time_t(cutoff);
            std::tm tm_buf{};
#ifdef _WIN32
            gmtime_s(&tm_buf, &time);
#else
            gmtime_r(&time, &tm_buf);
#endif
            char buffer[64];
            std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
            return buffer;
        } catch (...) {
            return since;
        }
    }

    return since;
}

#ifdef LEMONADE_HAVE_REQUEST_LOG
PGconn* as_pg_conn(void* conn) {
    return static_cast<PGconn*>(conn);
}

const char* nullable_cstr(const std::string& value) {
    return value.empty() ? nullptr : value.c_str();
}

const char* stream_param(const std::optional<bool>& stream) {
    if (!stream.has_value()) {
        return nullptr;
    }
    return stream.value() ? "true" : "false";
}

const char* kSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS request_logs (
  id BIGSERIAL PRIMARY KEY,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  client_ip TEXT,
  forwarded_for TEXT,
  method TEXT NOT NULL,
  path TEXT NOT NULL,
  query_string TEXT,
  status_code INTEGER,
  duration_ms INTEGER,
  user_agent TEXT,
  endpoint_type TEXT,
  model TEXT,
  keep_alive TEXT,
  stream BOOLEAN,
  request_body_bytes INTEGER,
  response_body_bytes INTEGER,
  prompt_chars INTEGER,
  messages_chars INTEGER,
  prompt_tokens INTEGER,
  completion_tokens INTEGER,
  redacted_body JSONB,
  redacted_response JSONB,
  error TEXT
);
CREATE INDEX IF NOT EXISTS idx_request_logs_created_at ON request_logs (created_at);
CREATE INDEX IF NOT EXISTS idx_request_logs_model ON request_logs (model);
CREATE INDEX IF NOT EXISTS idx_request_logs_client_ip ON request_logs (client_ip);
CREATE INDEX IF NOT EXISTS idx_request_logs_path ON request_logs (path);
CREATE INDEX IF NOT EXISTS idx_request_logs_keep_alive ON request_logs (keep_alive);
ALTER TABLE request_logs ADD COLUMN IF NOT EXISTS prompt_tokens INTEGER;
ALTER TABLE request_logs ADD COLUMN IF NOT EXISTS completion_tokens INTEGER;
ALTER TABLE request_logs ADD COLUMN IF NOT EXISTS redacted_response JSONB;
)SQL";

nlohmann::json row_to_json_from_result(PGresult* result, int row) {
    auto value = [result, row](int column) -> std::string {
        const char* raw = PQgetvalue(result, row, column);
        return raw ? raw : "";
    };

    nlohmann::json entry = {
        {"id", std::stoll(value(0))},
        {"created_at", value(1)},
        {"client_ip", value(2)},
        {"forwarded_for", value(3)},
        {"method", value(4)},
        {"path", value(5)},
        {"query_string", value(6)},
        {"status_code", value(7).empty() ? nlohmann::json(nullptr) : nlohmann::json(std::stoi(value(7)))},
        {"duration_ms", value(8).empty() ? nlohmann::json(nullptr) : nlohmann::json(std::stoi(value(8)))},
        {"user_agent", value(9)},
        {"endpoint_type", value(10)},
        {"model", value(11)},
        {"keep_alive", value(12).empty() ? nlohmann::json(nullptr) : nlohmann::json(value(12))},
        {"stream", value(13).empty() ? nlohmann::json(nullptr) : nlohmann::json(value(13) == "t")},
        {"request_body_bytes", value(14).empty() ? nlohmann::json(nullptr) : nlohmann::json(std::stoi(value(14)))},
        {"response_body_bytes", value(15).empty() ? nlohmann::json(nullptr) : nlohmann::json(std::stoi(value(15)))},
        {"prompt_chars", value(16).empty() ? nlohmann::json(nullptr) : nlohmann::json(std::stoi(value(16)))},
        {"messages_chars", value(17).empty() ? nlohmann::json(nullptr) : nlohmann::json(std::stoi(value(17)))},
        {"prompt_tokens", value(18).empty() ? nlohmann::json(nullptr) : nlohmann::json(std::stoi(value(18)))},
        {"completion_tokens", value(19).empty() ? nlohmann::json(nullptr) : nlohmann::json(std::stoi(value(19)))},
        {"error", value(22).empty() ? nlohmann::json(nullptr) : nlohmann::json(value(22))},
    };

    const char* redacted = PQgetvalue(result, row, 20);
    if (redacted && *redacted) {
        try {
            entry["redacted_body"] = nlohmann::json::parse(redacted);
        } catch (...) {
            entry["redacted_body"] = redacted;
        }
    } else {
        entry["redacted_body"] = nullptr;
    }

    const char* redacted_response = PQgetvalue(result, row, 21);
    if (redacted_response && *redacted_response) {
        try {
            entry["redacted_response"] = nlohmann::json::parse(redacted_response);
        } catch (...) {
            entry["redacted_response"] = redacted_response;
        }
    } else {
        entry["redacted_response"] = nullptr;
    }
    return entry;
}

#endif

} // namespace

void request_log_mark_start() {
    g_request_log_start = std::chrono::steady_clock::now();
    g_request_log_start_valid = true;
}

int64_t request_log_elapsed_ms() {
    if (!g_request_log_start_valid) {
        return 0;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - g_request_log_start)
        .count();
}

std::unique_ptr<RequestLogService> RequestLogService::from_env() {
    const bool enabled =
        parse_bool_env(std::getenv("LEMONADE_REQUEST_LOG_ENABLED"), false);
    const char* database_url_env = std::getenv("LEMONADE_REQUEST_LOG_DATABASE_URL");
    const std::string database_url = database_url_env ? database_url_env : "";
    const int retention_days =
        parse_int_env(std::getenv("LEMONADE_REQUEST_LOG_RETENTION_DAYS"), 30);
    const bool log_prompts = parse_bool_env(std::getenv("LEMONADE_LOG_PROMPTS"), false);

#ifndef LEMONADE_HAVE_REQUEST_LOG
    if (enabled) {
        LOG(WARNING, "RequestLog")
            << "LEMONADE_REQUEST_LOG_ENABLED is set but lemond was built without libpq "
               "support. Request logging is disabled." << std::endl;
    }
    return nullptr;
#else
    if (!enabled) {
        return nullptr;
    }
    if (database_url.empty()) {
        LOG(WARNING, "RequestLog")
            << "LEMONADE_REQUEST_LOG_ENABLED is true but "
               "LEMONADE_REQUEST_LOG_DATABASE_URL is empty. Request logging is disabled."
            << std::endl;
        return nullptr;
    }

    auto service = std::unique_ptr<RequestLogService>(new RequestLogService(
        true, database_url, retention_days, log_prompts, false));
    if (!service->ensure_connection() || !service->init_schema()) {
        LOG(WARNING, "RequestLog")
            << "Failed to connect to PostgreSQL request log database. "
               "Lemonade will continue serving requests without persistence."
            << std::endl;
        service->database_available_.store(false);
    } else {
        service->database_available_.store(true);
        LOG(INFO, "RequestLog") << "PostgreSQL request logging enabled." << std::endl;
    }
    return service;
#endif
}

RequestLogService::RequestLogService(bool enabled,
                                     std::string database_url,
                                     int retention_days,
                                     bool log_prompts,
                                     bool database_available)
    : enabled_(enabled),
      database_url_(std::move(database_url)),
      retention_days_(retention_days),
      log_prompts_(log_prompts),
      database_available_(database_available) {}

RequestLogService::~RequestLogService() {
    stop();
#ifdef LEMONADE_HAVE_REQUEST_LOG
    close_connection();
#endif
}

void RequestLogService::start() {
    if (!enabled_ || running_.exchange(true)) {
        return;
    }
    writer_thread_ = std::thread(&RequestLogService::writer_loop, this);
    purge_thread_ = std::thread(&RequestLogService::purge_loop, this);
}

void RequestLogService::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    queue_cv_.notify_all();
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }
    if (purge_thread_.joinable()) {
        purge_thread_.join();
    }
}

void RequestLogService::mark_request_start() {
    request_log_mark_start();
}

void RequestLogService::log_response(const httplib::Request& req,
                                     const httplib::Response& res) {
    if (!enabled_ || should_skip_request_log_path(req.path, req.method)) {
        return;
    }

    RequestLogEntry entry;
    entry.client_ip = req.remote_addr;
    entry.forwarded_for = extract_forwarded_for(
        req.has_header("X-Forwarded-For") ? req.get_header_value("X-Forwarded-For") : "",
        req.has_header("X-Real-IP") ? req.get_header_value("X-Real-IP") : "",
        req.has_header("Forwarded") ? req.get_header_value("Forwarded") : "");
    entry.method = req.method;
    entry.path = req.path;
    entry.query_string = req.target;
    const auto query_pos = entry.query_string.find('?');
    if (query_pos != std::string::npos) {
        entry.query_string = entry.query_string.substr(query_pos + 1);
    } else {
        entry.query_string.clear();
    }
    entry.status_code = res.status;
    entry.duration_ms = static_cast<int>(request_log_elapsed_ms());
    entry.user_agent = req.has_header("User-Agent") ? req.get_header_value("User-Agent") : "";
    entry.endpoint_type = classify_endpoint_type(req.path, req.method);
    entry.request_body_bytes = static_cast<int>(req.body.size());
    entry.response_body_bytes = static_cast<int>(res.body.size());
    const std::string response_body = prepare_response_body_for_logging(res);
    entry.error = extract_response_error(response_body, res.status);
    entry.request_body = req.body;
    entry.response_body = response_body;

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (queue_.size() >= kMaxQueueSize) {
            const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count();
            const int64_t last = last_drop_warning_ms_.load();
            if (now_ms - last > 5000) {
                last_drop_warning_ms_.store(now_ms);
                LOG(WARNING, "RequestLog")
                    << "Request log queue is full; dropping log entries." << std::endl;
            }
            return;
        }
        queue_.push_back(std::move(entry));
    }
    queue_cv_.notify_one();
}

#ifdef LEMONADE_HAVE_REQUEST_LOG

bool RequestLogService::ensure_connection() {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (pg_conn_ && PQstatus(as_pg_conn(pg_conn_)) == CONNECTION_OK) {
        return true;
    }
    close_connection();
    pg_conn_ = PQconnectdb(database_url_.c_str());
    if (!pg_conn_ || PQstatus(as_pg_conn(pg_conn_)) != CONNECTION_OK) {
        if (pg_conn_) {
            LOG(WARNING, "RequestLog")
                << "PostgreSQL connection failed: " << PQerrorMessage(as_pg_conn(pg_conn_))
                << std::endl;
        }
        close_connection();
        database_available_.store(false);
        return false;
    }
    database_available_.store(true);
    return true;
}

void RequestLogService::close_connection() {
    if (pg_conn_) {
        PQfinish(as_pg_conn(pg_conn_));
        pg_conn_ = nullptr;
    }
    schema_initialized_ = false;
}

bool RequestLogService::init_schema() {
    if (schema_initialized_) {
        return true;
    }
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!pg_conn_ || PQstatus(as_pg_conn(pg_conn_)) != CONNECTION_OK) {
        return false;
    }
    PGresult* result = PQexec(as_pg_conn(pg_conn_), kSchemaSql);
    const bool ok = result && PQresultStatus(result) == PGRES_COMMAND_OK;
    if (!ok && result) {
        LOG(WARNING, "RequestLog")
            << "Failed to initialize request log schema: "
            << PQerrorMessage(as_pg_conn(pg_conn_)) << std::endl;
    }
    if (result) {
        PQclear(result);
    }
    if (ok) {
        schema_initialized_ = true;
    }
    return ok;
}

bool RequestLogService::insert_entries(const std::vector<RequestLogEntry>& entries) {
    if (entries.empty()) {
        return true;
    }
    if (!ensure_connection() || !init_schema()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!pg_conn_ || PQstatus(as_pg_conn(pg_conn_)) != CONNECTION_OK) {
        return false;
    }

    PGresult* begin = PQexec(as_pg_conn(pg_conn_), "BEGIN");
    if (!begin || PQresultStatus(begin) != PGRES_COMMAND_OK) {
        if (begin) {
            PQclear(begin);
        }
        database_available_.store(false);
        return false;
    }
    PQclear(begin);

    const char* insert_sql =
        "INSERT INTO request_logs (client_ip, forwarded_for, method, path, query_string, "
        "status_code, duration_ms, user_agent, endpoint_type, model, keep_alive, stream, "
        "request_body_bytes, response_body_bytes, prompt_chars, messages_chars, "
        "prompt_tokens, completion_tokens, redacted_body, redacted_response, error) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,$19,$20,$21)";

    bool ok = true;
    for (const auto& raw_entry : entries) {
        try {
        RequestLogEntry entry = raw_entry;
        ParsedRequestBody parsed =
            parse_request_body(entry.request_body, entry.path, log_prompts_);
        ParsedResponseBody parsed_response = parse_response_body(
            entry.response_body, entry.path, entry.status_code, log_prompts_);
        entry.model = parsed.model;
        entry.keep_alive = parsed.keep_alive;
        entry.stream = parsed.stream;
        entry.prompt_chars = parsed.prompt_chars;
        entry.messages_chars = parsed.messages_chars;
        entry.prompt_tokens = parsed_response.prompt_tokens;
        entry.completion_tokens = parsed_response.completion_tokens;
        if (parsed.has_redacted_body) {
            entry.redacted_body_json = safe_json_dump_for_db(parsed.redacted_body);
            entry.has_redacted_body = true;
        }
        if (parsed_response.has_redacted_response) {
            entry.redacted_response_json =
                safe_json_dump_for_db(parsed_response.redacted_response);
            entry.has_redacted_response = true;
        }

        entry.client_ip = sanitize_utf8_for_db(std::move(entry.client_ip));
        entry.forwarded_for = sanitize_utf8_for_db(std::move(entry.forwarded_for));
        entry.method = sanitize_utf8_for_db(std::move(entry.method));
        entry.path = sanitize_utf8_for_db(std::move(entry.path));
        entry.query_string = sanitize_utf8_for_db(std::move(entry.query_string));
        entry.user_agent = sanitize_utf8_for_db(std::move(entry.user_agent));
        entry.endpoint_type = sanitize_utf8_for_db(std::move(entry.endpoint_type));
        entry.model = sanitize_utf8_for_db(std::move(entry.model));
        entry.keep_alive = sanitize_utf8_for_db(std::move(entry.keep_alive));
        entry.error = sanitize_utf8_for_db(std::move(entry.error));

        const std::string status_code = std::to_string(entry.status_code);
        const std::string duration_ms = std::to_string(entry.duration_ms);
        const std::string request_body_bytes = std::to_string(entry.request_body_bytes);
        const std::string response_body_bytes = std::to_string(entry.response_body_bytes);
        const std::string prompt_chars = std::to_string(entry.prompt_chars);
        const std::string messages_chars = std::to_string(entry.messages_chars);
        std::string prompt_tokens;
        std::string completion_tokens;
        if (entry.prompt_tokens.has_value()) {
            prompt_tokens = std::to_string(entry.prompt_tokens.value());
        }
        if (entry.completion_tokens.has_value()) {
            completion_tokens = std::to_string(entry.completion_tokens.value());
        }

        const char* params[21] = {
            nullable_cstr(entry.client_ip),
            nullable_cstr(entry.forwarded_for),
            entry.method.c_str(),
            entry.path.c_str(),
            nullable_cstr(entry.query_string),
            status_code.c_str(),
            duration_ms.c_str(),
            nullable_cstr(entry.user_agent),
            nullable_cstr(entry.endpoint_type),
            nullable_cstr(entry.model),
            nullable_cstr(entry.keep_alive),
            stream_param(entry.stream),
            request_body_bytes.c_str(),
            response_body_bytes.c_str(),
            prompt_chars.c_str(),
            messages_chars.c_str(),
            prompt_tokens.empty() ? nullptr : prompt_tokens.c_str(),
            completion_tokens.empty() ? nullptr : completion_tokens.c_str(),
            entry.has_redacted_body ? entry.redacted_body_json.c_str() : nullptr,
            entry.has_redacted_response ? entry.redacted_response_json.c_str() : nullptr,
            nullable_cstr(entry.error),
        };

        PGresult* result = PQexecParams(as_pg_conn(pg_conn_), insert_sql, 21, nullptr, params,
                                        nullptr, nullptr, 0);
        if (!result || PQresultStatus(result) != PGRES_COMMAND_OK) {
            LOG(WARNING, "RequestLog")
                << "Failed to insert request log row: "
                << (pg_conn_ ? PQerrorMessage(as_pg_conn(pg_conn_)) : "no connection")
                << std::endl;
            ok = false;
            if (result) {
                PQclear(result);
            }
            break;
        }
        PQclear(result);
        } catch (const std::exception& e) {
            LOG(WARNING, "RequestLog")
                << "Skipping request log row after serialization error: " << e.what()
                << std::endl;
        } catch (...) {
            LOG(WARNING, "RequestLog")
                << "Skipping request log row after serialization error." << std::endl;
        }
    }

    PGresult* end = PQexec(as_pg_conn(pg_conn_), ok ? "COMMIT" : "ROLLBACK");
    if (end) {
        PQclear(end);
    }
    if (!ok) {
        database_available_.store(false);
        close_connection();
    }
    return ok;
}

void RequestLogService::run_purge() {
    if (retention_days_ == -1 || !ensure_connection()) {
        return;
    }

    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!pg_conn_ || PQstatus(as_pg_conn(pg_conn_)) != CONNECTION_OK) {
        return;
    }

    const char* sql = nullptr;
    if (retention_days_ == 0) {
        sql = "DELETE FROM request_logs";
    } else {
        sql = "DELETE FROM request_logs WHERE created_at < NOW() - make_interval(days => $1)";
    }

    PGresult* result = nullptr;
    if (retention_days_ == 0) {
        result = PQexec(as_pg_conn(pg_conn_), sql);
    } else {
        const std::string days = std::to_string(retention_days_);
        const char* params[1] = {days.c_str()};
        result = PQexecParams(as_pg_conn(pg_conn_), sql, 1, nullptr, params, nullptr, nullptr, 0);
    }

    if (!result || PQresultStatus(result) != PGRES_COMMAND_OK) {
        LOG(WARNING, "RequestLog")
            << "Failed to purge request logs: "
            << PQerrorMessage(as_pg_conn(pg_conn_)) << std::endl;
        database_available_.store(false);
    }
    if (result) {
        PQclear(result);
    }
}

nlohmann::json RequestLogService::get_recent(int limit) const {
    if (!const_cast<RequestLogService*>(this)->ensure_connection() ||
        !const_cast<RequestLogService*>(this)->init_schema()) {
        throw std::runtime_error("Request log database is unavailable");
    }

    std::lock_guard<std::mutex> lock(db_mutex_);
    const std::string sql =
        "SELECT id, created_at, client_ip, forwarded_for, method, path, query_string, "
        "status_code, duration_ms, user_agent, endpoint_type, model, keep_alive, stream, "
        "request_body_bytes, response_body_bytes, prompt_chars, messages_chars, "
        "prompt_tokens, completion_tokens, redacted_body, redacted_response, error "
        "FROM request_logs ORDER BY created_at DESC LIMIT $1";
    const std::string limit_str = std::to_string(limit);
    const char* params[1] = {limit_str.c_str()};

    PGresult* result = PQexecParams(as_pg_conn(pg_conn_), sql.c_str(), 1, nullptr, params,
                                    nullptr, nullptr, 0);
    if (!result || PQresultStatus(result) != PGRES_TUPLES_OK) {
        if (result) {
            PQclear(result);
        }
        throw std::runtime_error("Failed to query recent request logs");
    }

    nlohmann::json entries = nlohmann::json::array();
    const int rows = PQntuples(result);
    for (int i = 0; i < rows; ++i) {
        entries.push_back(row_to_json_from_result(result, i));
    }
    PQclear(result);
    return {{"entries", entries}};
}

nlohmann::json RequestLogService::search(const httplib::Request& req) const {
    if (!const_cast<RequestLogService*>(this)->ensure_connection() ||
        !const_cast<RequestLogService*>(this)->init_schema()) {
        throw std::runtime_error("Request log database is unavailable");
    }

    const int limit = clamp_limit(query_param(req, "limit"), 100, 1000);
    const int offset = clamp_offset(query_param(req, "offset"));
    const std::string model = query_param(req, "model");
    const std::string client_ip = query_param(req, "client_ip");
    const std::string path = query_param(req, "path");
    const std::string keep_alive = query_param(req, "keep_alive");
    const std::string since = parse_since_timestamp(query_param(req, "since"));

    std::ostringstream sql;
    sql << "SELECT id, created_at, client_ip, forwarded_for, method, path, query_string, "
           "status_code, duration_ms, user_agent, endpoint_type, model, keep_alive, stream, "
           "request_body_bytes, response_body_bytes, prompt_chars, messages_chars, "
           "prompt_tokens, completion_tokens, redacted_body, redacted_response, error "
           "FROM request_logs WHERE 1=1";

    std::vector<std::string> params;
    auto add_filter = [&](const std::string& clause, const std::string& value) {
        if (value.empty()) {
            return;
        }
        params.push_back(value);
        sql << clause << "$" << params.size();
    };

    add_filter(" AND model = ", model);
    add_filter(" AND client_ip = ", client_ip);
    add_filter(" AND path LIKE ", path.empty() ? "" : "%" + path + "%");
    add_filter(" AND keep_alive = ", keep_alive);
    add_filter(" AND created_at >= ", since);

    params.push_back(std::to_string(limit));
    sql << " ORDER BY created_at DESC LIMIT $" << params.size();
    params.push_back(std::to_string(offset));
    sql << " OFFSET $" << params.size();

    std::vector<const char*> param_ptrs;
    param_ptrs.reserve(params.size());
    for (const auto& param : params) {
        param_ptrs.push_back(param.c_str());
    }

    std::lock_guard<std::mutex> lock(db_mutex_);
    PGresult* result = PQexecParams(as_pg_conn(pg_conn_), sql.str().c_str(),
                                    static_cast<int>(param_ptrs.size()), nullptr,
                                    param_ptrs.data(), nullptr, nullptr, 0);
    if (!result || PQresultStatus(result) != PGRES_TUPLES_OK) {
        if (result) {
            PQclear(result);
        }
        throw std::runtime_error("Failed to search request logs");
    }

    nlohmann::json entries = nlohmann::json::array();
    const int rows = PQntuples(result);
    for (int i = 0; i < rows; ++i) {
        entries.push_back(row_to_json_from_result(result, i));
    }
    PQclear(result);
    return {{"entries", entries}, {"limit", limit}, {"offset", offset}};
}

nlohmann::json RequestLogService::get_stats(const httplib::Request& req) const {
    if (!const_cast<RequestLogService*>(this)->ensure_connection() ||
        !const_cast<RequestLogService*>(this)->init_schema()) {
        throw std::runtime_error("Request log database is unavailable");
    }

    const std::string since =
        parse_since_timestamp(query_param(req, "since").empty() ? "24h"
                                                               : query_param(req, "since"));

    std::lock_guard<std::mutex> lock(db_mutex_);
    const char* summary_sql =
        "SELECT COUNT(*)::bigint, COALESCE(AVG(duration_ms), 0), "
        "COUNT(DISTINCT client_ip)::bigint, COUNT(*) FILTER (WHERE keep_alive IS NOT NULL)::bigint "
        "FROM request_logs WHERE created_at >= $1";
    const char* summary_params[1] = {since.c_str()};
    PGresult* summary = PQexecParams(as_pg_conn(pg_conn_), summary_sql, 1, nullptr, summary_params,
                                     nullptr, nullptr, 0);
    if (!summary || PQresultStatus(summary) != PGRES_TUPLES_OK) {
        if (summary) {
            PQclear(summary);
        }
        throw std::runtime_error("Failed to query request log stats");
    }

    nlohmann::json response = {
        {"since", since},
        {"total_requests", std::stoll(PQgetvalue(summary, 0, 0))},
        {"avg_duration_ms", std::stod(PQgetvalue(summary, 0, 1))},
        {"unique_client_ips", std::stoll(PQgetvalue(summary, 0, 2))},
        {"keep_alive_requests", std::stoll(PQgetvalue(summary, 0, 3))},
        {"by_endpoint_type", nlohmann::json::object()},
        {"by_model", nlohmann::json::object()},
    };
    PQclear(summary);

    const char* by_type_sql =
        "SELECT endpoint_type, COUNT(*)::bigint FROM request_logs "
        "WHERE created_at >= $1 GROUP BY endpoint_type ORDER BY COUNT(*) DESC";
    PGresult* by_type = PQexecParams(as_pg_conn(pg_conn_), by_type_sql, 1, nullptr, summary_params,
                                     nullptr, nullptr, 0);
    if (by_type && PQresultStatus(by_type) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(by_type); ++i) {
            response["by_endpoint_type"][PQgetvalue(by_type, i, 0)] =
                std::stoll(PQgetvalue(by_type, i, 1));
        }
    }
    if (by_type) {
        PQclear(by_type);
    }

    const char* by_model_sql =
        "SELECT COALESCE(model, ''), COUNT(*)::bigint FROM request_logs "
        "WHERE created_at >= $1 GROUP BY model ORDER BY COUNT(*) DESC LIMIT 20";
    PGresult* by_model = PQexecParams(as_pg_conn(pg_conn_), by_model_sql, 1, nullptr,
                                      summary_params, nullptr, nullptr, 0);
    if (by_model && PQresultStatus(by_model) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(by_model); ++i) {
            const char* model = PQgetvalue(by_model, i, 0);
            response["by_model"][model && *model ? model : "(none)"] =
                std::stoll(PQgetvalue(by_model, i, 1));
        }
    }
    if (by_model) {
        PQclear(by_model);
    }

    return response;
}

nlohmann::json RequestLogService::clear_all() {
    if (!ensure_connection()) {
        throw std::runtime_error("Request log database is unavailable");
    }

    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!pg_conn_ || PQstatus(as_pg_conn(pg_conn_)) != CONNECTION_OK) {
        throw std::runtime_error("Request log database is unavailable");
    }

    int64_t deleted = 0;
    PGresult* count_result = PQexec(as_pg_conn(pg_conn_), "SELECT COUNT(*)::bigint FROM request_logs");
    if (count_result && PQresultStatus(count_result) == PGRES_TUPLES_OK && PQntuples(count_result) > 0) {
        const char* value = PQgetvalue(count_result, 0, 0);
        if (value && *value) {
            deleted = std::stoll(value);
        }
    }
    if (count_result) {
        PQclear(count_result);
    }

    PGresult* delete_result = PQexec(as_pg_conn(pg_conn_), "DELETE FROM request_logs");
    const bool ok = delete_result && PQresultStatus(delete_result) == PGRES_COMMAND_OK;
    if (!ok) {
        if (delete_result) {
            PQclear(delete_result);
        }
        database_available_.store(false);
        throw std::runtime_error("Failed to clear request logs");
    }
    if (delete_result) {
        PQclear(delete_result);
    }

    LOG(INFO, "RequestLog") << "Cleared " << deleted << " request log entries." << std::endl;
    return {{"deleted", deleted}};
}

#else

bool RequestLogService::ensure_connection() { return false; }
void RequestLogService::close_connection() {}
bool RequestLogService::init_schema() { return false; }
bool RequestLogService::insert_entries(const std::vector<RequestLogEntry>&) { return false; }
void RequestLogService::run_purge() {}

nlohmann::json RequestLogService::get_recent(int) const {
    throw std::runtime_error("Request logging is not available in this build");
}

nlohmann::json RequestLogService::search(const httplib::Request&) const {
    throw std::runtime_error("Request logging is not available in this build");
}

nlohmann::json RequestLogService::get_stats(const httplib::Request&) const {
    throw std::runtime_error("Request logging is not available in this build");
}

nlohmann::json RequestLogService::clear_all() {
    throw std::runtime_error("Request logging is not available in this build");
}

#endif

void RequestLogService::writer_loop() {
    while (running_.load()) {
        std::vector<RequestLogEntry> batch;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(500), [this]() {
                return !running_.load() || !queue_.empty();
            });
            if (!running_.load() && queue_.empty()) {
                break;
            }
            while (!queue_.empty() && batch.size() < 50) {
                batch.push_back(std::move(queue_.front()));
                queue_.pop_front();
            }
        }

        if (!batch.empty()) {
            if (!insert_entries(batch)) {
                LOG(WARNING, "RequestLog")
                    << "Request log insert failed; entries in this batch were dropped."
                    << std::endl;
            }
        }
    }

    std::vector<RequestLogEntry> remaining;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        remaining.assign(std::make_move_iterator(queue_.begin()),
                         std::make_move_iterator(queue_.end()));
        queue_.clear();
    }
    if (!remaining.empty()) {
        insert_entries(remaining);
    }
}

void RequestLogService::purge_loop() {
    while (running_.load()) {
        for (int i = 0; i < 3600 && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!running_.load()) {
            break;
        }
        run_purge();
    }
}

} // namespace lemon
