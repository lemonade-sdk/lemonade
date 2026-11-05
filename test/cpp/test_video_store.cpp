#include "lemon/server/video_store.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

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

static void test_is_video_job() {
    using nlohmann::json;
    const json video_step = {{"id", "video"}, {"op", "generate_video"}, {"params", json::object()}};
    check("video job: one generate_video step", store::is_video_job({{"steps", {video_step}}}));
    check("video job: a load job is not one",
          !store::is_video_job({{"steps", {{{"id", "a"}, {"op", "load"}}}}}));
    check("video job: a chat job is not one",
          !store::is_video_job({{"steps", {{{"id", "a"}, {"op", "chat"}}}}}));
    check("video job: generate_video among other steps is not one",
          !store::is_video_job({{"steps", {{{"id", "a"}, {"op", "load"}}, video_step}}}));
    check("video job: no steps is not one", !store::is_video_job({{"steps", json::array()}}));
    check("video job: a malformed record is not one", !store::is_video_job(json::array()));
}

static void test_video_status() {
    using nlohmann::json;
    check("status: running passes through",
          store::video_status(json{{"status", "running"}}) == "running");
    check("status: a cancel reads canceled",
          store::video_status(json{{"status", "interrupted"}}) == "canceled");
    check("status: a restart reads failed",
          store::video_status(json{{"status", "interrupted"},
                                   {"error", "server restarted while the job was active"}}) ==
              "failed");
    const auto paused = [](const char* step_status) {
        return json{{"status", "paused"},
                    {"steps", json::array({{{"op", "generate_video"}, {"status", step_status}}})}};
    };
    check("status: paused after the clip was made reads completed",
          store::video_status(paused("completed")) == "completed");
    check("status: paused before the clip reads paused",
          store::video_status(paused("pending")) == "paused");
}

int main() {
    test_file_names();
    test_write_read();
    test_remove_all_except();
    test_is_video_job();
    test_video_status();
    if (g_failures) {
        std::printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("ALL PASS (0 failures)\n");
    return 0;
}
