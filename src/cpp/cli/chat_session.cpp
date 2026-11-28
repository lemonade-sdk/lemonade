#include "lemon_cli/chat_session.h"

#include <lemon/utils/path_utils.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <sys/file.h>
    #include <unistd.h>
#endif

namespace lemon_cli {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr size_t kMaxTitleBytes = 60;
constexpr size_t kMaxNameBytes = 120;
constexpr uintmax_t kMaxImportBytes = 64ull * 1024 * 1024;
constexpr int kLockPollMs = 50;
constexpr int kIndexLockWaitMs = 10000;

const char kMetaPrefix[] = "<!-- lemonade:chat-session ";
const char kMessagePrefix[] = "<!-- lemonade:message role=";
const char kCommentSuffix[] = " -->";
const char kEscapeProbe[] = "<!-- lemonade:";

std::string dump_json(const json& j, int indent = -1) {
    return j.dump(indent, ' ', false, json::error_handler_t::replace);
}

bool starts_with(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

bool ends_with(const std::string& s, const char* suffix) {
    const std::string tail(suffix);
    return s.size() >= tail.size() &&
           s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

std::string title_from_text(const std::string& text) {
    std::string out;
    bool pending_space = false;
    for (char c : text) {
        if (c == '\n' && !out.empty()) break;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) out += ' ';
        pending_space = false;
        out += c;
    }
    return truncate_utf8(out, kMaxTitleBytes);
}

std::string random_hex(size_t digits) {
    static thread_local std::mt19937_64 rng{
        std::random_device{}() ^
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < digits; ++i) out += kHex[rng() & 0xF];
    return out;
}

bool valid_id(const std::string& id) {
    if (id.size() < 4 || id.size() > 64) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_';
    });
}

void require_valid_id(const std::string& id) {
    if (!valid_id(id)) throw std::runtime_error("invalid session id '" + id + "'");
}

std::string get_string(const json& doc, const char* key) {
    auto it = doc.find(key);
    if (it == doc.end() || it->is_null()) return {};
    if (!it->is_string()) {
        throw std::runtime_error(std::string("field '") + key + "' must be a string");
    }
    return it->get<std::string>();
}

int64_t get_count(const json& doc, const char* key) {
    auto it = doc.find(key);
    if (it == doc.end() || it->is_null()) return 0;
    if (!it->is_number_integer() || it->get<int64_t>() < 0) {
        throw std::runtime_error(std::string("field '") + key +
                                 "' must be a non-negative integer");
    }
    return it->get<int64_t>();
}

std::string role_heading(const std::string& role) {
    if (role == "system") return "System";
    if (role == "user") return "User";
    return "Assistant";
}

