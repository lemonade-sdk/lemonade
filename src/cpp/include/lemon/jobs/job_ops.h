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

    std::function<bool(const std::string& job_id, CancelFlag*)> begin_exclusive;
    std::function<void()> end_exclusive;

    std::function<void(const std::string& job_id)> reconcile_unload;
    std::function<bool(const std::string& job_id, const json& manifest, CancelFlag*)> restore_exclusive;
    std::function<void(const std::string& job_id)> discard_exclusive;

private:
    std::map<std::string, OpHandler> handlers_;
};

struct OpProviders {
    std::function<json()> system_info;
    std::function<json()> system_stats;
    std::function<json()> models_list;
    std::function<json(const std::string& id)> model_get;

    std::function<json(const json& params, CancelFlag& cancel)> load_op;
    std::function<json(const json& params, CancelFlag& cancel)> unload_op;
    std::function<json(const json& params, CancelFlag& cancel)> chat_op;

    std::function<bool(const std::string& job_id, CancelFlag*)> begin_exclusive;
    std::function<void()> end_exclusive;
    std::function<void(const std::string& job_id)> reconcile_unload;
    std::function<bool(const std::string& job_id, const json& manifest, CancelFlag*)> restore_exclusive;
    std::function<void(const std::string& job_id)> discard_exclusive;
};

OpRegistry build_op_registry(OpProviders providers);

}
}
