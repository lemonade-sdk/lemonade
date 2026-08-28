#include "lemon/logging_config.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/path_utils.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using lemon::RotatingFileSink;
using lemon::RuntimeConfig;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static fs::path make_temp_dir() {
    fs::path dir = fs::temp_directory_path();
    dir /= "lemonade_test_log_rot_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);
    return dir;
}

static AixLog::Metadata make_metadata() {
    AixLog::Metadata md;
    md.severity = AixLog::Severity::info;
    return md;
}

static void test_incremental_runtime_rotation() {
    fs::path temp_dir = make_temp_dir();
    fs::path log_path = temp_dir / "lemonade-server.log";
    AixLog::Filter filter(AixLog::Severity::info);

    {
        RotatingFileSink sink(filter, log_path.string(), "%m", 1, 2);

        std::string chunk(100 * 1024, 'A'); // 100 KB
        for (int i = 0; i < 11; ++i) {
            sink.log(make_metadata(), chunk);
        }
    }

    check("active log exists after rotation", fs::exists(log_path));
    fs::path backup_1 = temp_dir / "lemonade-server.log.1";
    check("backup .1 created at runtime", fs::exists(backup_1));
    check("backup .1 size >= 1000KB", fs::file_size(backup_1) >= 1000 * 1024);
    check("active log size < 200KB", fs::file_size(log_path) < 200 * 1024);

    fs::remove_all(temp_dir);
}

static void test_preexisting_oversized_file() {
    fs::path temp_dir = make_temp_dir();
    fs::path log_path = temp_dir / "lemonade-server.log";

    {
        std::ofstream f(log_path, std::ios::binary);
        std::string oversized(1200 * 1024, 'O');
        f << oversized;
    }

    AixLog::Filter filter(AixLog::Severity::info);
    {
        RotatingFileSink sink(filter, log_path.string(), "%m", 1, 2);
        sink.log(make_metadata(), "New fresh entry");
    }

    fs::path backup_1 = temp_dir / "lemonade-server.log.1";
    check("pre-existing oversized file rotated to .1", fs::exists(backup_1));
    check("pre-existing .1 size >= 1.1MB", fs::file_size(backup_1) >= 1100 * 1024);
    check("active log contains new entry", fs::file_size(log_path) < 1000);

    fs::remove_all(temp_dir);
}

static void test_single_large_record_boundary() {
    fs::path temp_dir = make_temp_dir();
    fs::path log_path = temp_dir / "lemonade-server.log";
    AixLog::Filter filter(AixLog::Severity::info);

    {
        RotatingFileSink sink(filter, log_path.string(), "%m", 1, 2);

        // Record larger than 1 MB threshold written to fresh file
        std::string huge_record(1300 * 1024, 'H');
        sink.log(make_metadata(), huge_record);

        check("single huge record written without crash", fs::file_size(log_path) >= 1300 * 1024);

        // Next log call triggers rotation
        sink.log(make_metadata(), "Next record after huge line");
    }

    fs::path backup_1 = temp_dir / "lemonade-server.log.1";
    check("huge record rotated to .1 on next write", fs::exists(backup_1));
    check("backup .1 contains huge record", fs::file_size(backup_1) >= 1300 * 1024);
    check("active log has subsequent entry", fs::file_size(log_path) < 1000);

    fs::remove_all(temp_dir);
}

static void test_retention_cap_pruning() {
    fs::path temp_dir = make_temp_dir();
    fs::path log_path = temp_dir / "lemonade-server.log";
    AixLog::Filter filter(AixLog::Severity::info);

    // Pre-create higher-numbered backups (e.g. from a prior run with max_files=5)
    for (int i = 1; i <= 5; ++i) {
        std::ofstream f(temp_dir / ("lemonade-server.log." + std::to_string(i)));
        f << "legacy backup " << i << "\n";
    }
    std::ofstream f10(temp_dir / "lemonade-server.log.10");
    f10 << "legacy backup 10\n";
    std::ofstream f_date(temp_dir / "lemonade-server.log.20260825");
    f_date << "date-based backup 20260825\n";
    std::ofstream f_101(temp_dir / "lemonade-server.log.101");
    f_101 << "out of range 101\n";

    {
        RotatingFileSink sink(filter, log_path.string(), "%m", 1, 2);

        std::string block(600 * 1024, 'X');
        // Trigger rotations under max_files=2
        for (int r = 0; r < 6; ++r) {
            sink.log(make_metadata(), block);
        }
    }

    fs::path backup_1 = temp_dir / "lemonade-server.log.1";
    fs::path backup_2 = temp_dir / "lemonade-server.log.2";
    fs::path backup_3 = temp_dir / "lemonade-server.log.3";
    fs::path backup_4 = temp_dir / "lemonade-server.log.4";
    fs::path backup_5 = temp_dir / "lemonade-server.log.5";
    fs::path backup_10 = temp_dir / "lemonade-server.log.10";
    fs::path backup_date = temp_dir / "lemonade-server.log.20260825";
    fs::path backup_101 = temp_dir / "lemonade-server.log.101";

    check("backup .1 exists", fs::exists(backup_1));
    check("backup .2 exists", fs::exists(backup_2));
    check("backup .3 pruned (max_files=2)", !fs::exists(backup_3));
    check("backup .4 pruned (max_files=2)", !fs::exists(backup_4));
    check("backup .5 pruned (max_files=2)", !fs::exists(backup_5));
    check("backup .10 pruned (max_files=2)", !fs::exists(backup_10));
    check("date-based backup 20260825 preserved", fs::exists(backup_date));
    check("out of range backup 101 preserved", fs::exists(backup_101));

    fs::remove_all(temp_dir);
}