std::string read_text_file(const fs::path& path) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec) throw std::runtime_error("cannot read '" + lemon::utils::path_to_utf8(path) + "'");
    if (size > kMaxImportBytes) {
        throw std::runtime_error("'" + lemon::utils::path_to_utf8(path) + "' is too large");
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open '" + lemon::utils::path_to_utf8(path) + "'");
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string escape_content(const std::string& content) {
    std::string out;
    size_t pos = 0;
    while (true) {
        const size_t nl = content.find('\n', pos);
        const std::string line = content.substr(pos, nl == std::string::npos ? nl : nl - pos);
        const size_t slashes = line.find_first_not_of('\\');
        if (slashes != std::string::npos && line.compare(slashes, sizeof(kEscapeProbe) - 1,
                                                         kEscapeProbe) == 0) {
            out += '\\';
        }
        out += line;
        if (nl == std::string::npos) break;
        out += '\n';
        pos = nl + 1;
    }
    return out;
}

std::string unescape_content(const std::string& content) {
    std::string out;
    size_t pos = 0;
    while (true) {
        const size_t nl = content.find('\n', pos);
        std::string line = content.substr(pos, nl == std::string::npos ? nl : nl - pos);
        const size_t slashes = line.find_first_not_of('\\');
        if (slashes != std::string::npos && slashes > 0 &&
            line.compare(slashes, sizeof(kEscapeProbe) - 1, kEscapeProbe) == 0) {
            line.erase(0, 1);
        }
        out += line;
        if (nl == std::string::npos) break;
        out += '\n';
        pos = nl + 1;
    }
    return out;
}

std::string meta_comment_json(const ChatSession& s) {
    json meta = {
        {"version", kChatSessionVersion},
        {"title", s.title},
        {"name", s.name},
        {"model", s.model},
        {"created_at", s.created_at},
        {"updated_at", s.updated_at},
        {"settings", s.settings},
        {"usage", {{"prompt_tokens", s.usage.prompt_tokens},
                   {"completion_tokens", s.usage.completion_tokens},
                   {"turns", s.usage.turns}}},
    };
    std::string text = dump_json(meta);
    // '>' only occurs inside JSON strings, so escaping it keeps "-->" out of the comment.
    std::string safe;
    for (char c : text) {
        if (c == '>') safe += "\\u003e";
        else safe += c;
    }
    return safe;
}

#ifdef _WIN32
// Returns false when another process holds the file.
bool try_acquire(const fs::path& path, void*& handle) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED ||
            err == ERROR_LOCK_VIOLATION) {
            return false;
        }
        throw std::runtime_error("cannot lock '" + lemon::utils::path_to_utf8(path) +
                                 "' (error " + std::to_string(err) + ")");
    }
    handle = h;
    return true;
}
#else
bool try_acquire(const fs::path& path, int& fd_out) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        throw std::runtime_error("cannot open lock file '" + path.string() + "'");
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const bool busy = errno == EWOULDBLOCK;
        ::close(fd);
        if (busy) return false;
        throw std::runtime_error("cannot lock '" + path.string() + "'");
    }
    fd_out = fd;
    return true;
}
#endif

} // namespace

// ---------------------------------------------------------------------------
// ChatSession
// ---------------------------------------------------------------------------

std::string trim(const std::string& text) {
    const char* ws = " \t\r\n";
    const size_t first = text.find_first_not_of(ws);
    if (first == std::string::npos) return {};
    const size_t last = text.find_last_not_of(ws);
    return text.substr(first, last - first + 1);
}

std::string truncate_utf8(const std::string& text, size_t max_bytes) {
    if (text.size() <= max_bytes) return text;
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return text.substr(0, cut) + "...";
}

std::string search_text(const ChatSession& session) {
    std::string text = session.name + "\n" + session.title;
    for (const auto& m : session.messages) {
        text += "\n";
        text += m.value("content", "");
    }
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool session_matches(const ChatSession& session, const std::string& query) {
    std::string needle = trim(query);
    if (needle.empty()) return true;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return search_text(session).find(needle) != std::string::npos;
}

std::string now_iso8601() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t secs = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    char out[48];
    std::snprintf(out, sizeof(out), "%s.%03dZ", buf, static_cast<int>(ms));
    return out;
}

std::string validate_session_name(const std::string& name) {
    std::string trimmed = trim(name);
    if (trimmed.empty()) throw std::runtime_error("session name cannot be empty");
    if (trimmed.size() > kMaxNameBytes) {
        throw std::runtime_error("session name is longer than " +
                                 std::to_string(kMaxNameBytes) + " bytes");
    }
    for (unsigned char c : trimmed) {
        if (c < 0x20 || c == 0x7f) {
            throw std::runtime_error("session name cannot contain control characters");
        }
    }
    return trimmed;
}

json ChatSession::request_messages() const {
    json out = json::array();
    if (!system_prompt.empty()) {
        out.push_back({{"role", "system"}, {"content", system_prompt}});
    }
    for (const auto& m : messages) out.push_back(m);
    return out;
}

std::string ChatSession::display_name() const {
    if (!name.empty()) return name;
    if (!title.empty()) return title;
    return "(empty)";
}

