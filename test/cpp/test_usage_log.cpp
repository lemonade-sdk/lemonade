#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "usage_log.h"

namespace fs = std::filesystem;
using lemon::usage::make_record;
using lemon::usage::UsageEvent;
using lemon::usage::UsageLog;
using lemon::usage::UsageLogOptions;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static fs::path fresh_dir(const std::string& name) {
    fs::path dir = fs::temp_directory_path() / ("lemonade_usage_test_" + name);
    fs::remove_all(dir);
    return dir;
}

static std::vector<std::string> read_lines(const fs::path& file) {
    std::vector<std::string> lines;
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    return lines;
}

static std::vector<std::string> file_names(const fs::path& dir) {
    std::vector<std::string> names;
    if (!fs::exists(dir)) return names;
    for (const auto& entry : fs::directory_iterator(dir)) names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

// Fixed-date records must not age out as the real clock moves on.
static UsageLogOptions roomy(const fs::path& dir) {
    return UsageLogOptions{dir.string(), 100 * 1024 * 1024, 1000000};
}

static nlohmann::ordered_json record_on(const std::string& date, int n = 0) {
    return {{"ts", date + "T12:00:00.000Z"}, {"n", n}};
}

static UsageEvent sample_event() {
    UsageEvent e;
    e.end_unix_ms = 1790622131412;  // 2026-09-28T19:02:11.412Z
    e.request = "chat.completions";
    e.model = "Qwen3-8B-GGUF";
    e.backend = "llamacpp";
    e.device = "gpu";
    e.stream = true;
    e.status = "ok";
    e.duration_ms = 4210;
    e.input_tokens = 812;
    e.output_tokens = 356;
    e.cached_tokens = 640;
    e.ttft_ms = 182.4;
    e.tokens_per_second = 48.26;
    e.input = "hello";
    e.output = "hi there";
    return e;
}

static void test_record_format() {
    auto r = make_record(sample_event(), false);
    check("record: ts is ISO-8601 UTC with ms", r["ts"] == "2026-09-28T19:02:11.412Z");
    check("record: host is set", r["host"].is_string() && !r["host"].get<std::string>().empty());
    check("record: lemonade_version is set", r["lemonade_version"].is_string());
    check("record: token counts", r["input_tokens"] == 812 && r["output_tokens"] == 356 && r["cached_tokens"] == 640);
    check("record: tokens_reported true", r["tokens_reported"] == true);
    check("record: tokens_estimated false", r["tokens_estimated"] == false);
    check("record: ttft_ms rounded to whole ms", r["ttft_ms"] == 182);
    check("record: tokens_per_second rounded to 0.1", r["tokens_per_second"] == 48.3);
    check("record: missing session/client_ip are null", r["session_id"].is_null() && r["client_ip"].is_null());
    check("record: content none omits text", !r.contains("input") && !r.contains("output"));

    auto full = make_record(sample_event(), true);
    check("record: content full includes text", full["input"] == "hello" && full["output"] == "hi there");
}

static void test_record_unreported_tokens() {
    auto e = sample_event();
    e.input_tokens = e.output_tokens = e.cached_tokens = -1;
    auto r = make_record(e, false);
    check("unreported: tokens_reported false", r["tokens_reported"] == false);
    check("unreported: token fields null, not 0",
          r["input_tokens"].is_null() && r["output_tokens"].is_null() && r["cached_tokens"].is_null());
}

static void test_record_unreported_perf() {
    auto e = sample_event();
    e.ttft_ms = -1;
    e.tokens_per_second = 0;
    auto r = make_record(e, false);
    check("perf: missing ttft_ms is null", r.contains("ttft_ms") && r["ttft_ms"].is_null());
    check("perf: zero tokens_per_second is null", r.contains("tokens_per_second") && r["tokens_per_second"].is_null());
}

static void test_record_estimated_abort() {
    auto e = sample_event();
    e.status = "aborted";
    e.input_tokens = -1;
    e.cached_tokens = -1;
    e.output_tokens = 42;
    e.output_estimated = true;
    auto r = make_record(e, false);
    check("aborted: status", r["status"] == "aborted");
    check("aborted: tokens_estimated true", r["tokens_estimated"] == true);
    check("aborted: tokens_reported false", r["tokens_reported"] == false);
    check("aborted: output_tokens holds chunk count", r["output_tokens"] == 42);
}

static void test_writes_one_line_per_record_into_dated_file() {
    auto dir = fresh_dir("dated");
    UsageLog log([&] { return roomy(dir); });
    log.start();
    log.push(record_on("2026-09-28", 1));
    log.push(record_on("2026-09-28", 2));
    log.flush();
    auto lines = read_lines(dir / "usage-2026-09-28.jsonl");
    check("dated: two lines in usage-2026-09-28.jsonl", lines.size() == 2);
    check("dated: lines are the records", lines.size() == 2 && nlohmann::json::parse(lines[1])["n"] == 2);
}

static void test_rolls_over_at_day_boundary() {
    auto dir = fresh_dir("day");
    UsageLog log([&] { return roomy(dir); });
    log.start();
    log.push(record_on("2026-09-28"));
    log.push(record_on("2026-09-29"));
    log.flush();
    auto names = file_names(dir);
    check("day: one file per date",
          names == std::vector<std::string>{"usage-2026-09-28.jsonl", "usage-2026-09-29.jsonl"});
}

static void test_prunes_oldest_days_over_total_cap() {
    auto dir = fresh_dir("cap");
    auto opts = roomy(dir);
    opts.max_total_bytes = 150;
    UsageLog log([&] { return opts; });
    log.start();
    for (int d = 20; d <= 29; ++d) log.push(record_on("2026-09-" + std::to_string(d), d));
    log.flush();
    uint64_t total = 0;
    for (const auto& n : file_names(dir)) total += fs::file_size(dir / n);
    auto names = file_names(dir);
    check("cap: total size within max_size", total <= 150);
    check("cap: oldest days deleted first", !names.empty() && names.front() != "usage-2026-09-20.jsonl");
    check("cap: newest day kept", fs::exists(dir / "usage-2026-09-29.jsonl"));
}

static void test_cap_never_deletes_newest_day() {
    auto dir = fresh_dir("cap_today");
    auto opts = roomy(dir);
    opts.max_total_bytes = 100;
    UsageLog log([&] { return opts; });
    log.start();
    for (int i = 0; i < 20; ++i) log.push(record_on("2026-09-29", i));
    log.flush();
    check("cap: a single day over the cap is kept whole", read_lines(dir / "usage-2026-09-29.jsonl").size() == 20);
}

static void test_unlimited_limits_keep_everything() {
    auto dir = fresh_dir("unlimited");
    fs::create_directories(dir);
    std::ofstream(dir / "usage-2020-01-01.jsonl") << "{}\n";
    UsageLogOptions opts{dir.string(), 0, 0};
    UsageLog log([&] { return opts; });
    log.start();
    for (int i = 0; i < 20; ++i) log.push(record_on("2026-09-29", i));
    log.flush();
    check("unlimited: old file kept", fs::exists(dir / "usage-2020-01-01.jsonl"));
}

static void test_disk_usage_tracks_files() {
    auto dir = fresh_dir("disk");
    fs::create_directories(dir);
    std::ofstream(dir / "usage-2026-09-27.jsonl") << "{\"n\":0}\n";
    auto opts = roomy(dir);
    UsageLog log([&] { return opts; });
    log.start();
    log.flush();
    check("disk: existing files counted at startup", log.disk_usage_bytes() == fs::file_size(dir / "usage-2026-09-27.jsonl"));
    log.push(record_on("2026-09-29", 1));
    log.flush();
    uint64_t total = 0;
    for (const auto& n : file_names(dir)) total += fs::file_size(dir / n);
    check("disk: total follows writes", log.disk_usage_bytes() == total);
}

static void test_prunes_files_older_than_max_days() {
    auto dir = fresh_dir("age");
    fs::create_directories(dir);
    std::ofstream(dir / "usage-2020-01-01.jsonl") << "{}\n";
    auto opts = roomy(dir);
    opts.max_days = 90;
    UsageLog log([&] { return opts; });
    log.start();
    log.push(record_on("2026-09-28"));
    log.flush();
    check("age: file older than max_days deleted", !fs::exists(dir / "usage-2020-01-01.jsonl"));
}

static void test_queue_full_writes_dropped_marker() {
    auto dir = fresh_dir("drop");
    UsageLog log([&] { return roomy(dir); }, 2);
    for (int i = 0; i < 5; ++i) log.push(record_on("2026-09-28", i));
    log.start();
    log.flush();
    auto lines = read_lines(dir / "usage-2026-09-28.jsonl");
    check("drop: queued records written", lines.size() == 3);
    check("drop: marker counts dropped records",
          lines.size() == 3 && nlohmann::json::parse(lines[2]) == nlohmann::json{{"dropped", 3}});
}

static void test_write_failure_disables_log() {
    auto blocker = fresh_dir("fail");
    std::ofstream(blocker) << "not a directory";
    UsageLog log([&] { return roomy(blocker); });
    log.start();
    log.push(record_on("2026-09-28"));
    log.flush();
    check("failure: log disabled", log.failed());
    log.push(record_on("2026-09-28"));
    log.flush();
    check("failure: later pushes are ignored without throwing", log.failed());
    fs::remove(blocker);
}

int main() {
    std::printf("=== RUNNING USAGE LOG C++ TESTS ===\n");
    test_record_format();
    test_record_unreported_tokens();
    test_record_unreported_perf();
    test_record_estimated_abort();
    test_writes_one_line_per_record_into_dated_file();
    test_rolls_over_at_day_boundary();
    test_prunes_oldest_days_over_total_cap();
    test_cap_never_deletes_newest_day();
    test_unlimited_limits_keep_everything();
    test_disk_usage_tracks_files();
    test_prunes_files_older_than_max_days();
    test_queue_full_writes_dropped_marker();
    test_write_failure_disables_log();
    std::printf("=== %s (%d failures) ===\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures ? 1 : 0;
}
