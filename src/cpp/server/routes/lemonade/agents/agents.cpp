#include <chrono>

#include "lemon/agents/host_protocol.h"
#include "lemon/server/agent_host_registry.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;
namespace protocol = agents::protocol;

const RouteArg kId{"id", ArgIn::Path, {{"type", "string"}}, true, Support::Available,
                   "The agent, e.g. `pi`."};

const RouteError kTokenMissing{400, "agent_token_missing",
                               "the request has no `X-Lemonade-Agent-Token` header"};
const RouteError kHostUnavailable{503, "agent_host_unavailable",
                                  "no agent host with the request's token is connected"};
const RouteError kHostBusy{409, "agent_busy",
                           "the agent is being installed or removed, or too many agent "
                           "requests are in flight"};
const RouteError kHostTimeout{504, "agent_host_timeout", "the agent host didn't answer in time"};
const RouteError kUnknownAgent{404, "unknown_agent", "`id` names no agent Lemonade manages"};
const RouteError kNotInstalled{409, "agent_not_installed",
                               "the agent is neither installed by Lemonade nor on `PATH`"};

constexpr auto kReplyTimeout = std::chrono::seconds(60);
constexpr auto kTick = std::chrono::seconds(1);
// A keep-alive comment lets a relay notice a client that went away while
// the host works without sending anything, such as during a model load.
constexpr int kKeepAliveTicks = 10;

const char* kTokenNote =
    "Every `/v1/agents` request carries the `X-Lemonade-Agent-Token` header with the token "
    "the user's agent host wrote to its state directory. lemond relays the request to that host "
    "over the host's WebSocket at `/v1/agents/host`; see [Agent host protocol]"
    "(#agent-host-protocol).";

class AgentRelayRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

protected:
    RouteSpec base_spec(const std::string& name) const {
        RouteSpec s;
        s.id = "lemonade.agents_" + name;
        s.experimental = true;
        s.notes = {kTokenNote};
        s.errors = {kTokenMissing, kHostUnavailable, kHostBusy, kHostTimeout, kUnknownAgent};
        return s;
    }

    virtual bool streaming(const RouteRequest&) const { return false; }
    // Installs take as long as the download, so they wait for the host.
    virtual bool bounded() const { return true; }

    void handle(RouteRequest& req, httplib::Response& res) override {
        const std::string token = req.http.get_header_value(protocol::kTokenHeader);
        if (token.empty()) {
            write_plain_error(res, kTokenMissing,
                              "Agent requests need the X-Lemonade-Agent-Token header.");
            return;
        }
        const json body = req.body.is_object() ? req.body : json::object();
        AgentHostRegistry::OpenError error;
        std::shared_ptr<AgentHostRegistry::Exchange> exchange =
            ctx_.agent_hosts->open(token, spec().id, req.params, body, error);
        if (!exchange) {
            if (error == AgentHostRegistry::OpenError::Busy) {
                write_plain_error(res, kHostBusy, "Too many agent requests are in flight.");
            } else {
                write_plain_error(res, kHostUnavailable,
                                  "No agent host is connected for this user. Run `lemonade "
                                  "agents list` on this machine to start one.");
            }
            return;
        }

        if (streaming(req)) {
            stream_response(req, res, [exchange](const std::string&, httplib::DataSink& sink) {
                relay_stream(*exchange, sink);
            });
            return;
        }

        const auto started = std::chrono::steady_clock::now();
        while (true) {
            auto frame = exchange->next(kTick);
            if (!frame) {
                if (req.http.is_connection_closed && req.http.is_connection_closed()) {
                    exchange->cancel();
                    return;
                }
                if (bounded() && std::chrono::steady_clock::now() - started >= kReplyTimeout) {
                    write_plain_error(res, kHostTimeout, "The agent host didn't answer in time.");
                    return;
                }
                continue;
            }
            if ((*frame)["type"] == "response") {
                res.status = (*frame)["status"].get<int>();
                res.set_content(protocol::serialize((*frame)["body"]), "application/json");
                return;
            }
        }
    }