static void test_max_files_zero_truncation() {
    fs::path temp_dir = make_temp_dir();
    fs::path log_path = temp_dir / "lemonade-server.log";
    AixLog::Filter filter(AixLog::Severity::info);

    // Pre-create legacy backups
    std::ofstream f1(temp_dir / "lemonade-server.log.1");
    f1 << "legacy 1\n";
    std::ofstream f2(temp_dir / "lemonade-server.log.2");
    f2 << "legacy 2\n";

    {
        RotatingFileSink sink(filter, log_path.string(), "%m", 1, 0);

        std::string block(600 * 1024, 'Z');
        sink.log(make_metadata(), block);
        sink.log(make_metadata(), block); // Crosses 1MB -> truncate, writes 2nd block
        sink.log(make_metadata(), "Post truncation line");
    }

    fs::path backup_1 = temp_dir / "lemonade-server.log.1";
    fs::path backup_2 = temp_dir / "lemonade-server.log.2";
    check("max_files=0 creates no .1 backup and prunes old ones", !fs::exists(backup_1));
    check("max_files=0 prunes old .2 backup", !fs::exists(backup_2));
    check("active log exists after truncation", fs::exists(log_path));
    check("active log size reflects truncation", fs::file_size(log_path) < 700 * 1024);

    fs::remove_all(temp_dir);
}

