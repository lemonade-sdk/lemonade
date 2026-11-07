#include "lemon/agents/host_routes.h"

#include <chrono>

#include "lemon/agents/agent_installer.h"
#include "lemon/agents/agent_pins.h"
#include "lemon/agents/host_protocol.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/version.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace lemon::agents {

void Responder::event(const std::string& name, const json& data) {
    if (!responded_) {
        send_(protocol::event(id_, name, data));
    }
}

void Responder::respond(int status, const json& body) {
    if (!responded_) {
        responded_ = true;
        send_(protocol::response(id_, status, body));
    }
}

HostRoutes::Installing::Installing(HostRoutes& routes, const std::string& id)
    : routes_(routes), id_(id) {
    std::lock_guard<std::mutex> lock(routes_.mutex_);
    if (!routes_.installing_.insert(id_).second) {
        id_.clear();
        throw AgentError(409, "agent_busy", "The agent is already being installed or removed.");
    }
}

HostRoutes::Installing::~Installing() {
    if (!id_.empty()) {
        std::lock_guard<std::mutex> lock(routes_.mutex_);
        routes_.installing_.erase(id_);
    }
}

bool HostRoutes::is_installing(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return installing_.count(id) != 0;
}

void HostRoutes::cancel(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cancels_.find(id);
    if (it != cancels_.end()) {
        *it->second = true;
    }
}

void HostRoutes::handle(const json& request, Responder& responder) {
    const uint64_t id = request["id"].get<uint64_t>();
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancels_[id] = cancelled;
    }
    in_flight_++;
    struct Done {
        HostRoutes& routes;
        uint64_t id;
        std::shared_ptr<std::atomic<bool>> cancelled;
        ~Done() {
            {
                // After a reconnect lemond can reuse an id still running here.
                std::lock_guard<std::mutex> lock(routes.mutex_);
                auto it = routes.cancels_.find(id);
                if (it != routes.cancels_.end() && it->second == cancelled) {
                    routes.cancels_.erase(it);
                }
            }
            routes.in_flight_--;
        }
    } done{*this, id, cancelled};
    const std::string route = request["route"].get<std::string>();
    const json& params = request["params"];
    const json body = request["body"].is_object() ? request["body"] : json::object();
    const std::string agent = params.value("id", "");

    try {
        if (!protocol::constant_time_equal(request["token"].get<std::string>(), ctx_.token)) {
            throw AgentError(403, "agent_token_invalid", "The agent token doesn't match.");
        }
        json result;
        if (route == "lemonade.agents_list") {
            result = list();
        } else if (route == "lemonade.agents_install") {
            result = install(agent, body, responder, cancelled);
        } else if (route == "lemonade.agents_uninstall") {
            result = uninstall(agent);
        } else if (route == "lemonade.agents_configure") {
            result = configure(agent, body);
        } else if (route == "lemonade.agents_launch") {
            result = launch(agent, body, responder);
        } else {
            throw AgentError(404, "unknown_route", "The agent host doesn't serve " + route + ".");
        }
        responder.respond(200, result);
    } catch (const AgentError& e) {
        responder.respond(e.status, protocol::error_body(e.code, e.what(), e.extra));
    } catch (const std::exception& e) {
        LOG(ERROR, "AgentHost") << route << " failed: " << e.what() << std::endl;
        responder.respond(500, protocol::error_body("agent_host_error", e.what()));
    }
}

json HostRoutes::list() {
    json agents = json::array();
    for (const auto& agent : agent_descriptors()) {
        if (agent.id == "pi") {
            agents.push_back(pi_status(ctx_));
        }
    }
    return {{"agents", agents},
            {"host",
             {{"version", LEMON_VERSION_STRING},
              {"platform", host_os() + "-" + host_arch()},
              {"bin_dir", ctx_.paths.bin_dir.string()},
              {"data_dir", ctx_.paths.data_root.string()}}}};
}

json HostRoutes::install(const std::string& id, const json& body, Responder& responder,
                         const std::shared_ptr<std::atomic<bool>>& cancelled) {
    const auto& agent = find_agent(id);
    const std::string agent_dir = requested_agent_dir(body);
    Installing guard(*this, agent.id);
    const AgentPin pin = resolve_pin(agent, load_agent_versions(), host_os(), host_arch(),
                                     ctx_.release_base_url);
    responder.event("status", {{"message", "Installing " + agent.name + " " + pin.version}});

    auto last_report = std::chrono::steady_clock::time_point();
    int last_percent = -1;
    const InstallProgress progress = [&](size_t done, size_t total) {
        const int percent = total > 0 ? static_cast<int>(done * 100 / total) : -1;
        const auto now = std::chrono::steady_clock::now();
        if (percent != last_percent &&
            (now - last_report > std::chrono::milliseconds(200) || percent == 100)) {
            last_percent = percent;
            last_report = now;
            // The fields the CLI's pull progress already renders.
            responder.event("progress", {{"file", pin.asset},
                                         {"file_index", 1},
                                         {"total_files", 1},
                                         {"bytes_downloaded", done},
                                         {"bytes_total", total}});
        }
        return !cancelled->load();
    };
    lemon::utils::CancelToken cancel;
    cancel.flag = cancelled.get();

    InstallOutcome outcome = install_agent(agent, pin, ctx_.paths, body.value("force", false),
                                           progress, cancel, ctx_.download_policy, &ctx_.leases);
    // pi's own config is best effort: the install stands without it.
    try {
        for (auto& warning : pi_configure(ctx_, std::nullopt, agent_dir)) {
            outcome.warnings.push_back(warning);
        }
    } catch (const AgentError& e) {
        outcome.warnings.push_back(std::string("Can't configure ") + agent.name + ": " + e.what());
    }
    for (const auto& warning : outcome.warnings) {
        responder.event("warning", {{"message", warning}});
    }
    return {{"agent", agent.id},
            {"version", outcome.installed.version},
            {"binary", outcome.installed.binary.string()},
            {"downloaded", outcome.downloaded},
            {"shim",
             {{"path", shim_path(ctx_.paths, agent.binary).string()},
              {"state", shim_state_name(outcome.shim)}}},
            {"warnings", outcome.warnings}};
}

json HostRoutes::uninstall(const std::string& id) {
    const auto& agent = find_agent(id);
    Installing guard(*this, agent.id);
    std::vector<std::string> warnings;
    const bool removed = uninstall_agent(agent, ctx_.paths, warnings, &ctx_.leases);
    return {{"removed", removed}, {"warnings", warnings}};
}

json HostRoutes::configure(const std::string& id, const json& body) {
    find_agent(id);
    std::optional<bool> mcp;
    if (body.contains("mcp") && body["mcp"].is_boolean()) {
        mcp = body["mcp"].get<bool>();
    }
    return {{"warnings", pi_configure(ctx_, mcp, requested_agent_dir(body))}};
}

json HostRoutes::launch(const std::string& id, const json& body, Responder& responder) {
    const auto& agent = find_agent(id);
    return pi_launch(
        ctx_, body,
        [&responder](const std::string& event, const json& data) { responder.event(event, data); },
        [this, &agent] { return is_installing(agent.id); });
}

} // namespace lemon::agents
