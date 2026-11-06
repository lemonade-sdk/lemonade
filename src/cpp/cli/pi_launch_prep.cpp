#include "lemon_cli/pi_launch_prep.h"

#include <algorithm>
#include <vector>

#include "lemon/utils/process_manager.h"
#include "lemon_cli/pi_profile.h"

namespace lemon_cli {

namespace {

constexpr int kDefaultContextWindow = 40960;

bool has_label(const AgentModelEntry& model, const std::string& label) {
    return std::find(model.labels.begin(), model.labels.end(), label) != model.labels.end();
}

} // namespace

bool prepare_pi_config(const LemondCall& call,
                       const PiPrepRequest& request,
                       const PrepReport& report,
                       bool* model_loaded,
                       std::mutex* write_mutex) {
    // A window of 0 marks a model without a saved size until the fallback is applied.
    std::vector<AgentModelEntry> models = fetch_llm_models_for_sync(call, 0);

    bool selected_unknown = false;
    if (request.model) {
        const std::string& selected_id = *request.model;
        const bool selected_present =
            std::any_of(models.begin(), models.end(),
                        [&](const AgentModelEntry& m) { return m.id == selected_id; });

        // The list holds only downloaded models; a selected model that is not
        // downloaded yet still needs its labels and saved window.
        if (!selected_present) {
            nlohmann::json selected = nlohmann::json::object();
            try {
                selected = nlohmann::json::parse(
                    call("GET", "/api/v1/models/" + selected_id, "", 3000));
            } catch (const LemondHttpError& e) {
                selected_unknown = e.status == 404;
            } catch (const std::exception&) {
                // Other failures: the model goes in without labels.
            }
            if (!selected.is_object()) {
                selected = nlohmann::json::object();
            }
            models.push_back(agent_model_entry_from_json(selected_id, selected, 0));
        }

        // Launch loads the selected model with its own ctx_size, which outranks the saved one.
        if (request.recipe_options.is_object() && request.recipe_options.contains("ctx_size")) {
            const int launch_ctx_size = request.recipe_options["ctx_size"].get<int>();
            for (auto& model : models) {
                if (model.id == selected_id) {
                    model.context_window = launch_ctx_size > 0 ? launch_ctx_size : 0;
                }
            }
        }

        // Pi compacts by contextWindow, so it waits for the load rather than
        // guessing a window the auto-tuned model may not have.
        if (!selected_unknown) {
            for (auto& model : models) {
                if (model.id != selected_id || model.context_window > 0) {
                    continue;
                }
                if (!load_for_context_window(call, selected_id, request.recipe_options, report,
                                             model.context_window)) {
                    report(PrepLevel::Error, "Not launching pi, since the model failed to load.");
                    return false;
                }
                if (model_loaded != nullptr) {
                    *model_loaded = true;
                }
                if (model.context_window <= 0) {
                    report(PrepLevel::Warning, "Launching pi anyway, using a context size of " +
                                                   std::to_string(kDefaultContextWindow) + ".");
                }
            }
        }
    }

    for (auto& model : models) {
        if (model.context_window <= 0) {
            model.context_window = kDefaultContextWindow;
        }
    }

    std::unique_lock<std::mutex> write_lock;
    if (write_mutex != nullptr) {
        write_lock = std::unique_lock<std::mutex>(*write_mutex);
    }

    // Pi's config references the key launch exports, so starting pi on a
    // config that still points at another server would send it this key.
    const bool with_key = request.has_api_key;
    const std::string key_stop = "Not launching pi, since it would send this server's API key "
                                 "to the server in pi's old config.";
    // pi_api_key_value only distinguishes an empty key from a set one.
    const std::string api_key = with_key ? "set" : "";

    std::string error_message;
    if (!sync_agent_config_file(pi_profile(request.agent_dir), "Lemonade",
                                request.server_origin + "/v1", api_key, models,
                                error_message)) {
        if (with_key) {
            report(PrepLevel::Error, "Failed to sync pi config: " + error_message + "\n" + key_stop);
            return false;
        }
        report(PrepLevel::Warning, "Failed to sync pi config: " + error_message +
                                       "\nContinuing with launch anyway...");
    }

    if (request.model) {
        const std::string& selected_id = *request.model;
        if (selected_unknown) {
            report(PrepLevel::Warning, selected_id +
                                           " is not a model on the Lemonade server; check the "
                                           "name with `lemonade list`.");
        }
        for (const auto& model : models) {
            if (model.id == selected_id && !model.labels.empty() &&
                !has_label(model, "tool-calling")) {
                report(PrepLevel::Warning,
                       selected_id + " is not labeled tool-calling; pi may fail to use its tools.");
            }
        }
    }

    // Pi may still load an mcp.json launch cannot parse (Node tolerates
    // invalid UTF-8), so its lemonade entry could not be checked or refreshed.
    std::string mcp_read_error;
    if (with_key && !pi_mcp_config_readable(mcp_read_error, request.agent_dir)) {
        report(PrepLevel::Error, "Can't read pi's mcp.json: " + mcp_read_error +
                                     "\nNot launching pi with an API key until it can be read.");
        return false;
    }

    // An existing entry is refreshed so it follows the current host, port and key.
    const bool had_mcp_entry = pi_has_mcp_server(request.agent_dir);
    if (request.mcp.value_or(had_mcp_entry)) {
        std::string mcp_error;
        if (!sync_pi_mcp_server(request.server_origin, with_key, mcp_error, request.agent_dir)) {
            if (with_key && had_mcp_entry) {
                report(PrepLevel::Error,
                       "Failed to update Lemonade MCP in pi: " + mcp_error + "\n" + key_stop);
                return false;
            }
            report(PrepLevel::Warning, "Failed to add Lemonade MCP to pi: " + mcp_error +
                                           "\nContinuing with launch anyway...");
        }
    } else if (request.mcp.has_value()) {
        std::string mcp_error;
        if (!remove_pi_mcp_server(mcp_error, request.agent_dir)) {
            report(PrepLevel::Error, "Failed to remove Lemonade MCP from pi: " + mcp_error +
                                         "\nNot launching pi, since --no-mcp was requested.");
            return false;
        }
    }

    // A default the user chose is kept; only a pi without one gets this model.
    if (request.model && !pi_has_default_config(request.agent_dir)) {
        std::string settings_error;
        if (!sync_pi_settings_file("Lemonade", *request.model, settings_error,
                                   request.agent_dir)) {
            report(PrepLevel::Warning, "Failed to sync pi settings: " + settings_error +
                                           "\nContinuing with launch anyway...");
        }
    }
    return true;
}

bool check_pi_version_for_flags(const std::string& pi_binary,
                                bool codemode,
                                bool mcp,
                                const std::string& install_instructions,
                                const PrepReport& report) {
    if (!codemode && !mcp) {
        return true;
    }

    std::string output;
    try {
        lemon::utils::ProcessManager::run_process_with_output(
            pi_binary, {"--version"},
            [&output](const std::string& line) {
                output += line + "\n";
                return true;
            },
            "", 10, false);
    } catch (const std::exception&) {
        return true;
    }
    int major = 0;
    int minor = 0;
    if (!parse_pi_version(output, major, minor)) {
        return true;
    }
    const std::string found = output.substr(0, output.find('\n'));

    if (codemode && (major < 1 || (major == 1 && minor < 1))) {
        report(PrepLevel::Error, "--codemode needs pi 1.1 or newer (found " + found +
                                     "). Reinstall pi to update it.\n" + install_instructions);
        return false;
    }
    if (mcp && major < 1) {
        report(PrepLevel::Warning,
               "--mcp needs pi 1.0 or newer (found " + found + "); this pi ignores mcp.json.");
    }
    return true;
}

} // namespace lemon_cli
