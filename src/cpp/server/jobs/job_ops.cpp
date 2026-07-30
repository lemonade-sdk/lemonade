#include "lemon/jobs/job_ops.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace lemon {
namespace jobs {

void OpRegistry::register_op(const std::string& name, OpHandler handler) {
    handlers_[name] = std::move(handler);
}

const OpHandler* OpRegistry::find(const std::string& name) const {
    auto it = handlers_.find(name);
    return it == handlers_.end() ? nullptr : &it->second;
}

std::set<std::string> OpRegistry::names() const {
    std::set<std::string> out;
    for (const auto& kv : handlers_) out.insert(kv.first);
    return out;
}

OpRegistry build_op_registry(OpProviders providers) {
    OpRegistry reg;

    reg.register_op("system_info", {[providers](const json&, const json&, CancelFlag&) -> json {
        return providers.system_info ? providers.system_info() : json::object();
    }, false});

    reg.register_op("system_stats", {[providers](const json&, const json&, CancelFlag&) -> json {
        return providers.system_stats ? providers.system_stats() : json::object();
    }, false});

    reg.register_op("models", {[providers](const json& params, const json&, CancelFlag&) -> json {
        if (params.contains("id")) {
            const std::string id = params["id"].get<std::string>();
            json model = providers.model_get ? providers.model_get(id) : json(nullptr);
            if (model.is_null()) throw JobError(404, "unknown model '" + id + "'");
            return model;
        }
        return providers.models_list ? providers.models_list() : json::object();
    }, false});

    reg.register_op("sleep", {[](const json& params, const json&, CancelFlag& cancel) -> json {
        using clock = std::chrono::steady_clock;

        const int64_t total_ms =
            std::max<int64_t>(0, params.value("ms", int64_t{0}));
        const auto deadline = clock::now() + std::chrono::milliseconds(total_ms);

        while (!cancel.load()) {
            const auto now = clock::now();
            if (now >= deadline) break;

            const auto next_poll = now + std::chrono::milliseconds(50);
            std::this_thread::sleep_until(next_poll < deadline ? next_poll : deadline);
        }
        return json::object();
    }, false});

    reg.register_op("load", {[providers](const json& params, const json&, CancelFlag& cancel) -> json {
        if (!providers.load_op) throw JobError(501, "load op not available");
        return providers.load_op(params, cancel);
    }, true});

    reg.register_op("unload", {[providers](const json& params, const json&, CancelFlag& cancel) -> json {
        if (!providers.unload_op) throw JobError(501, "unload op not available");
        return providers.unload_op(params, cancel);
    }, true});

    reg.register_op("chat", {[providers](const json& params, const json&, CancelFlag& cancel) -> json {
        if (!providers.chat_op) throw JobError(501, "chat op not available");
        return providers.chat_op(params, cancel);
    }, true});

    reg.begin_exclusive = providers.begin_exclusive;
    reg.end_exclusive = providers.end_exclusive;
    reg.reconcile_unload = providers.reconcile_unload;
    reg.restore_exclusive = providers.restore_exclusive;
    reg.discard_exclusive = providers.discard_exclusive;

    return reg;
}

}
}
