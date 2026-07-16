#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace lemon {
namespace jobs {

using json = nlohmann::ordered_json;

class JobError : public std::runtime_error {
public:
    JobError(int status, std::string message)
        : std::runtime_error(std::move(message)), status(status) {}
    int status;
};

enum class JobStatus { Queued, Running, Paused, Interrupted, Completed, Failed };
enum class StepStatus { Pending, Running, Completed, Failed, Skipped };

inline std::string to_string(JobStatus s) {
    switch (s) {
        case JobStatus::Queued: return "queued";
        case JobStatus::Running: return "running";
        case JobStatus::Paused: return "paused";
        case JobStatus::Interrupted: return "interrupted";
        case JobStatus::Completed: return "completed";
        default: return "failed";
    }
}

inline JobStatus job_status_from_string(const std::string& s) {
    if (s == "queued") return JobStatus::Queued;
    if (s == "running") return JobStatus::Running;
    if (s == "paused") return JobStatus::Paused;
    if (s == "interrupted") return JobStatus::Interrupted;
    if (s == "completed") return JobStatus::Completed;
    return JobStatus::Failed;
}

inline std::string to_string(StepStatus s) {
    switch (s) {
        case StepStatus::Pending: return "pending";
        case StepStatus::Running: return "running";
        case StepStatus::Completed: return "completed";
        case StepStatus::Failed: return "failed";
        default: return "skipped";
    }
}

inline StepStatus step_status_from_string(const std::string& s) {
    if (s == "running") return StepStatus::Running;
    if (s == "completed") return StepStatus::Completed;
    if (s == "failed") return StepStatus::Failed;
    if (s == "skipped") return StepStatus::Skipped;
    return StepStatus::Pending;
}

struct Case {
    std::string when;
    std::string goto_id;

    json to_json() const { return {{"when", when}, {"goto", goto_id}}; }

    static Case from_json(const json& j) {
        Case c;
        c.when = j.value("when", "");
        c.goto_id = j.value("goto", "");
        return c;
    }
};

inline constexpr const char* kOnFailAbort = "abort";
inline constexpr const char* kOnFailContinue = "continue";

struct StepRecord {

    std::string id;
    std::string op;
    json params = json::object();
    std::string when;
    json extract = json::object();
    std::string on_done;
    std::vector<Case> branch;
    std::string on_fail = kOnFailAbort;

    StepStatus status = StepStatus::Pending;
    bool failure_handled = false;
    int64_t duration_ms = 0;
    std::string error;
    json output = json::object();

    json to_json() const {
        json b = json::array();
        for (const auto& c : branch) b.push_back(c.to_json());
        json j = {{"id", id},
                  {"op", op},
                  {"params", params},
                  {"status", to_string(status)},
                  {"duration_ms", duration_ms}};
        if (!when.empty()) j["when"] = when;
        if (!extract.empty()) j["extract"] = extract;
        if (!on_done.empty()) j["on_done"] = on_done;
        if (!b.empty()) j["branch"] = b;
        if (on_fail != kOnFailAbort) j["on_fail"] = on_fail;
        if (failure_handled) j["failure_handled"] = true;
        if (!error.empty()) j["error"] = error;
        if (!output.empty()) j["output"] = output;
        return j;
    }

    static StepRecord from_json(const json& j) {
        StepRecord s;
        s.id = j.value("id", "");
        s.op = j.value("op", "");
        if (j.contains("params") && j["params"].is_object()) s.params = j["params"];
        s.when = j.value("when", "");
        if (j.contains("extract") && j["extract"].is_object()) s.extract = j["extract"];
        s.on_done = j.value("on_done", "");
        if (j.contains("branch") && j["branch"].is_array())
            for (const auto& c : j["branch"]) s.branch.push_back(Case::from_json(c));
        s.on_fail = j.value("on_fail", std::string(kOnFailAbort));
        s.failure_handled = j.value("failure_handled", false);
        if (j.contains("status")) s.status = step_status_from_string(j["status"].get<std::string>());
        s.duration_ms = j.value("duration_ms", (int64_t)0);
        s.error = j.value("error", "");
        if (j.contains("output")) s.output = j["output"];
        return s;
    }
};

struct Job {
    std::string id;
    std::string name;
    JobStatus status = JobStatus::Queued;
    bool deleted = false;
    json inputs = json::object();
    json context = json::object();
    std::vector<StepRecord> steps;
    std::string cursor;
    std::string created_at;
    std::string started_at;
    std::string finished_at;
    std::string summary;
    std::string error;

    json to_summary_json() const {
        int completed = 0;
        for (const auto& s : steps)
            if (s.status == StepStatus::Completed || s.status == StepStatus::Skipped
                || (s.status == StepStatus::Failed && s.failure_handled)) completed++;
        json j = {{"id", id},
                  {"name", name},
                  {"status", to_string(status)},
                  {"created_at", created_at},
                  {"progress", {{"cursor", cursor},
                                {"completed", completed},
                                {"step_count", (int)steps.size()}}}};
        if (!finished_at.empty()) j["finished_at"] = finished_at;
        if (!summary.empty()) j["summary"] = summary;
        if (!error.empty()) j["error"] = error;
        return j;
    }

    json to_json() const {
        json st = json::array();
        for (const auto& s : steps) st.push_back(s.to_json());
        json j = {{"id", id},
                  {"name", name},
                  {"status", to_string(status)},
                  {"inputs", inputs},
                  {"context", context},
                  {"steps", st},
                  {"cursor", cursor},
                  {"created_at", created_at}};
        if (deleted) j["deleted"] = true;
        if (!started_at.empty()) j["started_at"] = started_at;
        if (!finished_at.empty()) j["finished_at"] = finished_at;
        if (!summary.empty()) j["summary"] = summary;
        if (!error.empty()) j["error"] = error;
        return j;
    }

    static Job from_json(const json& j) {
        Job job;
        job.id = j.value("id", "");
        job.name = j.value("name", "");
        if (j.contains("status")) job.status = job_status_from_string(j["status"].get<std::string>());
        job.deleted = j.value("deleted", false);
        if (j.contains("inputs")) job.inputs = j["inputs"];
        if (j.contains("context")) job.context = j["context"];
        if (j.contains("steps") && j["steps"].is_array())
            for (const auto& s : j["steps"]) job.steps.push_back(StepRecord::from_json(s));
        job.cursor = j.value("cursor", "");
        job.created_at = j.value("created_at", "");
        job.started_at = j.value("started_at", "");
        job.finished_at = j.value("finished_at", "");
        job.summary = j.value("summary", "");
        job.error = j.value("error", "");
        return job;
    }
};

}
}
