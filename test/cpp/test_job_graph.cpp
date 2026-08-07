#include "lemon/jobs/job_graph.h"
#include "lemon/jobs/job_types.h"

#include <cstdio>
#include <set>
#include <string>

using namespace lemon::jobs;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static const std::set<std::string> kOps = {"system_info", "load", "unload", "chat"};

static StepRecord step(const std::string& id, const std::string& op) {
    StepRecord s;
    s.id = id;
    s.op = op;
    return s;
}

static void test_valid_graphs() {
    std::vector<StepRecord> ok = {step("a", "system_info"), step("b", "load"), step("c", "chat")};
    check("valid: linear ok", validate_steps(ok, kOps).empty());

    std::vector<StepRecord> br = {step("a", "load"), step("b", "chat"), step("c", "unload")};
    br[0].on_fail = "c";
    br[1].branch.push_back({"${b.tps} > 0", "c"});
    br[1].on_done = "c";
    check("valid: forward on_fail/branch/on_done ok", validate_steps(br, kOps).empty());
}

static void test_invalid_graphs() {
    std::vector<StepRecord> empty;
    check("invalid: empty rejected", !validate_steps(empty, kOps).empty());

    std::vector<StepRecord> dup = {step("a", "load"), step("a", "chat")};
    check("invalid: duplicate id rejected",
          validate_steps(dup, kOps).find("duplicate") != std::string::npos);

    std::vector<StepRecord> unk = {step("a", "teleport")};
    check("invalid: unknown op rejected",
          validate_steps(unk, kOps).find("unknown op") != std::string::npos);

    std::vector<StepRecord> back = {step("a", "load"), step("b", "chat")};
    back[1].on_done = "a";
    check("invalid: backward on_done rejected (no loops)",
          validate_steps(back, kOps).find("later step") != std::string::npos);

    std::vector<StepRecord> self = {step("a", "load"), step("b", "chat")};
    self[0].branch.push_back({"true", "a"});
    check("invalid: non-forward branch rejected",
          validate_steps(self, kOps).find("later step") != std::string::npos);

    std::vector<StepRecord> ghost = {step("a", "load"), step("b", "chat")};
    ghost[0].on_fail = "nowhere";
    check("invalid: on_fail to unknown id rejected",
          validate_steps(ghost, kOps).find("not a known step") != std::string::npos);

    std::vector<StepRecord> emptyid = {step("", "load")};
    check("invalid: empty id rejected", !validate_steps(emptyid, kOps).empty());

    std::vector<StepRecord> badexpr = {step("a", "load")};
    badexpr[0].when = "1 @ 2";
    check("invalid: malformed when rejected",
          validate_steps(badexpr, kOps).find("when") != std::string::npos);

    std::vector<StepRecord> incomplete = {step("a", "load")};
    incomplete[0].when = "1 +";
    check("invalid: incomplete expression rejected at validation",
          !validate_steps(incomplete, kOps).empty());

    std::vector<StepRecord> unbalanced = {step("a", "load")};
    unbalanced[0].when = "(true";
    check("invalid: unmatched paren rejected at validation",
          !validate_steps(unbalanced, kOps).empty());

    std::vector<StepRecord> chained = {step("a", "load"), step("b", "chat")};
    chained[0].branch.push_back({"1 < 2 < 3", "b"});
    check("invalid: chained comparison rejected at validation",
          !validate_steps(chained, kOps).empty());

    std::vector<StepRecord> defref = {step("a", "load")};
    defref[0].when = "${some.missing.ref} > 0";
    check("valid: unresolved ref deferred past validation",
          validate_steps(defref, kOps).empty());

    std::vector<StepRecord> reserved = {step("inputs", "load")};
    check("invalid: step id 'inputs' rejected",
          validate_steps(reserved, kOps).find("reserved") != std::string::npos);

    std::vector<StepRecord> dotted = {step("a.b", "load")};
    check("invalid: step id with '.' rejected",
          validate_steps(dotted, kOps).find("'.'") != std::string::npos);
}

static void test_json_roundtrip() {
    Job j;
    j.id = "job-1";
    j.name = "bench-duel";
    j.status = JobStatus::Running;
    j.inputs = {{"model", "Agents-A1"}};
    j.cursor = "run_v";
    StepRecord s = step("load_v", "load");
    s.params = {{"llamacpp_backend", "vulkan"}, {"ctx_size", "${inputs.ctx}"}};
    s.on_fail = "load_v_lo";
    s.branch.push_back({"${vulkan_tps} > 0", "unload_v"});
    s.extract = {{"vulkan_tps", "timings.predicted_per_second"}};
    s.status = StepStatus::Completed;
    s.duration_ms = 4200;
    j.steps.push_back(s);

    Job back = Job::from_json(j.to_json());
    check("json: job fields", back.id == "job-1" && back.name == "bench-duel"
              && back.status == JobStatus::Running && back.cursor == "run_v");
    check("json: step definition survives",
          back.steps.size() == 1 && back.steps[0].op == "load"
              && back.steps[0].on_fail == "load_v_lo"
              && back.steps[0].branch.size() == 1
              && back.steps[0].branch[0].goto_id == "unload_v"
              && back.steps[0].extract["vulkan_tps"] == "timings.predicted_per_second");
    check("json: step runtime survives",
          back.steps[0].status == StepStatus::Completed && back.steps[0].duration_ms == 4200);

    std::set<std::string> ops = {"load"};

    back.steps.push_back(step("unload_v", "load"));
    back.steps.push_back(step("load_v_lo", "load"));
    check("json: round-tripped graph validates", validate_steps(back.steps, ops).empty());
}

int main() {
    test_valid_graphs();
    test_invalid_graphs();
    test_json_roundtrip();
    if (g_failures) {
        std::printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("ALL PASS (0 failures)\n");
    return 0;
}