private:
    static void relay_stream(AgentHostRegistry::Exchange& exchange, httplib::DataSink& sink) {
        int idle_ticks = 0;
        while (true) {
            auto frame = exchange.next(kTick);
            if (!frame) {
                if (++idle_ticks >= kKeepAliveTicks) {
                    idle_ticks = 0;
                    static const std::string keep_alive = ": keep-alive\n\n";
                    if (!sink.write(keep_alive.data(), keep_alive.size())) {
                        exchange.cancel();
                        return;
                    }
                }
                continue;
            }
            idle_ticks = 0;
            if ((*frame)["type"] == "event") {
                if (!write_sse_event(sink, (*frame)["event"].get<std::string>(),
                                     (*frame)["data"])) {
                    exchange.cancel();
                    return;
                }
                continue;
            }
            const int status = (*frame)["status"].get<int>();
            json data = (*frame)["body"];
            if (status >= 200 && status < 300) {
                write_sse_event(sink, "complete", data);
            } else {
                if (!data.is_object()) {
                    data = json::object();
                }
                data["status"] = status;
                write_sse_event(sink, "error", data);
            }
            sink.done();
            return;
        }
    }
};

ordered_json object_schema(const char* properties) {
    ordered_json schema = {{"type", "object"}};
    schema["properties"] = ordered_json::parse(properties);
    return schema;
}

class AgentsListRoute : public AgentRelayRoute {
public:
    using AgentRelayRoute::AgentRelayRoute;

    RouteSpec spec() const override {
        RouteSpec s = base_spec("list");
        s.methods = {"GET"};
        s.paths = {"agents"};
        s.summary = "List the agents Lemonade manages";
        s.description =
            "Reports, for each agent Lemonade can manage, the pinned version, the installed "
            "version, any copy found on `PATH`, which of them is in use and the state of the "
            "shim Lemonade writes.";
        s.responses = {{ResponseFormat::Json, object_schema(R"({
            "agents": {"type": "array", "items": {"type": "object"}},
            "host": {"type": "object", "description": "The host's version, platform and paths."}
        })")}};
        s.errors = {kTokenMissing, kHostUnavailable, kHostBusy, kHostTimeout};
        return s;
    }
};

class AgentsInstallRoute : public AgentRelayRoute {
public:
    using AgentRelayRoute::AgentRelayRoute;

    RouteSpec spec() const override {
        RouteSpec s = base_spec("install");
        s.methods = {"POST"};
        s.paths = {"agents/{id}/install"};
        s.summary = "Install or update an agent to its pinned version";
        s.description =
            "Downloads the pinned release, verifies its SHA-256, installs it under the user's "
            "data directory, writes the shim and configures the agent for this server. "
            "Installing the version that is already installed does nothing unless `force` is "
            "set.";
        s.request_format = RequestFormat::OptionalJson;
        s.args = {kId,
                  {"stream", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                   "Stream progress as server-sent events. Default `true`."},
                  {"force", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                   "Download and install again even if the version is installed."}};
        s.responses = {{ResponseFormat::EventStream, {{"type", "object"}}},
                       {ResponseFormat::Json, {{"type", "object"}}}};
        s.errors.push_back({422, "unsupported_platform", "the agent has no release for this OS"});
        s.errors.push_back({502, "download_failed", "the release couldn't be downloaded"});
        s.errors.push_back({502, "checksum_mismatch", "the release doesn't match its pin"});
        return s;
    }

protected:
    bool streaming(const RouteRequest& req) const override {
        return !req.body.is_object() || req.body.value("stream", true);
    }
    bool bounded() const override { return false; }
};

class AgentsUninstallRoute : public AgentRelayRoute {
public:
    using AgentRelayRoute::AgentRelayRoute;

    RouteSpec spec() const override {
        RouteSpec s = base_spec("uninstall");
        s.methods = {"DELETE"};
        s.paths = {"agents/{id}"};
        s.summary = "Remove an installed agent";
        s.description =
            "Removes the agent's installed versions and Lemonade's shim. The agent's own "
            "settings, such as `~/.pi/agent`, are kept.";
        s.args = {kId};
        s.responses = {{ResponseFormat::Json, object_schema(R"({
            "removed": {"type": "boolean"},
            "warnings": {"type": "array", "items": {"type": "string"}}
        })")}};
        return s;
    }
};

class AgentsConfigureRoute : public AgentRelayRoute {
public:
    using AgentRelayRoute::AgentRelayRoute;

