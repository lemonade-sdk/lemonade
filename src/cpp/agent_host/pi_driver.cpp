#include "lemon/agents/pi_driver.h"

#include <filesystem>
#include <system_error>

#include <httplib.h>

#include "lemon/agents/agent_installer.h"
#include "lemon/agents/agent_pins.h"
#include "lemon/agents/agent_shim.h"
#include "lemon/utils/path_utils.h"
#include "lemon_cli/agent_launcher.h"
#include "lemon_cli/pi_launch_prep.h"
#include "lemon_cli/pi_profile.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace lemon::agents {

namespace {

std::string server_error_message(const std::string& body, int status) {
    const json parsed = json::parse(body, nullptr, false);
    if (parsed.is_object() && parsed.contains("error")) {
        const json& error = parsed["error"];
        if (error.is_string()) {
            return error.get<std::string>();
        }
        if (error.is_object() && error.contains("message") && error["message"].is_string()) {
            return error["message"].get<std::string>();
        }
    }
    return "HTTP " + std::to_string(status);
}

std::string home_dir() {
#ifdef _WIN32
    return lemon::utils::get_environment_variable_utf8("USERPROFILE");
#else
    return lemon::utils::get_environment_variable_utf8("HOME");
#endif
}

std::string expand_home(const std::string& path) {
    if (path.rfind("~/", 0) == 0) {
        return (lemon::utils::path_from_utf8(home_dir()) / path.substr(2)).string();
    }
    return path;
}

lemon_cli::PrepReport report_to(const EventSink& events, std::vector<std::string>& errors) {
    return [&events, &errors](lemon_cli::PrepLevel level, const std::string& message) {
        switch (level) {
        case lemon_cli::PrepLevel::Status:
            events("status", {{"message", message}});
            break;
        case lemon_cli::PrepLevel::Warning:
            events("warning", {{"message", message}});
            break;
        case lemon_cli::PrepLevel::Error:
            errors.push_back(message);
            break;
        }
    };
}

std::string join(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) {
        out += (out.empty() ? "" : "\n") + line;
    }
    return out;
}

std::string pi_install_hint() {
    return "Install it with `lemonade agents install pi`.";
}

} // namespace

lemon_cli::LemondCall make_lemond_call(const std::string& origin, const std::string& api_key) {
    return [origin, api_key](const std::string& method, const std::string& path,
                             const std::string& body, int timeout_ms) -> std::string {
        httplib::Client client(origin);
        const time_t sec = timeout_ms / 1000;
        const time_t usec = (timeout_ms % 1000) * 1000;
        client.set_connection_timeout(sec, usec);
        client.set_read_timeout(sec, usec);
        if (!api_key.empty()) {
            client.set_bearer_token_auth(api_key);
        }
        const auto result = method == "POST"
                                ? client.Post(path, body, "application/json")
                                : client.Get(path);
        if (!result) {
            throw std::runtime_error("Can't reach Lemonade at " + origin + ": " +
                                     httplib::to_string(result.error()));
        }
        if (result->status >= 400) {
            throw lemon_cli::LemondHttpError(result->status,
                                             server_error_message(result->body, result->status));
        }
        return result->body;
    };
}

AgentCommand apply_execution_policy(AgentCommand command) {
    return command;
}

std::string find_external_pi(const std::string& search_path) {
    for (const fs::path& match :
         path_matches("pi", search_path,
                      lemon::utils::get_environment_variable_utf8("PATHEXT"))) {
        if (!is_lemonade_shim(match)) {
            return lemon::utils::path_to_utf8(match);
        }
    }
    lemon_tray::AgentConfig config;
    std::string error;
    lemon_tray::build_agent_config("pi", "127.0.0.1", 0, "", "", {}, config, error);
    for (const auto& fallback : config.fallback_paths) {
        const fs::path candidate = lemon::utils::path_from_utf8(expand_home(fallback));
        std::error_code ec;
        // ~/.local/bin/pi is both an npm fallback and our shim's path.
        if (fs::is_regular_file(candidate, ec) && !is_lemonade_shim(candidate)) {
            return lemon::utils::path_to_utf8(candidate);
        }
    }
    return "";
}

std::string requested_agent_dir(const json& body) {
    const std::string dir = body.contains("agent_dir") && body["agent_dir"].is_string()
                                ? body["agent_dir"].get<std::string>()
                                : "";
    if (!dir.empty() && !lemon::utils::path_from_utf8(dir).is_absolute()) {
        throw AgentError(400, "invalid_argument", "`agent_dir` must be an absolute path.");
    }
    return dir;
}

json pi_status(HostContext& ctx) {
    const auto& pi = find_agent("pi");
    json entry = {{"id", pi.id}, {"name", pi.name}, {"sandbox", "off"}};
    try {
        entry["pinned"] = resolve_pin(pi, load_agent_versions(), host_os(), host_arch()).version;
    } catch (const AgentError& e) {
        entry["pinned"] = nullptr;
        entry["pin_error"] = e.what();
    }

    const auto installed = find_installed(pi, ctx.paths);
    entry["installed"] = installed ? json{{"version", installed->version},
                                          {"binary", lemon::utils::path_to_utf8(installed->binary)}}
                                   : json(nullptr);
    const std::string path_value = lemon::utils::get_environment_variable_utf8("PATH");
    const std::string external = find_external_pi(path_value);
    entry["external"] = external.empty() ? json(nullptr) : json{{"binary", external}};
    entry["active"] = installed ? "managed" : (external.empty() ? "none" : "external");

    const fs::path shim = shim_path(ctx.paths, pi.binary);
    const auto on_path = path_matches(shim.filename().string(), path_value);
    bool shim_dir_on_path = false;
    for (const auto& match : on_path) {
        shim_dir_on_path = shim_dir_on_path || match.parent_path() == shim.parent_path();
    }
    entry["shim"] = {{"path", lemon::utils::path_to_utf8(shim)},
                     {"state", shim_state_name(shim_state(shim))},
                     {"on_path", shim_dir_on_path},
                     {"resolves_to", on_path.empty()
                                         ? json(nullptr)
                                         : json(lemon::utils::path_to_utf8(on_path.front()))}};
    return entry;
}

