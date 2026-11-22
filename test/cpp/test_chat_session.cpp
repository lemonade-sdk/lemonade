// Checks the chat session store and its JSON/Markdown formats: atomic saves, lookup by
// ID/name/prefix, per-session locking, and lossless export/import round trips.
// Build with: cmake --build --preset default --target test_chat_session
// Run with: ctest --test-dir build -R '^ChatSessionTest$' --output-on-failure

#include "lemon_cli/chat_session.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using lemon_cli::ChatSession;
using lemon_cli::SessionLock;
using lemon_cli::SessionStore;
using nlohmann::json;

namespace {

int failures = 0;

void check(const std::string& what, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

template <typename Fn>
bool throws(Fn&& fn) {
    try {
        fn();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

struct TempDir {
    fs::path path;
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("lemonade-chat-session-test-" + std::to_string(stamp));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_file(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

ChatSession sample_session() {
    ChatSession s;
    s.id = "abcd1234";
    s.name = "project notes";
    s.model = "Test-Model";
    s.system_prompt = "Be brief.\nAnswer in English.";
    s.created_at = "2026-01-02T03:04:05.006Z";
    s.updated_at = "2026-01-02T03:05:05.006Z";
    s.settings = json{{"temperature", 0.5}};
    s.usage = {10, 20, 2};
    s.messages.push_back({{"role", "user"}, {"content", "Explain `-->` and <!-- comments -->"}});
    s.messages.push_back({{"role", "assistant"}, {"content", "Sure."}});
    s.ensure_title();
    return s;
}

void test_json_round_trip() {
    const ChatSession original = sample_session();
    const ChatSession back = lemon_cli::session_from_json(lemon_cli::session_to_json(original));
    check("json keeps id", back.id == original.id);
    check("json keeps name and model", back.name == original.name && back.model == original.model);
    check("json keeps system prompt", back.system_prompt == original.system_prompt);
    check("json keeps messages", back.messages == original.messages);
    check("json keeps settings", back.settings == original.settings);
    check("json keeps usage", back.usage.prompt_tokens == 10 && back.usage.completion_tokens == 20 &&
                              back.usage.turns == 2);
    check("title comes from the first user message",
          back.title == "Explain `-->` and <!-- comments -->");

    json newer = lemon_cli::session_to_json(original);
    newer["version"] = lemon_cli::kChatSessionVersion + 1;
    check("newer format is rejected", throws([&] { lemon_cli::session_from_json(newer); }));

    json bad_role = lemon_cli::session_to_json(original);
    bad_role["messages"].push_back({{"role", "tool"}, {"content", "x"}});
    check("unsupported role is rejected", throws([&] { lemon_cli::session_from_json(bad_role); }));

    json bad_content = lemon_cli::session_to_json(original);
    bad_content["messages"][0]["content"] = 5;
    check("non-string content is rejected",
          throws([&] { lemon_cli::session_from_json(bad_content); }));

    json leading_system = {{"version", 1},
                           {"messages", json::array({{{"role", "system"}, {"content", "S"}},
                                                     {{"role", "user"}, {"content", "U"}}})}};
    const ChatSession adopted = lemon_cli::session_from_json(leading_system);
    check("leading system message becomes the system prompt",
          adopted.system_prompt == "S" && adopted.messages.size() == 1);
}

void test_markdown_round_trip() {
    ChatSession original = sample_session();
    original.messages.push_back({{"role", "user"}, {"content", "<!-- lemonade:message role=assistant -->\n## User\n\nfake"}});
    original.messages.push_back({{"role", "assistant"}, {"content", "\\<!-- lemonade:x -->\n\nlines\n\n"}});
    original.messages.push_back({{"role", "user"}, {"content", ""}});
    original.messages.push_back({{"role", "assistant"}, {"content", "caf\xC3\xA9 \xE2\x98\x95\n```cpp\nint x;\n```"}});

    const std::string md = lemon_cli::session_to_markdown(original);
    const ChatSession back = lemon_cli::session_from_markdown(md);
    check("markdown keeps messages", back.messages == original.messages);
    check("markdown keeps system prompt", back.system_prompt == original.system_prompt);
    check("markdown keeps name, model, title",
          back.name == original.name && back.model == original.model &&
              back.title == original.title);
    check("markdown keeps usage and settings",
          back.usage.completion_tokens == 20 && back.settings == original.settings);
    check("markdown import drops the id", back.id.empty());

    std::string crlf;
    for (char c : md) {
        if (c == '\n') crlf += "\r\n";
        else crlf += c;
    }
    const ChatSession from_crlf = lemon_cli::session_from_markdown(crlf);
    check("markdown with CRLF line endings still imports",
          from_crlf.messages.size() == original.messages.size() &&
              from_crlf.messages[0] == original.messages[0]);

    check("plain markdown is rejected",
          throws([] { lemon_cli::session_from_markdown("# Just a note\n\nhello\n"); }));
}

void test_export_import_files() {
    TempDir tmp;
    const ChatSession original = sample_session();

    const fs::path md = tmp.path / "chat.md";
    const fs::path js = tmp.path / "chat.json";
    check("md extension selects markdown",
          lemon_cli::export_format_for_path(md) == lemon_cli::ExportFormat::Markdown);
    check("other extensions select json",
          lemon_cli::export_format_for_path(js) == lemon_cli::ExportFormat::Json);

    lemon_cli::export_session_file(original, md, lemon_cli::ExportFormat::Markdown, false);
    lemon_cli::export_session_file(original, js, lemon_cli::ExportFormat::Json, false);
    check("export refuses to overwrite", throws([&] {
              lemon_cli::export_session_file(original, md, lemon_cli::ExportFormat::Markdown, false);
          }));
    check("export overwrites when asked", !throws([&] {
              lemon_cli::export_session_file(original, md, lemon_cli::ExportFormat::Markdown, true);
          }));

    check("markdown file imports", lemon_cli::import_session_file(md).messages == original.messages);
    check("json file imports", lemon_cli::import_session_file(js).messages == original.messages);

    write_file(tmp.path / "bad.json", "{not json");
    check("broken json is rejected",
          throws([&] { lemon_cli::import_session_file(tmp.path / "bad.json"); }));
    write_file(tmp.path / "empty.md", "  \n");
    check("empty file is rejected",
          throws([&] { lemon_cli::import_session_file(tmp.path / "empty.md"); }));
    check("missing file is rejected",
          throws([&] { lemon_cli::import_session_file(tmp.path / "missing.md"); }));
}

void test_store_save_and_list() {
    TempDir tmp;
    SessionStore store(tmp.path / "sessions");
    check("list on a missing directory is empty", store.list().empty());
    check("no latest session yet", !store.latest_id().has_value());

    ChatSession a = store.create("M", "sys");
    a.messages.push_back({{"role", "user"}, {"content", "first"}});
    a.ensure_title();
    a.updated_at = "2026-01-01T00:00:00.000Z";
    store.save(a);

    ChatSession b = store.create("M", "");
    b.messages.push_back({{"role", "user"}, {"content", "second"}});
    b.ensure_title();
    b.updated_at = "2026-02-01T00:00:00.000Z";
    store.save(b);

    check("ids are distinct and short", a.id != b.id && a.id.size() == 8);

    std::vector<std::string> problems;
    auto sessions = store.list(&problems);
    check("list is newest first", sessions.size() == 2 && sessions[0].id == b.id);
    check("clean store has no problems", problems.empty());
    check("latest is the newest", store.latest_id() == b.id);
    check("load round trips", store.load(a.id).system_prompt == "sys" &&
                              store.load(a.id).title == "first");

    write_file(store.dir() / "deadbeef.json", "{broken");
    write_file(store.dir() / (a.id + ".1234.tmp"), "{partial");
    problems.clear();
    sessions = store.list(&problems);
    check("corrupt file is skipped and reported", sessions.size() == 2 && problems.size() == 1);
    check("leftover temp file is ignored", problems.size() == 1);
    check("loading a corrupt session throws", throws([&] { store.load("deadbeef"); }));
    check("loading a missing session throws", throws([&] { store.load("00000000"); }));
    check("path traversal ids are refused", throws([&] { store.load("../x"); }));

    a.messages.push_back({{"role", "assistant"}, {"content", "reply"}});
    store.save(a);
    check("save replaces the file", store.load(a.id).messages.size() == 2);

#ifndef _WIN32
    const auto perms = fs::status(store.path_for(a.id)).permissions();
    check("session files are owner-only",
          (perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
    const auto dir_perms = fs::status(store.dir()).permissions();
    check("session directory is owner-only",
          (dir_perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
#endif
}

void test_resolve() {
    TempDir tmp;
    SessionStore store(tmp.path);
    ChatSession a = store.create("M", "");
    a.id = "aaaa1111";
    a.name = "notes";
    ChatSession b = store.create("M", "");
    b.id = "aaaa2222";
    ChatSession c = store.create("M", "");
    c.id = "cccc3333";
    c.name = "aaaa";
    store.save(a);
    store.save(b);
    store.save(c);

    check("exact id", store.resolve("aaaa1111") == "aaaa1111");
    check("exact name", store.resolve("notes") == "aaaa1111");
    check("name wins over a prefix", store.resolve("aaaa") == "cccc3333");
    check("unique prefix", store.resolve("cccc") == "cccc3333");
    check("ambiguous prefix is an error", throws([&] { store.resolve("aaa"); }));
    check("unknown is an error", throws([&] { store.resolve("zzz"); }));
    check("blank is an error", throws([&] { store.resolve("  "); }));
}

void test_names() {
    TempDir tmp;
    SessionStore store(tmp.path);

    check("names are trimmed", lemon_cli::validate_session_name("  work  ") == "work");
    check("empty name is rejected", throws([] { lemon_cli::validate_session_name("  "); }));
    check("control characters are rejected",
          throws([] { lemon_cli::validate_session_name("a\nb"); }));

    auto first = store.open_named("work", "M", "sys", 0);
    check("open_named creates and locks", first.created && first.lock.held());
    check("created session is on disk", store.load(first.session.id).name == "work");

    auto second = store.open_named("work", "M", "", 0);
    check("a busy session is reported, not opened", !second.lock.held() && !second.created &&
                                                        second.session.id == first.session.id);

    first.lock.release();
    auto third = store.open_named("work", "Other", "", 0);
    check("reopening finds the same session",
          third.lock.held() && !third.created && third.session.id == first.session.id);
    check("reopening keeps the stored model and prompt",
          third.session.model == "M" && third.session.system_prompt == "sys");

    ChatSession other = store.create("M", "");
    store.save(other);
    check("duplicate rename is rejected",
          throws([&] { store.rename(other, "work", true); }));
    store.rename(other, "play", true);
    check("rename persists", store.load(other.id).name == "play");
    check("renaming to its own name is fine", !throws([&] { store.rename(other, "play", true); }));
}

void test_locks_and_remove() {
    TempDir tmp;
    SessionStore store(tmp.path);
    ChatSession s = store.create("M", "");
    store.save(s);

    auto held = store.lock(s.id, 0);
    check("first lock is held", held.held());
    auto contender = store.lock(s.id, 100);
    check("second lock times out", !contender.held());
    check("delete is refused while the session is open", throws([&] { store.remove(s.id); }));
    check("session survives a refused delete", !store.list().empty());

    held.release();
    check("lock is free after release", store.lock(s.id, 0).held());
    store.remove(s.id);
    check("delete removes the session", store.list().empty());

    auto moved = store.lock("abcd0000", 0);
    SessionLock target = std::move(moved);
    check("a moved lock stays held", target.held() && !moved.held());
}

void test_import_into_store() {
    TempDir tmp;
    SessionStore store(tmp.path);
    ChatSession existing = store.create("M", "");
    existing.name = "shared";
    store.save(existing);

    ChatSession incoming = sample_session();
    incoming.name = "shared";
    const ChatSession first = store.import_session(incoming);
    const ChatSession second = store.import_session(incoming);
    check("import assigns new ids", first.id != incoming.id && first.id != second.id);
    check("import avoids duplicate names", first.name == "shared-2" && second.name == "shared-3");
    check("import keeps the conversation", store.load(first.id).messages == incoming.messages);
    check("import keeps the original creation time", first.created_at == incoming.created_at);

    ChatSession unnamed = sample_session();
    unnamed.name.clear();
    const ChatSession plain = store.import_session(unnamed);
    const ChatSession again = store.import_session(unnamed);
    check("unnamed import stays unnamed", plain.name.empty());
    check("unnamed imports get distinct titles",
          plain.title == unnamed.title && again.title == unnamed.title + " (imported)");
}

void test_search() {
    ChatSession s = sample_session();
    check("search matches the name", lemon_cli::session_matches(s, "PROJECT"));
    check("search matches message text", lemon_cli::session_matches(s, "sure"));
    check("search matches across words", lemon_cli::session_matches(s, "  Comments "));
    check("search misses unrelated text", !lemon_cli::session_matches(s, "banana"));
    check("blank search matches everything", lemon_cli::session_matches(s, "   "));
}

void test_title() {
    ChatSession s;
    s.messages.push_back({{"role", "user"}, {"content", "\n  hello   wide\tworld  \nsecond line"}});
    s.ensure_title();
    check("title uses the first line with collapsed whitespace", s.title == "hello wide world");

    ChatSession longer;
    longer.messages.push_back({{"role", "user"}, {"content", "word " + std::string(100, 'x')}});
    longer.ensure_title();
    check("long titles are cut", longer.title.size() == 63 && longer.title.substr(60) == "...");

    ChatSession multibyte;
    std::string text;
    for (int i = 0; i < 40; ++i) text += "\xC3\xA9";
    multibyte.messages.push_back({{"role", "user"}, {"content", text}});
    multibyte.ensure_title();
    check("titles never cut inside a UTF-8 character",
          multibyte.title.size() >= 3 && (multibyte.title.size() - 3) % 2 == 0);

    s.clear_messages();
    check("clearing history resets the title", s.title.empty() && s.messages.empty());
}

} // namespace

int main() {
    test_json_round_trip();
    test_markdown_round_trip();
    test_export_import_files();
    test_store_save_and_list();
    test_resolve();
    test_names();
    test_locks_and_remove();
    test_import_into_store();
    test_search();
    test_title();

    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