void ChatSession::ensure_title() {
    if (!title.empty()) return;
    for (const auto& m : messages) {
        if (m.value("role", "") == "user" && m.contains("content") && m["content"].is_string()) {
            title = title_from_text(m["content"].get<std::string>());
            return;
        }
    }
}

void ChatSession::clear_messages() {
    messages = json::array();
    title.clear();
}

void ChatSession::touch() {
    updated_at = now_iso8601();
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

json session_to_json(const ChatSession& s) {
    return {
        {"version", kChatSessionVersion},
        {"id", s.id},
        {"name", s.name},
        {"title", s.title},
        {"created_at", s.created_at},
        {"updated_at", s.updated_at},
        {"model", s.model},
        {"system_prompt", s.system_prompt},
        {"settings", s.settings},
        {"usage", {{"prompt_tokens", s.usage.prompt_tokens},
                   {"completion_tokens", s.usage.completion_tokens},
                   {"turns", s.usage.turns}}},
        {"messages", s.messages},
    };
}

ChatSession session_from_json(const json& doc) {
    if (!doc.is_object()) throw std::runtime_error("session file is not a JSON object");
    auto version = doc.find("version");
    if (version == doc.end() || !version->is_number_integer()) {
        throw std::runtime_error("session file has no 'version'");
    }
    if (version->get<int>() > kChatSessionVersion) {
        throw std::runtime_error("session was written by a newer version of Lemonade (format " +
                                 std::to_string(version->get<int>()) + ")");
    }
    if (version->get<int>() < 1) throw std::runtime_error("unsupported session format");

    ChatSession s;
    s.id = get_string(doc, "id");
    s.name = get_string(doc, "name");
    s.title = get_string(doc, "title");
    s.created_at = get_string(doc, "created_at");
    s.updated_at = get_string(doc, "updated_at");
    s.model = get_string(doc, "model");
    s.system_prompt = get_string(doc, "system_prompt");

    if (doc.contains("settings") && !doc["settings"].is_null()) {
        if (!doc["settings"].is_object()) throw std::runtime_error("'settings' must be an object");
        s.settings = doc["settings"];
    }
    if (doc.contains("usage") && !doc["usage"].is_null()) {
        if (!doc["usage"].is_object()) throw std::runtime_error("'usage' must be an object");
        s.usage.prompt_tokens = get_count(doc["usage"], "prompt_tokens");
        s.usage.completion_tokens = get_count(doc["usage"], "completion_tokens");
        s.usage.turns = get_count(doc["usage"], "turns");
    }

    auto messages = doc.find("messages");
    if (messages == doc.end() || !messages->is_array()) {
        throw std::runtime_error("session file has no 'messages' array");
    }
    size_t index = 0;
    for (const auto& m : *messages) {
        if (!m.is_object() || !m.contains("role") || !m["role"].is_string() ||
            !m.contains("content") || !m["content"].is_string()) {
            throw std::runtime_error("message " + std::to_string(index) +
                                     " needs a string 'role' and 'content'");
        }
        const std::string role = m["role"].get<std::string>();
        const std::string content = m["content"].get<std::string>();
        if (role == "system" && index == 0 && s.system_prompt.empty()) {
            s.system_prompt = content;
        } else if (role == "user" || role == "assistant") {
            s.messages.push_back({{"role", role}, {"content", content}});
        } else {
            throw std::runtime_error("message " + std::to_string(index) +
                                     " has unsupported role '" + role + "'");
        }
        ++index;
    }
    s.ensure_title();
    return s;
}

// ---------------------------------------------------------------------------
// Markdown
// ---------------------------------------------------------------------------

std::string session_to_markdown(const ChatSession& s) {
    std::string out;
    out += kMetaPrefix + meta_comment_json(s) + kCommentSuffix + "\n";
    out += "# " + (s.title.empty() ? s.display_name() : s.title) + "\n\n";
    if (!s.name.empty()) out += "- **Name:** " + s.name + "\n";
    if (!s.model.empty()) out += "- **Model:** " + s.model + "\n";
    if (!s.created_at.empty()) out += "- **Created:** " + s.created_at + "\n";
    if (!s.updated_at.empty()) out += "- **Updated:** " + s.updated_at + "\n";
    out += "- **Tokens:** " + std::to_string(s.usage.prompt_tokens) + " prompt, " +
           std::to_string(s.usage.completion_tokens) + " completion\n\n";

    auto add_block = [&](const std::string& role, const std::string& content) {
        out += kMessagePrefix + role + kCommentSuffix + "\n";
        out += "## " + role_heading(role) + "\n\n";
        out += escape_content(content) + "\n\n";
    };
    if (!s.system_prompt.empty()) add_block("system", s.system_prompt);
    for (const auto& m : s.messages) {
        add_block(m["role"].get<std::string>(), m["content"].get<std::string>());
    }
    return out;
}

ChatSession session_from_markdown(const std::string& input) {
    std::string text = input;
    if (starts_with(text, "\xEF\xBB\xBF")) text.erase(0, 3);
    const size_t first_nl = text.find('\n');
    if (first_nl != std::string::npos && first_nl > 0 && text[first_nl - 1] == '\r') {
        std::string normalized;
        normalized.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
            normalized += text[i];
        }
        text.swap(normalized);
    }

    struct Marker {
        std::string role;
        size_t marker_start;
        size_t body_start;
    };
    std::vector<Marker> markers;
    std::optional<json> meta;

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        const size_t end = nl == std::string::npos ? text.size() : nl;
        const std::string line = text.substr(pos, end - pos);
        if (starts_with(line, kMessagePrefix) && ends_with(line, kCommentSuffix)) {
            const size_t begin = sizeof(kMessagePrefix) - 1;
            const std::string role =
                line.substr(begin, line.size() - begin - (sizeof(kCommentSuffix) - 1));
            if (role != "system" && role != "user" && role != "assistant") {
                throw std::runtime_error("unknown message role '" + role + "' in Markdown");
            }
            markers.push_back({role, pos, nl == std::string::npos ? text.size() : nl + 1});
        } else if (markers.empty() && !meta && starts_with(line, kMetaPrefix) &&
                   ends_with(line, kCommentSuffix)) {
            const size_t begin = sizeof(kMetaPrefix) - 1;
            try {
                meta = json::parse(
                    line.substr(begin, line.size() - begin - (sizeof(kCommentSuffix) - 1)));
            } catch (const json::exception& e) {
                throw std::runtime_error(std::string("invalid session metadata: ") + e.what());
            }
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    if (!meta && markers.empty()) {
        throw std::runtime_error("not a Lemonade chat export");
    }

    json doc = meta.value_or(json::object());
    if (!doc.is_object()) throw std::runtime_error("invalid session metadata");
    if (!doc.contains("version")) doc["version"] = kChatSessionVersion;
    doc["messages"] = json::array();

    for (size_t i = 0; i < markers.size(); ++i) {
        const size_t stop = i + 1 < markers.size() ? markers[i + 1].marker_start : text.size();
        std::string raw = text.substr(markers[i].body_start, stop - markers[i].body_start);
        const std::string heading = "## " + role_heading(markers[i].role);
        if (!starts_with(raw, heading.c_str())) {
            throw std::runtime_error("message " + std::to_string(i) + " is missing its '" +
                                     heading + "' heading");
        }
        raw.erase(0, heading.size());
        for (int skipped = 0; skipped < 2 && !raw.empty() && raw.front() == '\n'; ++skipped) {
            raw.erase(0, 1);
        }
        for (int skipped = 0; skipped < 2 && !raw.empty() && raw.back() == '\n'; ++skipped) {
            raw.pop_back();
        }
        doc["messages"].push_back({{"role", markers[i].role},
                                   {"content", unescape_content(raw)}});
    }

    ChatSession session = session_from_json(doc);
    session.id.clear();
    return session;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

ExportFormat export_format_for_path(const fs::path& path) {
    std::string ext = lemon::utils::path_to_utf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return (ext == ".md" || ext == ".markdown") ? ExportFormat::Markdown : ExportFormat::Json;
}

void export_session_file(const ChatSession& session, const fs::path& path, ExportFormat format,
                         bool overwrite) {
    std::error_code ec;
    if (!overwrite && fs::exists(path, ec)) {
        throw std::runtime_error("'" + lemon::utils::path_to_utf8(path) +
                                 "' already exists (use --force to overwrite)");
    }
    const std::string content = format == ExportFormat::Markdown
        ? session_to_markdown(session)
        : dump_json(session_to_json(session), 2) + "\n";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write '" + lemon::utils::path_to_utf8(path) + "'");
    out << content;
    out.flush();
    if (!out) throw std::runtime_error("failed writing '" + lemon::utils::path_to_utf8(path) + "'");
}

ChatSession import_session_file(const fs::path& path) {
    std::string text = read_text_file(path);
    if (starts_with(text, "\xEF\xBB\xBF")) text.erase(0, 3);
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) throw std::runtime_error("file is empty");
    if (text[first] == '{') {
        json doc;
        try {
            doc = json::parse(text);
        } catch (const json::exception& e) {
            throw std::runtime_error(std::string("invalid JSON: ") + e.what());
        }
        ChatSession s = session_from_json(doc);
        s.id.clear();
        return s;
    }
    return session_from_markdown(text);
}

// ---------------------------------------------------------------------------
// SessionLock
// ---------------------------------------------------------------------------

SessionLock::SessionLock(SessionLock&& other) noexcept {
    *this = std::move(other);
}

SessionLock& SessionLock::operator=(SessionLock&& other) noexcept {
    if (this == &other) return *this;
    release();
#ifdef _WIN32
    handle_ = other.handle_;
    other.handle_ = nullptr;
#else
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
    return *this;
}

SessionLock::~SessionLock() {
    release();
}

bool SessionLock::held() const {
#ifdef _WIN32
    return handle_ != nullptr;
#else
    return fd_ >= 0;
#endif
}

void SessionLock::release() {
#ifdef _WIN32
    if (handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
#else
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#endif
}

// ---------------------------------------------------------------------------
// SessionStore
// ---------------------------------------------------------------------------

SessionStore::SessionStore(fs::path dir) : dir_(std::move(dir)) {}

SessionStore SessionStore::for_cli() {
    return SessionStore(lemon::utils::path_from_utf8(lemon::utils::get_config_dir()) /
                        "chat_sessions");
}

fs::path SessionStore::path_for(const std::string& id) const {
    require_valid_id(id);
    return dir_ / (id + ".json");
}

namespace {

void ensure_dir(const fs::path& dir) {
    std::error_code ec;
    const bool existed = fs::exists(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) {
        throw std::runtime_error("cannot create '" + lemon::utils::path_to_utf8(dir) +
                                 "': " + ec.message());
    }
#ifndef _WIN32
    if (!existed) fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
#endif
}

} // namespace

std::vector<ChatSession> SessionStore::list(std::vector<std::string>* problems) const {
    std::vector<ChatSession> sessions;
    std::error_code ec;
    if (!fs::is_directory(dir_, ec)) return sessions;

    for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& file = it->path();
        if (file.extension() != ".json") continue;
        const std::string id = lemon::utils::path_to_utf8(file.stem());
        try {
            if (!valid_id(id)) throw std::runtime_error("unexpected file name");
            ChatSession s = session_from_json(json::parse(read_text_file(file)));
            s.id = id;
            sessions.push_back(std::move(s));
        } catch (const std::exception& e) {
            if (problems) {
                problems->push_back(lemon::utils::path_to_utf8(file.filename()) + ": " + e.what());
            }
        }
    }
    std::sort(sessions.begin(), sessions.end(), [](const ChatSession& a, const ChatSession& b) {
        if (a.updated_at != b.updated_at) return a.updated_at > b.updated_at;
        return a.id > b.id;
    });
    return sessions;
}

ChatSession SessionStore::load(const std::string& id) const {
    const fs::path path = path_for(id);
    std::error_code ec;
    if (!fs::exists(path, ec)) throw std::runtime_error("session '" + id + "' does not exist");
    try {
        ChatSession s = session_from_json(json::parse(read_text_file(path)));
        s.id = id;
        return s;
    } catch (const json::exception& e) {
        throw std::runtime_error("session '" + id + "' is corrupt: " + e.what());
    } catch (const std::runtime_error& e) {
        throw std::runtime_error("session '" + id + "': " + e.what());
    }
}

void SessionStore::save(const ChatSession& session) const {
    const fs::path dest = path_for(session.id);
    ensure_dir(dir_);

    const fs::path tmp = dir_ / (session.id + "." + random_hex(8) + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot write '" + lemon::utils::path_to_utf8(tmp) + "'");
        }
#ifndef _WIN32
        std::error_code perm_ec;
        fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::replace, perm_ec);
#endif
        out << dump_json(session_to_json(session), 2) << "\n";
        out.flush();
        if (!out) {
            out.close();
            std::error_code rm_ec;
            fs::remove(tmp, rm_ec);
            throw std::runtime_error("failed writing '" + lemon::utils::path_to_utf8(tmp) + "'");
        }
    }
    std::error_code ec;
    if (!lemon::utils::atomic_replace_file(tmp, dest, ec)) {
        std::error_code rm_ec;
        fs::remove(tmp, rm_ec);
        throw std::runtime_error("cannot replace '" + lemon::utils::path_to_utf8(dest) +
                                 "': " + ec.message());
    }
}

