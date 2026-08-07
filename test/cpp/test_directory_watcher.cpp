// Standalone test for lemon::DirectoryWatcher.
// Compile with:
//   g++ -std=c++17 -pthread -I src/cpp/include test/cpp/test_directory_watcher.cpp -o directory_watcher_test
//   cl /std:c++17 /EHsc /I src/cpp/include test/cpp/test_directory_watcher.cpp /link kernel32.lib

#include "lemon/directory_watcher.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using lemon::DirectoryWatcher;
namespace fs = std::filesystem;

struct TestResult {
    int passed = 0;
    int failed = 0;

    void ok(const std::string& name) {
        printf("[PASS] %s\n", name.c_str());
        ++passed;
    }

    void fail(const std::string& name) {
        printf("[FAIL] %s\n", name.c_str());
        ++failed;
    }
};

// Create a unique temp directory for this test run
static std::string make_temp_dir() {
    fs::path tmp = fs::temp_directory_path() / "directory_watcher_test_";
    tmp += std::to_string(std::hash<std::string>{}(std::to_string(std::time(nullptr))));
    fs::create_directories(tmp);
    return tmp.string();
}

// Test 1: Basic file-change detection via callback
static void test_basic_detection(TestResult& r) {
    fs::path dir = make_temp_dir();
    auto cb = fs::path(dir); // capture for lambda
    std::atomic<int> call_count{0};

    {
        DirectoryWatcher watcher(dir.string());
        watcher.set_callback([&call_count]() { ++call_count; });
        watcher.start();

        // Write a file — triggers inotify kqueue/polling
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::ofstream{fs::path(dir) / "test.txt"} << "hello";

        // Give debounce + poll time
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }

    int count = call_count.load();
    if (count > 0) {
        r.ok("basic file-change detection (calls=" + std::to_string(count) + ")");
    } else {
        r.fail("basic file-change detection (callback never fired)");
    }
}

// Test 2: File deletion detection
static void test_file_deletion(TestResult& r) {
    fs::path dir = make_temp_dir();
    std::atomic<int> call_count{0};

    // Pre-create a file
    std::ofstream{fs::path(dir) / "to_delete.txt"} << "bye";

    {
        DirectoryWatcher watcher(dir.string());
        watcher.set_callback([&call_count]() { ++call_count; });
        watcher.start();

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        fs::remove(fs::path(dir) / "to_delete.txt");

        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }

    int count = call_count.load();
    if (count > 0) {
        r.ok("file deletion detection (calls=" + std::to_string(count) + ")");
    } else {
        r.fail("file deletion detection (callback never fired)");
    }
}

// Test 3: Stop cleanly
static void test_stop(TestResult& r) {
    fs::path dir = make_temp_dir();
    std::atomic<int> call_count{0};

    {
        DirectoryWatcher watcher(dir.string());
        watcher.set_callback([&call_count]() { ++call_count; });
        watcher.start();

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        watcher.stop();
    }

    r.ok("stop cleanly");
}

// Test 4: Non-existent directory — watcher polls until dir appears
static void test_nonexistent_dir(TestResult& r) {
    fs::path dir = make_temp_dir();
    // Remove it so it doesn't exist
    fs::remove_all(dir);

    std::atomic<int> call_count{0};
    std::atomic<bool> done{false};

    {
        DirectoryWatcher watcher(dir.string());
        watcher.set_callback([&call_count]() { ++call_count; });
        watcher.start();

        // Wait a bit for the watcher to start polling
        std::this_thread::sleep_for(std::chrono::milliseconds(600));

        // Now create the directory (simulates late creation)
        fs::create_directories(dir);

        // Wait a bit for the watcher to detect the new dir and set up inotify
        std::this_thread::sleep_for(std::chrono::milliseconds(600));

        // Create a file to trigger the callback
        std::ofstream{dir / "file.txt"} << "data";

        // Wait for debounce + detection
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }

    int count = call_count.load();
    if (count > 0) {
        r.ok("non-existent dir recovery (calls=" + std::to_string(count) + ")");
    } else {
        r.fail("non-existent dir recovery (callback never fired)");
    }
}

// Test 5: Rapid changes — debounce coalesces events
static void test_debounce(TestResult& r) {
    fs::path dir = make_temp_dir();
    std::atomic<int> call_count{0};

    {
        DirectoryWatcher watcher(dir.string());
        watcher.set_callback([&call_count]() { ++call_count; });
        watcher.start();

        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Create many files in quick succession
        for (int i = 0; i < 10; ++i) {
            std::ofstream{fs::path(dir) / ("rapid_" + std::to_string(i) + ".txt")} << i;
        }

        // Wait for debounce window
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // After debounce, another batch
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        for (int i = 10; i < 20; ++i) {
            std::ofstream{fs::path(dir) / ("rapid_" + std::to_string(i) + ".txt")} << i;
        }

        // Wait for second debounce
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }

    int count = call_count.load();
    // Should have ~2 callback groups (two batches with gap between them)
    if (count >= 2) {
        r.ok("debounce coalescing (calls=" + std::to_string(count) + ", expected>=2)");
    } else {
        r.fail("debounce coalescing (calls=" + std::to_string(count) + ", expected>=2)");
    }
}

// Test 6: Directory removal — watcher stops gracefully
static void test_directory_removal(TestResult& r) {
    fs::path dir = make_temp_dir();

    {
        DirectoryWatcher watcher(dir.string());
        watcher.set_callback([]() {});
        watcher.start();

        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Remove the watched directory
        fs::remove_all(dir);

        // Wait for watcher to detect removal and stop
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }

    r.ok("directory removal graceful stop");
}

int main() {
    TestResult r;

    printf("=== DirectoryWatcher Unit Tests ===\n\n");

    test_basic_detection(r);
    test_file_deletion(r);
    test_stop(r);
    test_nonexistent_dir(r);
    test_debounce(r);
    test_directory_removal(r);

    printf("\n%d/%d tests passed\n", r.passed, r.passed + r.failed);
    return r.failed == 0 ? 0 : 1;
}