    RouteSpec spec() const override {
        RouteSpec s = base_spec("configure");
        s.methods = {"POST"};
        s.paths = {"agents/{id}/configure"};
        s.summary = "Point an agent at this server";
        s.description =
            "Writes the Lemonade provider with every chat model into the agent's own "
            "configuration, and adds, refreshes or removes Lemonade's MCP server.";
        s.request_format = RequestFormat::OptionalJson;
        s.args = {kId,
                  {"mcp", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                   "`true` adds Lemonade's MCP server, `false` removes it; unset refreshes an "
                   "existing entry."},
                  {"agent_dir", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                   "The agent's configuration directory, when the client's differs from the "
                   "default."}};
        s.responses = {{ResponseFormat::Json, object_schema(R"({
            "warnings": {"type": "array", "items": {"type": "string"}}
        })")}};
        return s;
    }
};

class AgentsLaunchRoute : public AgentRelayRoute {
public:
    using AgentRelayRoute::AgentRelayRoute;

    RouteSpec spec() const override {
        RouteSpec s = base_spec("launch");
        s.methods = {"POST"};
        s.paths = {"agents/{id}/launch"};
        s.summary = "Prepare an agent launch";
        s.description =
            "Configures the agent for the model and returns the command the client runs. The "
            "host runs nothing itself. Status and warning events come first; the final "
            "`complete` event carries `binary`, `args`, `cwd`, `env` and `model_loaded`.";
        s.request_format = RequestFormat::Json;
        s.args = {kId,
                  {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
                   "The model the agent starts with."},
                  {"cwd", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
                   "Absolute working directory for the agent."},
                  {"args", ArgIn::JsonBody, {{"type", "array"}, {"items", {{"type", "string"}}}},
                   false, Support::Available, "Extra arguments passed to the agent."},
                  {"client_has_api_key", ArgIn::JsonBody, {{"type", "boolean"}}, false,
                   Support::Available,
                   "Whether the client exports `LEMONADE_API_KEY` to the agent."},
                  {"mcp", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                   "As for configure."},
                  {"codemode", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                   "Start pi with its codemode tool."},
                  {"recipe_options", ArgIn::JsonBody, {{"type", "object"}}, false,
                   Support::Available, "Load options, such as `ctx_size`, for the model."},
                  {"agent_dir", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                   "The agent's configuration directory in the client's environment."},
                  {"search_path", ArgIn::JsonBody, {{"type", "string"}}, false,
                   Support::Available,
                   "The client's `PATH`, searched for an agent Lemonade didn't install."}};
        s.responses = {{ResponseFormat::EventStream, {{"type", "object"}}}};
        s.errors.push_back(kNotInstalled);
        s.errors.push_back({400, "invalid_cwd", "`cwd` isn't an absolute directory"});
        s.errors.push_back({424, "model_load_failed", "the model failed to load"});
        return s;
    }

protected:
    bool streaming(const RouteRequest&) const override { return true; }
};

} // namespace

std::unique_ptr<ApiRoute> make_agents_list_route(ServerContext& ctx) {
    return std::make_unique<AgentsListRoute>(ctx);
}
std::unique_ptr<ApiRoute> make_agents_install_route(ServerContext& ctx) {
    return std::make_unique<AgentsInstallRoute>(ctx);
}
std::unique_ptr<ApiRoute> make_agents_uninstall_route(ServerContext& ctx) {
    return std::make_unique<AgentsUninstallRoute>(ctx);
}
std::unique_ptr<ApiRoute> make_agents_configure_route(ServerContext& ctx) {
    return std::make_unique<AgentsConfigureRoute>(ctx);
}
std::unique_ptr<ApiRoute> make_agents_launch_route(ServerContext& ctx) {
    return std::make_unique<AgentsLaunchRoute>(ctx);
}

} // namespace lemon