static void test_validation_rejection() {
    AixLog::Filter filter(AixLog::Severity::info);
    bool threw = false;
    try {
        RotatingFileSink sink(filter, "test.log", "%m", 0, 5);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check("RotatingFileSink rejects max_file_size_mb = 0", threw);

    threw = false;
    try {
        RotatingFileSink sink(filter, "test.log", "%m", 5000, 5);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check("RotatingFileSink rejects max_file_size_mb > 2048", threw);

    threw = false;
    try {
        RotatingFileSink sink(filter, "test.log", "%m", 10, 150);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check("RotatingFileSink rejects max_files > 100", threw);

    threw = false;
    try {
        nlohmann::json bad_cfg = {{"log_max_file_size_mb", 0}};
        RuntimeConfig cfg(bad_cfg);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check("RuntimeConfig rejects log_max_file_size_mb = 0", threw);

    threw = false;
    try {
        nlohmann::json bad_cfg = {{"log_max_file_size_mb", 999999999999999999LL}};
        RuntimeConfig cfg(bad_cfg);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check("RuntimeConfig rejects 64-bit out-of-bounds log_max_file_size_mb", threw);

    threw = false;
    try {
        nlohmann::json bad_cfg = {{"log_max_files", -1}};
        RuntimeConfig cfg(bad_cfg);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check("RuntimeConfig rejects log_max_files = -1", threw);

    threw = false;
    try {
        nlohmann::json bad_cfg = {{"log_max_files", 999999999999999999LL}};
        RuntimeConfig cfg(bad_cfg);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check("RuntimeConfig rejects 64-bit out-of-bounds log_max_files", threw);

    fs::path temp_dir = make_temp_dir();
    threw = false;
    try {
        RotatingFileSink sink(filter, temp_dir.string(), "%m", 1, 2);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check("RotatingFileSink rejects directory path as log file", threw);

    threw = false;
    try {
        nlohmann::json bad_cfg = {{"log_file", temp_dir.string()}};
        RuntimeConfig cfg(bad_cfg);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check("RuntimeConfig rejects directory path as log_file", threw);

    fs::remove_all(temp_dir);
}

static void test_directory_collision_slot_fallback() {
    fs::path temp_dir = make_temp_dir();
    fs::path log_path = temp_dir / "lemonade.log";
    fs::path backup_1 = temp_dir / "lemonade.log.1";
    fs::path backup_2 = temp_dir / "lemonade.log.2";

    // Create a directory at backup_1 path
    fs::create_directories(backup_1);

    // Write initial data to log_path
    {
        std::ofstream f(log_path, std::ios::binary);
        std::string chunk = "INITIAL_CANARY_RECORD\n" + std::string(500 * 1024, 'X') + "\n";
        f << chunk;
        f.flush();
    }

    AixLog::Filter filter(AixLog::Severity::info);
    {
        RotatingFileSink sink(filter, log_path.string(), "%m", 1, 2);

        std::string extra_chunk = "NEW_ROTATED_RECORD\n" + std::string(600 * 1024, 'Y');
        sink.log(make_metadata(), extra_chunk);
    }

    std::error_code ec;
    check("backup_1 directory remains untouched", fs::is_directory(backup_1, ec));
    check("active log rotated into slot .2 when .1 is a directory", fs::exists(backup_2, ec));
    check("backup .2 contains initial canary data", fs::file_size(backup_2, ec) >= 500 * 1024);

    std::ifstream in(log_path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check("active log does not contain old canary after rotation", content.find("INITIAL_CANARY_RECORD") == std::string::npos);
    check("active log contains new rotated record", content.find("NEW_ROTATED_RECORD") != std::string::npos);

    fs::remove_all(temp_dir);
}

static void test_all_backup_slots_blocked_fallback() {
    fs::path temp_dir = make_temp_dir();
    fs::path log_path = temp_dir / "lemonade.log";
    fs::path backup_1 = temp_dir / "lemonade.log.1";
    fs::path backup_2 = temp_dir / "lemonade.log.2";

    // Create directories at all backup slots
    fs::create_directories(backup_1);
    fs::create_directories(backup_2);

    // Write initial canary record
    {
        std::ofstream f(log_path, std::ios::binary);
        f << "INITIAL_CANARY_RECORD\n" + std::string(500 * 1024, 'K') + "\n";
    }

    AixLog::Filter filter(AixLog::Severity::info);
    {
        RotatingFileSink sink(filter, log_path.string(), "%m", 1, 2);
        sink.log(make_metadata(), "NEW_POST_ROTATION_RECORD\n" + std::string(600 * 1024, 'L'));
    }

    std::error_code ec;
    check("backup_1 directory remains untouched", fs::is_directory(backup_1, ec));
    check("backup_2 directory remains untouched", fs::is_directory(backup_2, ec));

    std::ifstream in(log_path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check("all blocked slots truncates old canary (bounded policy)", content.find("INITIAL_CANARY_RECORD") == std::string::npos);
    check("all blocked slots contains fresh record", content.find("NEW_POST_ROTATION_RECORD") != std::string::npos);
    check("all blocked slots bounds active log size", fs::file_size(log_path, ec) < 700 * 1024);

    fs::remove_all(temp_dir);
}

static void test_real_rename_os_error_fallback() {
    fs::path temp_dir = make_temp_dir();
    // Filename of 254 chars + ".1" = 256 chars (> 255 NAME_MAX), causing fs::rename to fail with ENAMETOOLONG
    std::string long_base(254, 'l');
    fs::path log_path = temp_dir / long_base;

    // Write initial canary record
    {
        std::ofstream f(log_path, std::ios::binary);
        f << "INITIAL_CANARY_RECORD\n" + std::string(500 * 1024, 'K') + "\n";
    }

    AixLog::Filter filter(AixLog::Severity::info);
    {
        RotatingFileSink sink(filter, log_path.string(), "%m", 1, 2);
        sink.log(make_metadata(), "APPENDED_FALLBACK_RECORD\n" + std::string(600 * 1024, 'L'));
    }

    std::ifstream in(log_path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check("real rename OS error preserves initial canary", content.find("INITIAL_CANARY_RECORD") != std::string::npos);
    check("real rename OS error appends new record", content.find("APPENDED_FALLBACK_RECORD") != std::string::npos);

    fs::remove_all(temp_dir);
}

int main() {
    test_incremental_runtime_rotation();
    test_preexisting_oversized_file();
    test_single_large_record_boundary();
    test_retention_cap_pruning();
    test_max_files_zero_truncation();
    test_validation_rejection();
    test_directory_collision_slot_fallback();
    test_all_backup_slots_blocked_fallback();
    test_real_rename_os_error_fallback();

    std::printf("\nSummary: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
