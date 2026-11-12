#include <chrono>

#include "lemon/agents/host_protocol.h"
#include "lemon/server/agent_host_registry.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;
namespace protocol = agents::protocol;

const RouteArg kId{"id", ArgIn::Path, {{"type", "string"}, {"enum", {"pi"}}}, true,
                   Support::Available, "The agent. Lemonade manages `pi`."};
const RouteArg kStream{"stream", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                       "Stream progress as server-sent events. Defaults to `true`."};
const RouteArg kForce{"force", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                      "Download and install again even if the pinned version is installed."};
const RouteArg kMcp{"mcp", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                    "`true` adds Lemonade's MCP server to the agent, `false` removes it; unset "
                    "refreshes an existing entry."};
const RouteArg kAgentDir{"agent_dir", ArgIn::JsonBody, {{"type", "string"}}, false,
                         Support::Available,
                         "The agent's configuration directory in the client's environment, such "
                         "as pi's `PI_CODING_AGENT_DIR`. Defaults to the host's."};
const RouteArg kModel{"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
                      "The model the agent starts with."};
const RouteArg kCwd{"cwd", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
                    "Absolute working directory for the agent."};
const RouteArg kArgs{"args", ArgIn::JsonBody, {{"type", "array"}, {"items", {{"type", "string"}}}},
                     false, Support::Available, "Extra arguments for the agent."};
const RouteArg kClientHasApiKey{"client_has_api_key", ArgIn::JsonBody, {{"type", "boolean"}}, false,
                                Support::Available,
                                "Whether the client gives the agent `LEMONADE_API_KEY`, so its "
                                "config references the key instead of a placeholder."};
const RouteArg kCodemode{"codemode", ArgIn::JsonBody, {{"type", "boolean"}}, false,
                         Support::Available, "Start pi with its `codemode` tool."};
const RouteArg kRecipeOptions{"recipe_options", ArgIn::JsonBody, {{"type", "object"}}, false,
                              Support::Available,
                              "Load options for the model, such as `ctx_size`, which also sets "
                              "the agent's context window."};
const RouteArg kSearchPath{"search_path", ArgIn::JsonBody, {{"type", "string"}}, false,
                           Support::Available,
                           "The client's `PATH`, searched for an agent Lemonade didn't install."};

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
const RouteError kUnsupportedPlatform{422, "unsupported_platform",
                                      "the agent has no release for this OS and CPU"};
const RouteError kDownloadFailed{502, "download_failed", "the release couldn't be downloaded"};
const RouteError kChecksumMismatch{502, "checksum_mismatch",
                                   "the download doesn't match its pinned checksum"};
const RouteError kInvalidCwd{400, "invalid_cwd", "`cwd` isn't an absolute directory"};
const RouteError kModelLoadFailed{424, "model_load_failed", "the model failed to load"};
const RouteError kUninstallFailed{500, "uninstall_failed",
                                  "the shim couldn't be removed, or removal stopped partway"};
const RouteError kConfigWriteFailed{500, "config_write_failed",
                                    "the agent's configuration couldn't be written"};

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

const char* kWarningsField = R"("warnings": {"type": "array", "items": {"type": "string"},
    "description": "Problems that didn't stop the request, such as a foreign shim."})";

const char* kErrorFields = R"(
    "error": {"type": "string", "description": "Error event: what went wrong."},
    "code": {"type": "string", "description": "Error event: the error's code, as for a non-streamed error."},
    "status": {"type": "integer", "description": "Error event: the HTTP status a non-streamed request would have had."})";