std::optional<std::string> SessionStore::find_by_name(const std::string& name) const {
    for (const auto& s : list()) {
        if (!s.name.empty() && s.name == name) return s.id;
    }
    return std::nullopt;
}

std::optional<std::string> SessionStore::latest_id() const {
    auto sessions = list();
    if (sessions.empty()) return std::nullopt;
    return sessions.front().id;
}

std::string SessionStore::resolve(const std::string& id_or_name) const {
    const std::string wanted = trim(id_or_name);
    if (wanted.empty()) throw std::runtime_error("no session specified");

    const auto sessions = list();
    for (const auto& s : sessions) {
        if (s.id == wanted) return s.id;
    }
    for (const auto& s : sessions) {
        if (!s.name.empty() && s.name == wanted) return s.id;
    }
    std::vector<std::string> matches;
    for (const auto& s : sessions) {
        if (s.id.rfind(wanted, 0) == 0) matches.push_back(s.id);
    }
    if (matches.size() == 1) return matches.front();
    if (matches.size() > 1) {
        throw std::runtime_error("'" + wanted + "' matches more than one session; use a longer ID");
    }
    throw std::runtime_error("no session with ID or name '" + wanted + "'");
}

ChatSession SessionStore::create(const std::string& model, const std::string& system_prompt) const {
    ChatSession s;
    std::error_code ec;
    do {
        s.id = random_hex(8);
    } while (fs::exists(dir_ / (s.id + ".json"), ec));
    s.model = model;
    s.system_prompt = system_prompt;
    s.created_at = now_iso8601();
    s.updated_at = s.created_at;
    return s;
}

