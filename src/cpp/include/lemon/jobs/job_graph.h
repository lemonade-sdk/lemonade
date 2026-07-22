#pragma once

#include "lemon/jobs/job_expr.h"
#include "lemon/jobs/job_types.h"

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace lemon {
namespace jobs {

inline std::string validate_steps(const std::vector<StepRecord>& steps,
                                  const std::set<std::string>& known_ops) {
    if (steps.empty()) return "a job needs at least one step";

    std::unordered_map<std::string, int> index;
    for (int i = 0; i < (int)steps.size(); ++i) {
        const std::string& id = steps[i].id;
        if (id.empty()) return "step " + std::to_string(i) + " has an empty id";
        if (id == "inputs") return "step id 'inputs' is reserved";
        if (id.find('.') != std::string::npos)
            return "step id '" + id + "' must not contain '.'";
        if (index.count(id)) return "duplicate step id '" + id + "'";
        index[id] = i;
    }

    auto forward_target = [&](int from, const std::string& target,
                              const char* field) -> std::string {
        auto it = index.find(target);
        if (it == index.end())
            return std::string(field) + " target '" + target + "' in step '" + steps[from].id
                   + "' is not a known step id";
        if (it->second <= from)
            return std::string(field) + " target '" + target + "' in step '" + steps[from].id
                   + "' must reference a later step (no loops)";
        return "";
    };

    auto check_expr = [](const std::string& e, const std::string& where) -> std::string {
        if (e.empty()) return "";
        const std::string err = check_expression_syntax(e);
        if (!err.empty()) return where + ": " + err;
        return "";
    };

    for (int i = 0; i < (int)steps.size(); ++i) {
        const StepRecord& s = steps[i];
        if (s.op.empty()) return "step '" + s.id + "' has no op";
        if (!known_ops.count(s.op)) return "step '" + s.id + "' uses unknown op '" + s.op + "'";

        std::string e = check_expr(s.when, "step '" + s.id + "' when");
        if (!e.empty()) return e;

        if (!s.on_done.empty()) {
            std::string err = forward_target(i, s.on_done, "on_done");
            if (!err.empty()) return err;
        }
        for (const Case& c : s.branch) {
            if (c.goto_id.empty()) return "a branch case in step '" + s.id + "' has no goto";
            std::string err = forward_target(i, c.goto_id, "branch goto");
            if (!err.empty()) return err;
            std::string ce = check_expr(c.when, "step '" + s.id + "' branch when");
            if (!ce.empty()) return ce;
        }
        if (s.on_fail != kOnFailAbort && s.on_fail != kOnFailContinue) {
            std::string err = forward_target(i, s.on_fail, "on_fail");
            if (!err.empty()) return err;
        }
    }
    return "";
}

}
}
