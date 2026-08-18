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

    {
        RotatingFileSink sink(filter, log_path.string(), "%m", 1, 2);

        std::string block(600 * 1024, 'X');
        // Trigger 3 rotations
        for (int r = 0; r < 6; ++r) {
            sink.log(make_metadata(), block);
        }
    }

    fs::path backup_1 = temp_dir / "lemonade-server.log.1";
    fs::path backup_2 = temp_dir / "lemonade-server.log.2";
    fs::path backup_3 = temp_dir / "lemonade-server.log.3";

    check("backup .1 exists", fs::exists(backup_1));
    check("backup .2 exists", fs::exists(backup_2));
    check("backup .3 pruned (max_files=2)", !fs::exists(backup_3));

    fs::remove_all(temp_dir);
}

static void test_max_files_zero_truncation() {
    fs::path temp_dir = make_temp_dir();
    fs::path log_path = temp_dir / "lemonade-server.log";
    AixLog::Filter filter(AixLog::Severity::info);

    {
        RotatingFileSink sink(filter, log_path.string(), "%m", 1, 0);

        std::string block(600 * 1024, 'Z');
        sink.log(make_metadata(), block);
        sink.log(make_metadata(), block); // Crosses 1MB -> truncate, writes 2nd block
        sink.log(make_metadata(), "Post truncation line");
    }

    fs::path backup_1 = temp_dir / "lemonade-server.log.1";
    check("max_files=0 creates no .1 backup", !fs::exists(backup_1));
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
        nlohmann::json bad_cfg = {{"log_max_files", -1}};
        RuntimeConfig cfg(bad_cfg);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check("RuntimeConfig rejects log_max_files = -1", threw);
}

int main() {
    test_incremental_runtime_rotation();
    test_preexisting_oversized_file();
    test_single_large_record_boundary();
    test_retention_cap_pruning();
    test_max_files_zero_truncation();
    test_validation_rejection();

    std::printf("\nSummary: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
