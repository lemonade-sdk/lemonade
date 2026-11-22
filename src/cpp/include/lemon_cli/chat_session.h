#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lemon_cli {

constexpr int kChatSessionVersion = 1;

struct ChatUsage {
    int64_t prompt_tokens = 0;
    int64_t completion_tokens = 0;
    int64_t turns = 0;
};

// A saved conversation. `messages` holds only user and assistant turns; the
// system prompt is kept separately so it can be edited without touching history.
struct ChatSession {
    std::string id;
    std::string name;   // Set by the user; unique within a store. Empty if unnamed.
    std::string title;  // Derived from the first user message.
    std::string model;
    std::string system_prompt;
    std::string created_at;
    std::string updated_at;
    nlohmann::json settings = nlohmann::json::object();
    nlohmann::json messages = nlohmann::json::array();
    ChatUsage usage;

    nlohmann::json request_messages() const;
    std::string display_name() const;
    void ensure_title();
    void clear_messages();
    void touch();
};

enum class ExportFormat { Json, Markdown };

nlohmann::json session_to_json(const ChatSession& session);
// Throws std::runtime_error when the document is not a valid session.
ChatSession session_from_json(const nlohmann::json& doc);

std::string session_to_markdown(const ChatSession& session);
ChatSession session_from_markdown(const std::string& text);

// Picks Markdown for .md/.markdown paths and JSON otherwise.
ExportFormat export_format_for_path(const std::filesystem::path& path);
// Throws std::runtime_error on failure, or when `path` exists and !overwrite.
void export_session_file(const ChatSession& session, const std::filesystem::path& path,
                         ExportFormat format, bool overwrite);
// Detects JSON or Markdown from the content. Throws std::runtime_error on failure.
ChatSession import_session_file(const std::filesystem::path& path);

std::string now_iso8601();
std::string validate_session_name(const std::string& name);
std::string trim(const std::string& text);
// Lowercased name, title and message text, for case-insensitive searching.
std::string search_text(const ChatSession& session);
bool session_matches(const ChatSession& session, const std::string& query);
// Cuts at a UTF-8 boundary and appends "..." when the text is longer than `max_bytes`.
std::string truncate_utf8(const std::string& text, size_t max_bytes);

// An exclusive, advisory, cross-process lock. The OS drops it if the holder dies.
class SessionLock {
public:
    SessionLock() = default;
    SessionLock(const SessionLock&) = delete;
    SessionLock& operator=(const SessionLock&) = delete;
    SessionLock(SessionLock&& other) noexcept;
    SessionLock& operator=(SessionLock&& other) noexcept;
    ~SessionLock();

    bool held() const;
    void release();

private:
    friend class SessionStore;
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

// One JSON file per session in `dir`. Writes are atomic (temp file + rename),
// files are owner-only on POSIX, and a per-session lock serializes writers.
class SessionStore {
public:
    explicit SessionStore(std::filesystem::path dir);
    static SessionStore for_cli();

    const std::filesystem::path& dir() const { return dir_; }
    std::filesystem::path path_for(const std::string& id) const;

    // Newest first. Files that cannot be read are skipped and described in `problems`.
    std::vector<ChatSession> list(std::vector<std::string>* problems = nullptr) const;
    ChatSession load(const std::string& id) const;
    void save(const ChatSession& session) const;

    // Accepts an exact ID, an exact name, or a unique ID prefix.
    std::string resolve(const std::string& id_or_name) const;
    std::optional<std::string> latest_id() const;
    std::optional<std::string> find_by_name(const std::string& name) const;

    ChatSession create(const std::string& model, const std::string& system_prompt) const;
    ChatSession import_session(ChatSession imported) const;

    // Returns an unheld lock if another process holds it for the whole timeout.
    SessionLock lock(const std::string& id, int timeout_ms) const;

    struct Opened {
        ChatSession session;
        SessionLock lock;
        bool created = false;
    };
    // Finds the session called `name` or creates and saves an empty one, and locks it.
    // `lock.held()` is false when the session is busy.
    Opened open_named(const std::string& name, const std::string& model,
                      const std::string& system_prompt, int lock_timeout_ms) const;

    // Sets `session.name` after checking uniqueness; saves when `persist` is true.
    void rename(ChatSession& session, const std::string& name, bool persist) const;

    // Throws when another process has the session open.
    void remove(const std::string& id) const;

private:
    static SessionLock acquire(const std::filesystem::path& path, int timeout_ms);
    SessionLock lock_index(int timeout_ms) const;
    std::string unique_name(const std::string& wanted) const;

    std::filesystem::path dir_;
};

} // namespace lemon_cli