std::vector<std::string> pi_configure(HostContext& ctx, std::optional<bool> mcp,
                                      const std::string& agent_dir) {
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    lemon_cli::PiPrepRequest request;
    request.server_origin = ctx.lemond_origin;
    request.has_api_key = ctx.auth_required.load();
    request.mcp = mcp;
    request.agent_dir = agent_dir;
    const EventSink collect = [&warnings](const std::string& event, const json& data) {
        if (event == "warning") {
            warnings.push_back(data.value("message", ""));
        }
    };
    if (!lemon_cli::prepare_pi_config(make_lemond_call(ctx.lemond_origin, ctx.api_key), request,
                                      report_to(collect, errors), nullptr, &ctx.write_mutex)) {
        throw AgentError(500, "config_write_failed", join(errors));
    }
    return warnings;
}

json pi_launch(HostContext& ctx, const json& body, const EventSink& events,
               const std::function<bool()>& installing) {
    const auto& pi = find_agent("pi");
    const std::string cwd = body.value("cwd", "");
    std::error_code ec;
    if (cwd.empty() || !lemon::utils::path_from_utf8(cwd).is_absolute() ||
        !fs::is_directory(lemon::utils::path_from_utf8(cwd), ec)) {
        throw AgentError(400, "invalid_cwd", "`cwd` must be an absolute directory.");
    }
    const std::string model = body.value("model", "");
    if (model.empty()) {
        throw AgentError(400, "invalid_argument", "`model` must name a model.");
    }
    const bool codemode = body.value("codemode", false);
    std::optional<bool> mcp;
    if (body.contains("mcp") && body["mcp"].is_boolean()) {
        mcp = body["mcp"].get<bool>();
    }

    std::string pinned;
    try {
        pinned = resolve_pin(pi, load_agent_versions(), host_os(), host_arch()).version;
    } catch (const AgentError&) {
    }

    AgentCommand command;
    command.cwd = cwd;
    bool managed = false;
    if (const auto installed = find_installed(pi, ctx.paths)) {
        command.binary = lemon::utils::path_to_utf8(installed->binary);
        managed = true;
    } else {
        const std::string search_path =
            body.contains("search_path") && body["search_path"].is_string()
                ? body["search_path"].get<std::string>()
                : lemon::utils::get_environment_variable_utf8("PATH");
        command.binary = find_external_pi(search_path);
    }
    if (command.binary.empty()) {
        if (installing()) {
            throw AgentError(409, "agent_busy", "pi is being installed; try again when it's done.");
        }
        throw AgentError(409, "agent_not_installed", "pi isn't installed. " + pi_install_hint(),
                         {{"version", pinned}});
    }

    std::vector<std::string> errors;
    const auto report = report_to(events, errors);
    if (!managed && !lemon_cli::check_pi_version_for_flags(command.binary, codemode,
                                                           mcp.value_or(false), pi_install_hint(),
                                                           report)) {
        throw AgentError(409, "agent_outdated", join(errors), {{"version", pinned}});
    }

    lemon_cli::PiPrepRequest request;
    request.model = model;
    request.server_origin = ctx.lemond_origin;
    request.has_api_key = body.value("client_has_api_key", false);
    request.mcp = mcp;
    if (body.contains("recipe_options") && body["recipe_options"].is_object()) {
        request.recipe_options = body["recipe_options"];
    }
    request.agent_dir = requested_agent_dir(body);
    if (request.agent_dir.empty()) {
        request.agent_dir = lemon_cli::resolve_pi_agent_dir();
    }
    bool model_loaded = false;
    if (!lemon_cli::prepare_pi_config(make_lemond_call(ctx.lemond_origin, ctx.api_key), request,
                                      report, &model_loaded, &ctx.write_mutex)) {
        const std::string message = join(errors);
        throw message.rfind("Failed to load", 0) == 0
                  ? AgentError(424, "model_load_failed", message)
                  : AgentError(500, "config_write_failed", message);
    }

    command.args = {"--provider", "Lemonade", "--model", model};
    if (codemode) {
        command.args.push_back("--tools");
        command.args.push_back("+codemode");
    }
    if (body.contains("args") && body["args"].is_array()) {
        for (const auto& arg : body["args"]) {
            if (arg.is_string()) {
                command.args.push_back(arg.get<std::string>());
            }
        }
    }
    // pi reads exactly the config prepared above, whatever the client's environment.
    command.env["PI_CODING_AGENT_DIR"] = request.agent_dir;
    if (managed) {
        command.env["PI_SKIP_VERSION_CHECK"] = "1";
        command.env["PI_SELF_UPDATE_COMMAND"] = "lemonade agents install pi";
    }

    command = apply_execution_policy(std::move(command));
    return {{"binary", command.binary}, {"args", command.args}, {"cwd", command.cwd},
            {"env", command.env}, {"model_loaded", model_loaded}, {"managed", managed}};
}

} // namespace lemon::agents