ordered_json install_result_schema(bool stream) {
    std::string fields = std::string(R"(
        "agent": {"type": "string", "description": "The agent installed."},
        "version": {"type": "string", "description": "The installed version, which is the pinned one."},
        "binary": {"type": "string", "description": "Absolute path of the installed agent."},
        "downloaded": {"type": "boolean", "description": "`false` when the pinned version was already installed."},
        "shim": {"type": "object", "description": "The shim Lemonade writes, with its `path` and `state`: `ours`, `foreign` (someone else's file, left alone) or `absent`."},
        )") + kWarningsField;
    if (stream) {
        fields += std::string(R"(,
        "message": {"type": "string", "description": "Status and warning events: a line for the user."},
        "file": {"type": "string", "description": "Progress events: the release asset being downloaded."},
        "file_index": {"type": "integer", "description": "Progress events: always `1`."},
        "total_files": {"type": "integer", "description": "Progress events: always `1`."},
        "bytes_downloaded": {"type": "integer", "description": "Progress events: bytes downloaded so far."},
        "bytes_total": {"type": "integer", "description": "Progress events: the asset's size."},)") +
                  kErrorFields;
    }
    ordered_json schema = object_schema(("{" + fields + "}").c_str());
    schema["description"] = stream ? "`status`, `progress` and `warning` events, then `complete` "
                                     "with the result or `error`."
                                   : "The install's result.";
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
        ordered_json schema = object_schema(R"({
            "agents": {"type": "array", "description": "One entry per agent Lemonade manages.",
                "items": {"type": "object", "properties": {
                    "id": {"type": "string", "description": "The agent's id, as routes take it."},
                    "name": {"type": "string", "description": "The agent's display name."},
                    "pinned": {"type": ["string", "null"], "description": "The version this Lemonade installs."},
                    "sandbox": {"type": "string", "description": "Always `off`; reserved for running agents in a sandbox."},
                    "installed": {"type": ["object", "null"], "description": "The installed `version` and its `binary`, or `null`."},
                    "external": {"type": ["object", "null"], "description": "The `binary` of a copy Lemonade didn't install, found on the host's `PATH`, or `null`."},
                    "active": {"type": "string", "enum": ["managed", "external", "none"], "description": "Which copy launch uses."},
                    "shim": {"type": "object", "description": "The shim's `path`, its `state` (`ours`, `foreign` or `absent`), whether its directory is `on_path`, and the first match on `PATH` (`resolves_to`)."},
                    "pin_error": {"type": "string", "description": "Why there's no pinned version for this platform, when there isn't."}
                }}},
            "host": {"type": "object", "description": "The host's `version`, `platform`, shim directory (`bin_dir`) and install directory (`data_dir`)."}
        })");
        schema["description"] = "The agents and the host serving them.";
        s.responses = {{ResponseFormat::Json, schema}};
        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = ordered_json::object();
        s.examples = {example};
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
        s.args = {kId, kStream, kForce, kAgentDir};
        s.responses = {{ResponseFormat::EventStream, install_result_schema(true)},
                       {ResponseFormat::Json, install_result_schema(false)}};
        RouteExample streaming;
        streaming.name = "streaming";
        streaming.format = ResponseFormat::EventStream;
        streaming.args = {{"id", "pi"}};
        RouteExample installed;
        installed.name = "installed";
        installed.description = "Installing the pinned version again does nothing.";
        installed.format = ResponseFormat::Json;
        installed.args = {{"id", "pi"}, {"stream", false}};
        installed.setup = {{"lemonade.agents_install", "streaming"}};
        s.examples = {streaming, installed};
        s.errors = {kBodyNotJson, kArgInvalid, kTokenMissing, kHostUnavailable, kHostBusy,
                    kUnknownAgent, kUnsupportedPlatform, kDownloadFailed, kChecksumMismatch};
        return s;
    }

protected:
    bool streaming(const RouteRequest& req) const override {
        return req.value(kStream, true);
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
        s.errors = {kTokenMissing, kHostUnavailable, kHostBusy, kHostTimeout, kUnknownAgent,
                    kUninstallFailed};
        ordered_json schema = object_schema((std::string(R"({
            "removed": {"type": "boolean", "description": "`false` when nothing was installed."},
            )") + kWarningsField + "}").c_str());
        schema["description"] = "Whether anything was removed.";
        s.responses = {{ResponseFormat::Json, schema}};
        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = {{"id", "pi"}};
        example.setup = {{"lemonade.agents_install", "streaming"}};
        s.examples = {example};
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
        s.args = {kId, kMcp, kAgentDir};
        s.errors = {kBodyNotJson, kArgInvalid, kTokenMissing, kHostUnavailable, kHostBusy,
                    kHostTimeout, kUnknownAgent, kConfigWriteFailed};
        ordered_json schema = object_schema((std::string("{") + kWarningsField + "}").c_str());
        schema["description"] = "Problems that didn't stop the configuration.";
        s.responses = {{ResponseFormat::Json, schema}};
        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = {{"id", "pi"}, {"mcp", true}};
        s.examples = {example};
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
        s.args = {kId,   kModel,    kCwd,          kArgs,     kClientHasApiKey,
                  kMcp,  kCodemode, kRecipeOptions, kAgentDir, kSearchPath};
        ordered_json schema = object_schema((std::string(R"({
            "message": {"type": "string", "description": "Status and warning events: a line for the user."},
            "binary": {"type": "string", "description": "Complete event: absolute path of the agent to run."},
            "args": {"type": "array", "items": {"type": "string"}, "description": "Complete event: its arguments."},
            "cwd": {"type": "string", "description": "Complete event: the directory to run it in, as requested."},
            "env": {"type": "object", "description": "Complete event: variables to add to its environment. They never include an API key; the client adds its own `LEMONADE_API_KEY`."},
            "model_loaded": {"type": "boolean", "description": "Complete event: whether preparing the launch loaded the model, so the client needn't."},
            "managed": {"type": "boolean", "description": "Complete event: whether `binary` is the copy Lemonade installed."},
            )") + kErrorFields + "}").c_str());
        schema["description"] = "`status` and `warning` events, then `complete` with the command "
                                "to run, or `error`.";
        s.responses = {{ResponseFormat::EventStream, schema}};
        RouteExample example;
        example.format = ResponseFormat::EventStream;
        example.args = ordered_json::parse(R"({
            "id": "pi",
            "model": "Qwen3-0.6B-GGUF",
            "cwd": "/tmp",
            "args": ["--thinking", "off"],
            "recipe_options": {"ctx_size": 4096}
        })");
        example.setup = {{"lemonade.agents_install", "streaming"}};
        s.examples = {example};
        s.errors = {kBodyNotJson,     kArgMissing,      kArgInvalid,    kTokenMissing,
                    kHostUnavailable, kHostBusy,        kUnknownAgent,  kNotInstalled,
                    kInvalidCwd,      kModelLoadFailed, kConfigWriteFailed};
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