SessionLock SessionStore::acquire(const fs::path& path, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        SessionLock result;
#ifdef _WIN32
        void* handle = nullptr;
        if (try_acquire(path, handle)) {
            result.handle_ = handle;
            return result;
        }
#else
        int fd = -1;
        if (try_acquire(path, fd)) {
            result.fd_ = fd;
            return result;
        }
#endif
        if (std::chrono::steady_clock::now() >= deadline) return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(kLockPollMs));
    }
}

SessionLock SessionStore::lock(const std::string& id, int timeout_ms) const {
    require_valid_id(id);
    ensure_dir(dir_);
    return acquire(dir_ / (id + ".lock"), timeout_ms);
}

SessionLock SessionStore::lock_index(int timeout_ms) const {
    ensure_dir(dir_);
    return acquire(dir_ / ".index.lock", timeout_ms);
}

SessionStore::Opened SessionStore::open_named(const std::string& raw_name, const std::string& model,
                                              const std::string& system_prompt,
                                              int lock_timeout_ms) const {
    const std::string name = validate_session_name(raw_name);

    SessionLock index = lock_index(kIndexLockWaitMs);
    if (!index.held()) {
        throw std::runtime_error("the session list is busy; try again");
    }

    Opened opened;
    if (auto existing = find_by_name(name)) {
        index.release();
        opened.session.id = *existing;
        opened.session.name = name;
        opened.lock = lock(*existing, lock_timeout_ms);
        if (opened.lock.held()) opened.session = load(*existing);
        return opened;
    }

    opened.session = create(model, system_prompt);
    opened.session.name = name;
    opened.lock = lock(opened.session.id, 0);
    save(opened.session);
    opened.created = true;
    return opened;
}

