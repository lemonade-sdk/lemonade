// Operation interface and registry for the job engine. An op is a named,
// reference-resolved unit of work the engine invokes with concrete params, the
// job context, and a cooperative cancel flag. Ops declared here are generic and
// know nothing about any particular recipe.
#pragma once

#include "lemon/jobs/job_types.h"

#include <atomic>
#include <functional>
#include <map>
#include <set>
#include <string>

namespace lemon {
namespace jobs {

using CancelFlag = std::atomic<bool>;

struct OpHandler {
    std::function<json(const json& params, const json& context, CancelFlag& cancel)> run;
    bool exclusive = false;
};

class OpRegistry {
public:
    void register_op(const std::string& name, OpHandler handler);
    const OpHandler* find(const std::string& name) const;
    std::set<std::string> names() const;

private:
    std::map<std::string, OpHandler> handlers_;
};

// The concrete data sources the read-only ops need, injected so job_ops.cpp does
// not depend on the whole Server. A model_get returning null signals "unknown".
struct OpProviders {
    std::function<json()> system_info;
    std::function<json()> system_stats;
    std::function<json()> models_list;
    std::function<json(const std::string& id)> model_get;
};

OpRegistry build_op_registry(OpProviders providers);

}  // namespace jobs
}  // namespace lemon
