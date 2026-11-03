#include "lemon/server/video_store.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
namespace store = lemon::video_store;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static void test_file_names() {
    const std::string a = store::new_file_name();
    const std::string b = store::new_file_name();
    check("name: generated names are valid", store::is_valid_file_name(a) && store::is_valid_file_name(b));
    check("name: generated names differ", a != b);

    std::string upper = a;
    upper.back() = 'A';
    for (const std::string& bad : {std::string(), std::string("video-"), std::string("../") + a,
                                   a + "/x", std::string("video-../../../../../../etc/passwd"), upper, a + "0",
                                   a.substr(0, a.size() - 1)}) {
        if (store::is_valid_file_name(bad)) {
            check(("name: rejects " + bad).c_str(), false);
            return;
        }
    }
    check("name: rejects traversal, wrong length and other shapes", true);
}

static void test_write_read() {
    const std::string name = store::new_file_name();
    const std::string bytes("\x1a\x45\xdf\xa3\0clip", 9);
    store::write(name, bytes);
    std::string back;
    check("io: a written clip reads back unchanged", store::read(name, back) && back == bytes);
    check("io: a missing clip does not read", !store::read(store::new_file_name(), back));
    check("io: an invalid name does not read", !store::read("../" + name, back));

    bool threw = false;
    try {
        store::write("../escape", bytes);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check("io: an invalid name does not write", threw && !fs::exists(store::dir().parent_path() / "escape"));
}

static void test_remove_all_except() {
    const std::string keep = store::new_file_name();
    const std::string drop = store::new_file_name();
    store::write(keep, "keep");
    store::write(drop, "drop");
    { std::ofstream(store::dir() / "notes.txt") << "not a clip"; }

    store::remove_all_except({keep});
    check("prune: a referenced clip stays", fs::exists(store::dir() / keep));
    check("prune: an unreferenced clip goes", !fs::exists(store::dir() / drop));
    check("prune: a file it did not make stays", fs::exists(store::dir() / "notes.txt"));
}

int main() {
    test_file_names();
    test_write_read();
    test_remove_all_except();
    if (g_failures) {
        std::printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("ALL PASS (0 failures)\n");
    return 0;
}