void SessionStore::rename(ChatSession& session, const std::string& raw_name, bool persist) const {
    const std::string name = validate_session_name(raw_name);

    SessionLock index = lock_index(kIndexLockWaitMs);
    if (!index.held()) throw std::runtime_error("the session list is busy; try again");
    if (auto existing = find_by_name(name); existing && *existing != session.id) {
        throw std::runtime_error("name '" + name + "' is already used by session " + *existing);
    }
    session.name = name;
    session.touch();
    if (persist) save(session);
}

std::string SessionStore::unique_name(const std::string& wanted) const {
    if (wanted.empty() || !find_by_name(wanted)) return wanted;
    for (int n = 2;; ++n) {
        const std::string candidate = wanted + "-" + std::to_string(n);
        if (!find_by_name(candidate)) return candidate;
    }
}

ChatSession SessionStore::import_session(ChatSession imported) const {
    SessionLock index = lock_index(kIndexLockWaitMs);
    if (!index.held()) throw std::runtime_error("the session list is busy; try again");

    ChatSession fresh = create(imported.model, imported.system_prompt);
    fresh.name = unique_name(imported.name);
    fresh.title = imported.title;
    fresh.settings = imported.settings;
    fresh.messages = imported.messages;
    fresh.usage = imported.usage;
    if (!imported.created_at.empty()) fresh.created_at = imported.created_at;
    fresh.ensure_title();
    if (fresh.name.empty()) {
        // /history lists unnamed sessions by title, so keep an import apart from its original.
        std::vector<std::string> taken;
        for (const auto& s : list()) taken.push_back(s.display_name());
        auto in_use = [&](const std::string& title) {
            return std::find(taken.begin(), taken.end(), title) != taken.end();
        };
        if (in_use(fresh.title)) {
            const std::string base = fresh.title;
            fresh.title = base + " (imported)";
            for (int n = 2; in_use(fresh.title); ++n) {
                fresh.title = base + " (imported " + std::to_string(n) + ")";
            }
        }
    }
    save(fresh);
    return fresh;
}

void SessionStore::remove(const std::string& id) const {
    const fs::path path = path_for(id);
    SessionLock held = lock(id, 0);
    if (!held.held()) {
        throw std::runtime_error("session '" + id + "' is open in another lemonade chat");
    }
    std::error_code ec;
    if (!fs::remove(path, ec) && ec) {
        throw std::runtime_error("cannot delete '" + lemon::utils::path_to_utf8(path) +
                                 "': " + ec.message());
    }
    // Windows deletes the lock file when the handle closes.
    fs::remove(dir_ / (id + ".lock"), ec);
}

} // namespace lemon_cli
