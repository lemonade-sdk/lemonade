#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#include <lemon/jobs/job_manager.h>

namespace fs = std::filesystem;
using lemon::jobs::CancelFlag;
using lemon::jobs::JobManager;
using lemon::jobs::OpHandler;
using lemon::jobs::OpRegistry;
using lemon::jobs::StepRecord;
using lemon::jobs::json;

namespace {

int g_failures = 0;

void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

std::atomic<bool> g_started{false};
std::atomic<bool> g_release{false};

OpRegistry registry() {
    OpRegistry reg;
    OpHandler hold;
    hold.run = [](const json&, const json&, CancelFlag&) {
        g_started = true;
        while (!g_release) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return json{{"ok", true}};
    };
    reg.register_op("hold", hold);
    OpHandler quick;
    quick.run = [](const json&, const json&, CancelFlag&) { return json{{"ok", true}}; };
    reg.register_op("quick", quick);
    return reg;
}

StepRecord step(const std::string& id, const std::string& op) {
    StepRecord s;
    s.id = id;
    s.op = op;
    return s;
}

template <typename Pred> bool wait_for(Pred pred) {
    for (int i = 0; i < 400; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

std::string final_status(JobManager& jobs, const std::string& id) {
    std::string status;
    wait_for([&] {
        status = jobs.get(id)->value("status", std::string());
        return status != "queued" && status != "running";
    });
    return status;
}

std::string pause_during(std::vector<StepRecord> steps, const fs::path& dir) {
    g_started = false;
    g_release = false;
    JobManager jobs(dir.string(), dir.string(), registry());
    const std::string id = jobs.create("t", std::move(steps), json::object());
    wait_for([] { return g_started.load(); });
    jobs.pause(id);
    g_release = true;
    return final_status(jobs, id);
}

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "lemon_test_job_manager_pause";
    fs::remove_all(dir);
    fs::create_directories(dir);

    check("pause during the last step: the job completes",
          pause_during({step("a", "hold")}, dir) == "completed");
    check("pause before a remaining step: the job pauses",
          pause_during({step("a", "hold"), step("b", "quick")}, dir) == "paused");

    fs::remove_all(dir);
    if (g_failures) {
        std::printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("ALL PASS (0 failures)\n");
    return 0;
}
