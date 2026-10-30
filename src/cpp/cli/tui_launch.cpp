#include "lemon_cli/tui_launch.h"

#include "lemon_cli/agent_launcher.h"
#include "lemon_cli/recipe_import.h"
#include "lemon_cli/tui_launch_model.h"
#include "lemon_cli/tui_launch_worker.h"

#include <lemon/canonical_id.h>
#include <lemon/recipe_options.h>
#include <lemon/utils/url_utils.h>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <tuple>

namespace lemon_cli {
namespace {

using namespace ftxui;
using nlohmann::json;

struct AgentEntry {
    std::string name;
    std::string binary;
    std::string instructions;
};

struct PresetPreview {
    json payload;
    LaunchModel model;
    std::string error;
};

struct LaunchDecision {
    LaunchModel model;
    json options;
    json preset_payload;
    std::string preset_file;
};

struct ModelResult {
    std::vector<LaunchModel> models;
    std::string resolved_id;
    std::string error;
};

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string shortened(const std::string& value, size_t width) {
    if (value.size() <= width) return value;
    if (width < 8) return value.substr(0, width);
    const size_t head = (width - 3) / 2;
    return value.substr(0, head) + "..." + value.substr(value.size() - (width - 3 - head));
}

Element box(const std::string& title, Element content) {
    return vbox({text(title) | bold, separator(), std::move(content)}) | border;
}

Element detail(const std::string& label, const std::string& value, int width = 64) {
    Elements lines;
    std::string line;
    int columns = 0;
    for (const auto& glyph : Utf8ToGlyphs(label + value)) {
        const int glyph_width = string_width(glyph);
        if (columns + glyph_width > width && !line.empty()) {
            lines.push_back(text(line));
            line.clear();
            columns = 0;
        }
        line += glyph;
        columns += glyph_width;
    }
    lines.push_back(text(line));
    return vbox(std::move(lines));
}

std::string size_label(const LaunchModel& model) {
    if (!model.size_gb) return "unknown";
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << *model.size_gb << " GB";
    return out.str();
}

std::string option_label(const json& options, const std::string& key) {
    if (key.empty() || !options.contains(key)) return "model default";
    if (options[key] == -1 || options[key] == "auto") return "auto";
    return options[key].is_string() ? options[key].get<std::string>() : options[key].dump();
}

std::string backend_key(const std::string& recipe) {
    for (const auto& key : lemon::RecipeOptions::keys_for_recipe(recipe)) {
        if (key.size() >= 8 && key.substr(key.size() - 8) == "_backend") return key;
    }
    return {};
}

// The cleared margin keeps dialog borders from automerging with panels underneath.
Element dialog(Element content) {
    return vbox({text(""), hbox({text(" "), std::move(content), text(" ")}), text("")}) | clear_under | center;
}

ButtonOption action_style(bool primary = false) {
    ButtonOption option;
    option.transform = [primary](const EntryState& state) {
        auto element = text("[ " + state.label + " ]");
        if (primary) element = element | bold | color(Color::Green);
        return state.focused ? element | inverted : element;
    };
    return option;
}

bool uses_context(const std::string& recipe) {
    const auto keys = lemon::RecipeOptions::keys_for_recipe(recipe);
    return std::find(keys.begin(), keys.end(), "ctx_size") != keys.end();
}

} // namespace

bool launch_tui(lemonade::LemonadeClient& client, LaunchTuiState& state) {
    std::vector<AgentEntry> agents;
    std::vector<std::string> agent_entries;
    for (const auto& name : state.agents) {
        lemon_tray::AgentConfig config;
        lemon_tray::AgentLaunchOptions options;
        std::string error;
        lemon_tray::build_agent_config(name, state.host, state.port, state.model, "", options, config, error);
        AgentEntry entry{name, lemon_tray::find_agent_binary(config), config.install_instructions};
        agent_entries.push_back(name);
        agents.push_back(std::move(entry));
    }
    if (agents.empty()) return false;
    int agent_selected = 0;
    for (size_t i = 0; i < agents.size(); ++i) {
        if (agents[i].name == state.agent) agent_selected = static_cast<int>(i);
    }

    std::vector<LaunchModel> models;
    std::vector<std::string> presets;
    std::map<std::string, PresetPreview> previews;
    std::set<std::string> pending_previews;
    std::vector<int> filtered;
    std::vector<std::string> entries{"Loading models..."};
    std::map<std::string, int> entry_models;
    std::vector<std::string> tabs{"Downloaded", "Recommended", "Presets", "All"};
    int tab = state.model.empty() ? 0 : 3;
    if (!state.recipe_file.empty() && state.model.empty()) tab = 2;
    int selected = 0;
    std::string query;
    std::string selected_id = state.model.empty() ? state.recipe_file : state.model;
    const std::string preset_dir = state.recipe_dir.empty() ? "coding-agents" : state.recipe_dir;
    bool models_loading = true;
    bool presets_loading = false;
    bool presets_requested = false;
    bool initial_tab = state.model.empty() && state.recipe_file.empty();
    bool model_choice_required = false;
    std::string model_error;
    std::string preset_error;
    std::string notice;
    json system_info = json::object();
    bool narrow = Terminal::Size().dimx < 110;
    bool accepted = false;
    bool show_advanced = false;
    bool show_confirm = false;
    std::optional<LaunchDecision> decision;
    bool show_config = false;
    std::string context = option_label(state.recipe_options, "ctx_size");
    if (!state.recipe_options.contains("ctx_size")) context = "inherit";
    std::vector<std::string> backend_entries{"Inherit", "Auto (server default)"};
    int backend_selected = 0;
    std::map<std::string, std::string> backend_choices;
    std::string active_backend_key;
    std::string primary_action = "Launch";
    std::string agent_args = state.agent_args;
    std::string codex_provider = state.codex_model_provider;
    bool codex_user_config = state.codex_use_user_config;
    std::vector<std::string> mcp_entries{"Keep existing choice", "Enable", "Disable"};
    int mcp_selected = state.pi_mcp ? (*state.pi_mcp ? 1 : 2) : 0;
    bool codemode = state.pi_codemode;
    auto advanced_original = std::make_tuple(agent_args, codex_provider, codex_user_config, mcp_selected, codemode);
    std::vector<std::string> config_lines;
    int config_selected = 0;
    const auto cwd = std::filesystem::current_path().string();
    const auto server = lemon_tray::build_agent_server_base_url(state.host, state.port, state.is_ssl);
    auto screen = ScreenInteractive::Fullscreen();
    auto sidebar_width = [&] { return std::clamp(screen.dimx() * 36 / 100, 42, 58); };
    LaunchWorker worker;

    std::function<void()> refresh;
    std::function<void()> request_preview;
    std::function<void()> request_presets;
    std::function<void()> request_models;

    auto selected_file = [&]() -> std::string {
        if (tab != 2 || filtered.empty() || selected < 0 || selected >= static_cast<int>(filtered.size())) return {};
        return presets[filtered[selected]];
    };
    auto selected_model = [&]() -> const LaunchModel* {
        if (filtered.empty() || selected < 0 || selected >= static_cast<int>(filtered.size())) return nullptr;
        if (tab != 2) return &models[filtered[selected]];
        const auto it = previews.find(selected_file());
        return it == previews.end() || !it->second.error.empty() ? nullptr : &it->second.model;
    };
    auto update_backend = [&] {
        const auto* model = selected_model();
        const auto key = model ? backend_key(model->recipe) : std::string();
        if (!active_backend_key.empty()) {
            backend_choices[active_backend_key] = backend_selected == 0 ? "inherit" :
                backend_selected == 1 ? "auto" : backend_entries[backend_selected];
        }
        std::string previous;
        if (backend_choices.count(key)) previous = backend_choices[key];
        else if (!key.empty() && state.recipe_options.contains(key) && state.recipe_options[key].is_string()) {
            previous = state.recipe_options[key].get<std::string>();
        }
        active_backend_key = key;
        backend_entries = {"Inherit", "Auto (server default)"};
        if (model && system_info.contains("recipes") && system_info["recipes"].contains(model->recipe)) {
            const auto& recipe = system_info["recipes"][model->recipe];
            if (recipe.contains("backends") && recipe["backends"].is_object()) {
                for (const auto& [name, info] : recipe["backends"].items()) {
                    if (name != "auto" && info.is_object() && info.value("state", "unsupported") != "unsupported") {
                        backend_entries.push_back(name);
                    }
                }
            }
        }
        backend_selected = previous == "auto" ? 1 : 0;
        if (!previous.empty() && previous != "auto" && previous != "inherit") {
            auto it = std::find(backend_entries.begin(), backend_entries.end(), previous);
            if (it == backend_entries.end()) {
                backend_entries.push_back(previous);
                backend_selected = static_cast<int>(backend_entries.size()) - 1;
            } else backend_selected = static_cast<int>(it - backend_entries.begin());
        }
    };
    request_preview = [&] {
        const auto file = selected_file();
        if (file.empty() || previews.count(file) || pending_previews.count(file)) return;
        pending_previews.insert(file);
        auto result = std::make_shared<PresetPreview>();
        worker.submit([dir = preset_dir, file, result] {
            if (fetch_remote_recipe_json(launch_preset_url(dir, file), result->payload, result->error)) {
                try { result->model = parse_launch_preset(result->payload); }
                catch (const std::exception& e) { result->error = e.what(); }
            }
        }, [&, file, result] {
            pending_previews.erase(file);
            previews[file] = std::move(*result);
            if (previews[file].error.empty()) previews[file].model = reconcile_launch_preset(previews[file].model, models);
            update_backend();
        }, true);
    };
    refresh = [&] {
        if (tab == 2) {
            filtered.clear();
            for (size_t i = 0; i < presets.size(); ++i) {
                if (lower(presets[i]).find(lower(query)) != std::string::npos) filtered.push_back(static_cast<int>(i));
            }
        } else {
            filtered = filter_launch_models(models, static_cast<LaunchTab>(tab), query, agents[agent_selected].name);
        }
        entries.clear();
        entry_models.clear();
        for (int index : filtered) {
            entries.push_back(tab == 2 ? presets[index] : models[index].id);
            if (tab != 2) entry_models[models[index].id] = index;
        }
        if (entries.empty()) {
            entries.push_back(tab == 2 ? (presets_loading ? "Loading presets..." : "No matching presets") :
                (models_loading ? "Loading models..." : "No matching models; try another tab"));
        }
        selected = 0;
        for (size_t i = 0; i < filtered.size(); ++i) {
            const auto id = tab == 2 ? presets[filtered[i]] : models[filtered[i]].id;
            if (id == selected_id) selected = static_cast<int>(i);
        }
        update_backend();
        if (tab == 2) request_preview();
    };
    request_presets = [&] {
        if (presets_loading) return;
        presets_requested = true;
        presets_loading = true;
        preset_error.clear();
        auto result = std::make_shared<std::pair<std::vector<std::string>, std::string>>();
        worker.submit([dir = preset_dir, result] {
            list_remote_recipe_files(dir, result->first, result->second);
        }, [&, result] {
            presets = std::move(result->first);
            preset_error = std::move(result->second);
            presets_loading = false;
            refresh();
        });
        refresh();
    };
    request_models = [&] {
        models_loading = true;
        model_error.clear();
        auto result = std::make_shared<ModelResult>();
        worker.submit([client, requested = state.model, result] {
            try {
                const auto listing = json::parse(client.make_request("/api/v1/models?show_all=true", "GET", "", "", 2000, 3000));
                json health = json::object();
                try { health = json::parse(client.make_request("/api/v1/health", "GET", "", "", 1000, 1000)); }
                catch (const std::exception&) {}
                result->models = parse_launch_models(listing, health);
                if (!requested.empty()) {
                    if (std::any_of(result->models.begin(), result->models.end(), [&](const auto& model) { return model.id == requested; })) {
                        result->resolved_id = requested;
                    } else {
                        try {
                            const auto info = json::parse(client.make_request("/api/v1/models/" + lemon::utils::url_encode(requested), "GET", "", "", 1000, 1000));
                            const auto resolved = parse_launch_models({{"data", json::array({info})}}, health);
                            if (!resolved.empty()) {
                                result->resolved_id = resolved.front().id;
                                if (std::none_of(result->models.begin(), result->models.end(), [&](const auto& model) { return model.id == result->resolved_id; })) {
                                    result->models.push_back(resolved.front());
                                }
                            }
                        } catch (const std::exception&) {}
                    }
                }
            } catch (const lemonade::HttpError& e) {
                result->error = lemonade::extract_server_error_message(e);
            } catch (const std::exception& e) {
                result->error = e.what();
            }
        }, [&, result] {
            models = std::move(result->models);
            model_error = std::move(result->error);
            models_loading = false;
            for (auto& [file, preview] : previews) {
                if (preview.error.empty()) preview.model = reconcile_launch_preset(preview.model, models);
            }
            if (!state.model.empty() && selected_id == state.model && model_error.empty()) {
                const auto choices = filter_launch_models(models, LaunchTab::All, "", agents[agent_selected].name);
                model_choice_required = std::none_of(choices.begin(), choices.end(), [&](int index) { return models[index].id == state.model; });
                if (model_choice_required) notice = "Requested model is unavailable; choose a result explicitly before launching.";
            }
            if (!result->resolved_id.empty() && selected_id == state.model) {
                selected_id = result->resolved_id;
                const auto choices = filter_launch_models(models, LaunchTab::All, "", agents[agent_selected].name);
                model_choice_required = std::none_of(choices.begin(), choices.end(), [&](int index) { return models[index].id == selected_id; });
                if (!model_choice_required) notice.clear();
            }
            if (initial_tab) {
                if (filter_launch_models(models, LaunchTab::Downloaded, "", agents[agent_selected].name).empty() &&
                    model_error.empty()) tab = 2;
                initial_tab = false;
            }
            refresh();
            if (tab == 2 && !presets_requested) request_presets();
        });
    };

    MenuOption agent_option = MenuOption::Horizontal();
    agent_option.on_change = [&] { refresh(); };
    agent_option.entries_option.transform = [&](const EntryState& entry) {
        const bool installed = std::any_of(agents.begin(), agents.end(), [&](const AgentEntry& agent) {
            return agent.name == entry.label && !agent.binary.empty();
        });
        auto element = text(" " + entry.label + (installed ? " " : " (missing) "));
        if (!installed) element = element | dim;
        if (entry.active) element = element | bold | color(Color::Cyan);
        return entry.focused ? element | inverted : element;
    };
    auto agent_menu = Menu(&agent_entries, &agent_selected, agent_option);
    InputOption search_option;
    search_option.on_change = refresh;
    auto search = Input(&query, "Search models or labels", search_option);
    MenuOption tab_option = MenuOption::Horizontal();
    tab_option.elements_infix = [] { return text(" | ") | dim; };
    tab_option.on_change = [&] {
        initial_tab = false;
        refresh();
        if (tab == 2 && !presets_requested) request_presets();
    };
    auto tab_menu = Menu(&tabs, &tab, tab_option);
    MenuOption model_option;
    model_option.on_change = [&] {
        model_choice_required = false;
        notice.clear();
        selected_id = tab == 2 ? selected_file() : (selected_model() ? selected_model()->id : "");
        update_backend();
        request_preview();
    };
    model_option.entries_option.transform = [&](const EntryState& entry) {
        const auto row_model = entry_models.find(entry.label);
        const LaunchModel* model = row_model == entry_models.end() ? nullptr : &models[row_model->second];
        const int width = narrow ? screen.dimx() - 4 : screen.dimx() - sidebar_width() - 6;
        const int name_width = std::max(12, width - (model ? 34 : 2));
        auto name = text(std::string(entry.active ? "> " : "  ") +
                         shortened(entry.label, static_cast<size_t>(name_width)));
        if (entry.active) name = name | bold;
        Element row = name | xflex;
        if (model) {
            const bool loaded = model->runtime == "loaded" || model->runtime == "ready";
            const std::string state = !model->downloaded ? "download" : model->runtime;
            row = hbox({name | xflex,
                        text(model->recipe) | dim | size(WIDTH, EQUAL, 10),
                        text(model->size_gb ? size_label(*model) : "") | dim | size(WIDTH, EQUAL, 11),
                        text(state) | size(WIDTH, EQUAL, 10) |
                            color(!model->downloaded ? Color(Color::Yellow) : loaded ? Color(Color::Green) : Color(Color::Default))});
        }
        return entry.focused ? row | inverted : row;
    };
    auto model_menu = Menu(&entries, &selected, model_option);
    auto context_input = Input(&context, "inherit, auto, or tokens");
    bool backend_open = false;
    std::string backend_title;
    Component backend_toggle;
    Component backend_menu;
    CheckboxOption backend_toggle_option;
    backend_toggle_option.transform = [](const EntryState& entry) {
        auto element = text(std::string(entry.state ? "v " : "> ") + entry.label);
        return entry.focused ? element | inverted : element;
    };
    backend_toggle = Checkbox(&backend_title, &backend_open, backend_toggle_option);
    auto backend_radio = Radiobox(&backend_entries, &backend_selected);
    backend_menu = CatchEvent(backend_radio, [&](Event event) {
        if (event != Event::Return && event != Event::Character(' ')) return false;
        backend_radio->OnEvent(event);
        backend_open = false;
        backend_toggle->TakeFocus();
        return true;
    });
    auto backend_chooser = Container::Vertical({backend_toggle, Maybe(backend_menu, &backend_open)});
    auto backend_renderer = Renderer(backend_chooser, [&] {
        backend_title = backend_entries[backend_selected];
        return backend_chooser->Render();
    });
    auto backend_dropdown = CatchEvent(backend_renderer, [&](Event event) {
        if (backend_open || (event != Event::Return && event != Event::Character(' '))) return false;
        backend_toggle->OnEvent(event);
        if (backend_open) backend_menu->TakeFocus();
        return true;
    });
    auto args_input = Input(&agent_args, "Additional agent arguments");
    auto provider_input = Input(&codex_provider, "Provider name");
    auto provider_checkbox = Checkbox("Use an existing Codex provider", &codex_user_config);
    auto mcp_dropdown = Dropdown(&mcp_entries, &mcp_selected);
    auto codemode_checkbox = Checkbox("Enable codemode for this launch", &codemode);
    auto is_codex = [&] { return agents[agent_selected].name == "codex"; };
    auto is_pi = [&] { return agents[agent_selected].name == "pi"; };
    auto has_context = [&] { const auto* model = selected_model(); return model && uses_context(model->recipe); };
    auto has_backend = [&] { return !active_backend_key.empty(); };

    auto validate = [&](json& options) {
        notice.clear();
        options = state.recipe_options.is_object() ? state.recipe_options : json::object();
        if (has_context() && !apply_launch_context(context, options, notice)) return false;
        if (has_backend()) {
            options.erase(active_backend_key);
            if (backend_selected == 1) options[active_backend_key] = nullptr;
            else if (backend_selected >= 2) options[active_backend_key] = backend_entries[backend_selected];
        }
        if (is_codex() && codex_user_config && codex_provider.empty()) {
            notice = "Enter the name of the existing Codex provider.";
            return false;
        }
        return true;
    };
    auto finish = [&] {
        if (!decision || agents[agent_selected].binary.empty()) return;
        state.agent = agents[agent_selected].name;
        state.model = decision->model.id;
        state.recipe_options = decision->options;
        state.agent_args = agent_args;
        state.codex_model_provider = codex_provider;
        state.codex_use_user_config = is_codex() && codex_user_config;
        if (is_pi()) {
            state.pi_mcp = mcp_selected == 0 ? std::nullopt : std::optional<bool>(mcp_selected == 1);
            state.pi_codemode = codemode;
        }
        state.recipe_dir = decision->preset_file.empty() ? "" : preset_dir;
        state.recipe_file = decision->preset_file;
        state.preset_payload = decision->preset_payload;
        state.download_required = !decision->model.downloaded;
        accepted = true;
        screen.Exit();
    };
    auto request_launch = [&] {
        const auto* model = selected_model();
        json options;
        if (!model) { notice = "Choose a model with a complete preview first."; return; }
        if (model_choice_required) { notice = "Requested model is unavailable; choose a result explicitly before launching."; return; }
        if (agents[agent_selected].binary.empty()) { notice = "Install the selected agent before launching."; return; }
        if (!validate(options)) return;
        if (filter_launch_models({*model}, LaunchTab::All, "", agents[agent_selected].name).empty()) {
            notice = "This preset does not define a supported chat model.";
            return;
        }
        decision = LaunchDecision{*model, std::move(options),
            tab == 2 ? previews.at(selected_file()).payload : json(), selected_file()};
        if (tab == 2 || !model->downloaded || !launch_model_warning(*model, agents[agent_selected].name).empty()) {
            show_confirm = true;
        } else finish();
    };
    auto launch_button = Button(&primary_action, request_launch, action_style(true));
    auto cancel_button = Button("Cancel", [&] { screen.Exit(); }, action_style());
    auto retry_button = Button("Retry", [&] {
        if (tab == 2) {
            const auto file = selected_file();
            if (!file.empty()) { previews.erase(file); request_preview(); }
            else request_presets();
        } else if (!models_loading) request_models();
    }, action_style());
    auto cancel_advanced = [&] {
        std::tie(agent_args, codex_provider, codex_user_config, mcp_selected, codemode) = advanced_original;
        notice.clear();
        show_advanced = false;
    };
    auto advanced_button = Button("Advanced...", [&] {
        advanced_original = std::make_tuple(agent_args, codex_provider, codex_user_config, mcp_selected, codemode);
        show_advanced = true;
        args_input->TakeFocus();
    }, action_style());
    auto config_button = Button("Preset JSON", [&] {
        const auto it = previews.find(selected_file());
        if (it == previews.end() || !it->second.error.empty()) return;
        config_lines.clear();
        std::istringstream input(it->second.payload.dump(2));
        std::string line;
        while (std::getline(input, line)) config_lines.push_back(line);
        config_selected = 0;
        show_config = true;
    }, action_style());
    auto context_control = Maybe(context_input, has_context);
    auto backend_control = Maybe(backend_dropdown, has_backend);
    auto config_control = Maybe(config_button, [&] { return tab == 2 && selected_model(); });
    auto retry_control = Maybe(retry_button, [&] {
        const auto it = previews.find(selected_file());
        return (tab == 2 && (!preset_error.empty() || (it != previews.end() && !it->second.error.empty()))) ||
               (tab != 2 && !model_error.empty());
    });
    auto launch_control = Maybe(launch_button, [&] { return !model_choice_required && selected_model() && !agents[agent_selected].binary.empty(); });
    auto model_control = Container::Vertical({tab_menu, search, model_menu});
    auto option_control = Container::Horizontal({context_control, backend_control, advanced_button});
    auto actions = Container::Horizontal({retry_control, config_control, launch_control, cancel_button});
    auto main = Container::Vertical({agent_menu, model_control, option_control, actions});

    auto advanced_apply = Button("Apply", [&] {
        json options;
        if (validate(options)) { show_advanced = false; advanced_button->TakeFocus(); }
    }, action_style(true));
    auto advanced_cancel = Button("Cancel", [&] { cancel_advanced(); advanced_button->TakeFocus(); }, action_style());
    auto advanced = Container::Vertical({args_input, Maybe(provider_checkbox, is_codex),
        Maybe(provider_input, [&] { return is_codex() && codex_user_config; }),
        Maybe(mcp_dropdown, is_pi), Maybe(codemode_checkbox, is_pi),
        Container::Horizontal({advanced_apply, advanced_cancel})});
    auto advanced_renderer = Renderer(advanced, [&] {
        Elements body{text("Agent: " + agents[agent_selected].name) | bold,
                      hbox({text("Agent args: "), args_input->Render()}), separator()};
        if (is_codex()) {
            body.push_back(provider_checkbox->Render());
            if (codex_user_config) body.push_back(hbox({text("Provider: "), provider_input->Render()}));
        }
        if (is_pi()) {
            body.push_back(hbox({text("MCP tools: "), mcp_dropdown->Render()}));
            body.push_back(codemode_checkbox->Render());
            body.push_back(paragraph("MCP changes are kept in the client's Pi configuration."));
        }
        body.push_back(paragraph("Model overrides are for this launch only; saved model settings are unchanged.") | dim);
        if (!notice.empty()) body.push_back(paragraph(notice) | color(Color::Red));
        body.push_back(hbox({advanced_apply->Render(), text(" "), advanced_cancel->Render()}));
        return box("Advanced launch options", vbox(std::move(body))) | size(WIDTH, LESS_THAN, 70) | dialog;
    });
    auto confirm_button = Button("Continue", finish, action_style(true));
    auto back_button = Button("Back", [&] { show_confirm = false; decision.reset(); launch_button->TakeFocus(); }, action_style());
    auto confirm = Container::Horizontal({back_button, confirm_button});
    auto confirm_renderer = Renderer(confirm, [&] {
        const auto* model = decision ? &decision->model : nullptr;
        Elements body;
        if (model) {
            body = {detail("Agent: ", agents[agent_selected].name), detail("Model: ", model->id),
                    detail("Server: ", server), separator()};
            if (!decision->preset_file.empty()) body.push_back(paragraph("Register or update this model using the previewed preset."));
            if (!model->storage_known) body.push_back(paragraph("Check the server cache and download missing weights (estimate: " + size_label(*model) + ")."));
            else if (!model->downloaded) body.push_back(paragraph("Download model files onto the server (" + size_label(*model) + ")."));
            if (lemon_tray::agent_needs_config_sync(agents[agent_selected].name)) {
                body.push_back(paragraph("Update the agent's managed Lemonade profile on this machine."));
            }
            body.push_back(paragraph("Load the model and launch the agent in " + cwd + "."));
            const auto warning = launch_model_warning(*model, agents[agent_selected].name);
            if (!warning.empty()) body.push_back(paragraph(warning) | color(Color::Yellow));
        }
        body.push_back(separator());
        body.push_back(hbox({back_button->Render(), text(" "), confirm_button->Render()}));
        return box("Confirm launch", vbox(std::move(body))) | size(WIDTH, LESS_THAN, 70) | dialog;
    });
    MenuOption config_option;
    config_option.entries_option.transform = [&](const EntryState& entry) {
        auto element = detail("", entry.label, std::max(8, std::min(86, screen.dimx() - 4)));
        return entry.focused ? element | inverted : element;
    };
    auto config_menu = Menu(&config_lines, &config_selected, config_option);
    auto config_close = Button("Back", [&] { show_config = false; config_button->TakeFocus(); }, action_style());
    auto config_renderer = Renderer(Container::Vertical({config_menu, config_close}), [&] {
        return box("Preset configuration", vbox({config_menu->Render() | vscroll_indicator | frame | flex,
                                                  config_close->Render()})) | size(WIDTH, LESS_THAN, 90) |
               size(HEIGHT, LESS_THAN, std::max(8, screen.dimy() - 6)) | dialog;
    });

    auto renderer = Renderer(main, [&] {
        narrow = Terminal::Size().dimx < 110;
        const auto* model = selected_model();
        const auto& agent = agents[agent_selected];
        const int side = narrow ? screen.dimx() : sidebar_width();
        auto field = [&](const std::string& label, const std::string& value) {
            return hbox({text(label) | dim | size(WIDTH, EQUAL, 10), detail("", value, std::max(8, side - 14))});
        };
        auto note = [](const std::string& value, Color tone) { return paragraph(value) | color(tone); };

        Elements fields{field("Agent", agent.name + (agent.binary.empty() ? " (not installed)" : ""))};
        Elements notes;
        if (agent.binary.empty()) notes.push_back(note(agent.instructions, Color::Yellow));
        std::string storage;
        if (model) {
            storage = !model->storage_known ? "checked on import (" + size_label(*model) + ")" :
                model->downloaded ? "downloaded" : "download " + size_label(*model);
            fields.push_back(field("Model", model->id));
            fields.push_back(field("Recipe", model->recipe));
            fields.push_back(field("Storage", storage));
            fields.push_back(field("Runtime", model->runtime));
            if (tab == 2) {
                fields.push_back(field("Preset", preset_dir + "/" + selected_file()));
                const bool exists = std::any_of(models.begin(), models.end(), [&](const auto& existing) {
                    return existing.id == model->id || existing.id == lemon::strip_user_model_prefix(model->id);
                });
                notes.push_back(paragraph(exists ? "Existing name found; registration may replace its settings." : "Registers or updates this model on the server.") | dim);
            }
            if (lemon_tray::agent_needs_config_sync(agent.name)) notes.push_back(paragraph("Updates the agent's managed Lemonade profile.") | dim);
            const auto warning = launch_model_warning(*model, agent.name);
            if (!warning.empty()) notes.push_back(note(warning, Color::Yellow));
            if (agent.name == "pi" && context == "auto") notes.push_back(paragraph("Pi waits when context sizing is automatic.") | dim);
        } else if (tab == 2 && !selected_file().empty()) {
            const auto it = previews.find(selected_file());
            notes.push_back(it == previews.end() ? paragraph("Loading preset preview...") | dim : note(it->second.error, Color::Red));
        } else notes.push_back(paragraph("Choose a model or preset.") | dim);
        if (tab == 2 && !preset_error.empty()) notes.push_back(note(preset_error, Color::Red));
        if (tab != 2 && !model_error.empty()) notes.push_back(note(model_error, Color::Red));

        primary_action = tab == 2 ? "Import & launch" : (model && !model->downloaded ? "Download & launch" : "Launch");
        auto inherited = [&](const std::string& key) {
            return text(model && !narrow ? " (" + option_label(model->recipe_options, key) + ")" : "") | dim;
        };
        Elements option_rows;
        if (has_context()) {
            option_rows.push_back(hbox({text("Context: "), context_input->Render() | size(WIDTH, EQUAL, 10),
                                        context == "inherit" ? inherited("ctx_size") : text("")}));
        }
        if (has_backend()) {
            option_rows.push_back(hbox({text("Backend: "), backend_dropdown->Render(),
                                        backend_selected == 0 ? inherited(active_backend_key) : text("")}));
        }
        auto secondary = hbox({advanced_button->Render(), text(" "), config_control->Render(), text(" "), retry_control->Render()});
        auto status = notice.empty() ? text(primary_action + (model ? " " + agent.name : "")) | dim : paragraph(notice) | color(Color::Red);
        auto launch = launch_control->Focusable() ? launch_button->Render() : text("[ " + primary_action + " ]") | dim;
        auto action_row = hbox({filler(), launch, text(" "), cancel_button->Render()});

        auto panel = [&](const std::string& title, Element content, bool focused) {
            return window(text(" " + title + " ") | bold | (focused ? color(Color::Cyan) : nothing), std::move(content));
        };
        auto browser_content = vbox({
            hbox({tab_menu->Render(), filler(), text(std::to_string(filtered.size()) + " matches ") | dim}),
            hbox({text("/ ") | bold | color(Color::Cyan), search->Render() | flex}),
            separatorLight(),
            model_menu->Render() | vscroll_indicator | frame | flex});
        const bool browsing = tab_menu->Focused() || search->Focused() || model_menu->Focused();

        const Color status_color = models_loading ? Color::Yellow : model_error.empty() ? Color::Green : Color::Red;
        Elements layout{hbox({text(" Lemonade / launch ") | bold | inverted, text("  ● ") | color(status_color),
            text(server + (models_loading ? " connecting" : model_error.empty() ? " connected" : " unavailable")),
            filler(), text(narrow ? "" : shortened(cwd, static_cast<size_t>(std::max(8, screen.dimx() / 3))) + " ") | dim}),
            hbox({text(" Agent ") | bold | (agent_menu->Focused() ? color(Color::Cyan) : nothing), agent_menu->Render()})};
        if (narrow) {
            Elements compact;
            if (model) compact.push_back(field("Model", model->id + "  " + model->recipe + ", " + storage + ", " + model->runtime));
            for (auto& line : notes) compact.push_back(std::move(line));
            Elements controls;
            for (auto& row : option_rows) { controls.push_back(std::move(row)); controls.push_back(text("  ")); }
            controls.push_back(secondary);
            layout.push_back(panel("Models", browser_content, browsing) | flex);
            layout.push_back(vbox(std::move(compact)) | size(HEIGHT, LESS_THAN, std::max(3, screen.dimy() / 4)) | frame);
            layout.push_back(separatorLight());
            layout.push_back(hbox(std::move(controls)));
            layout.push_back(hbox({status | flex, action_row}));
        } else {
            Elements sidebar = std::move(fields);
            for (auto& line : notes) sidebar.push_back(std::move(line));
            sidebar.push_back(separatorLight());
            for (auto& row : option_rows) sidebar.push_back(std::move(row));
            sidebar.push_back(secondary);
            sidebar.push_back(filler());
            sidebar.push_back(separatorLight());
            sidebar.push_back(status);
            sidebar.push_back(action_row);
            const bool configuring = option_control->Focused() || actions->Focused();
            layout.push_back(hbox({panel("Models", browser_content, browsing) | flex,
                panel("Launch", vbox(std::move(sidebar)), configuring) | size(WIDTH, EQUAL, sidebar_width())}) | flex);
        }
        layout.push_back(text(" Tab focus  / search  ↑↓ select  Enter activate  F2 launch  Esc exit") | dim);
        return vbox(std::move(layout));
    });
    auto root = Modal(renderer, advanced_renderer, &show_advanced);
    root = Modal(root, confirm_renderer, &show_confirm);
    root = Modal(root, config_renderer, &show_config);
    root = CatchEvent(root, [&](Event event) {
        if (event == Event::Tab || event == Event::TabReverse) {
            std::vector<Component> order;
            if (show_config) order = {config_menu, config_close};
            else if (show_confirm) order = {back_button, confirm_button};
            else if (show_advanced) {
                order = {args_input};
                if (is_codex()) {
                    order.push_back(provider_checkbox);
                    if (codex_user_config) order.push_back(provider_input);
                }
                if (is_pi()) {
                    order.push_back(mcp_dropdown);
                    order.push_back(codemode_checkbox);
                }
                order.push_back(advanced_apply);
                order.push_back(advanced_cancel);
            } else {
                order = {agent_menu, tab_menu, search, model_menu};
                if (has_context()) order.push_back(context_input);
                if (has_backend()) order.push_back(backend_dropdown);
                order.push_back(advanced_button);
                if (retry_control->Focusable()) order.push_back(retry_button);
                if (config_control->Focusable()) order.push_back(config_button);
                if (launch_control->Focusable()) order.push_back(launch_button);
                order.push_back(cancel_button);
            }
            int current = 0;
            for (size_t i = 0; i < order.size(); ++i) {
                if (order[i]->Focused()) current = static_cast<int>(i);
            }
            const int count = static_cast<int>(order.size());
            order[(current + (event == Event::Tab ? 1 : count - 1)) % count]->TakeFocus();
            return true;
        }
        if (event == Event::Escape) {
            if (show_config) { show_config = false; config_button->TakeFocus(); }
            else if (show_confirm) { show_confirm = false; decision.reset(); launch_button->TakeFocus(); }
            else if (show_advanced) { cancel_advanced(); advanced_button->TakeFocus(); }
            else if (backend_open) { backend_open = false; backend_toggle->TakeFocus(); }
            else screen.Exit();
            return true;
        }
        if (event == Event::Special({3})) { screen.Exit(); return true; }
        if (show_advanced || show_confirm || show_config) return false;
        if (event == Event::F2) { request_launch(); return true; }
        const bool editing = search->Focused() || context_input->Focused() || args_input->Focused() || provider_input->Focused();
        if (event == Event::Character('/') && !editing) { search->TakeFocus(); return true; }
        if (event == Event::Return && search->Focused()) { model_menu->TakeFocus(); return true; }
        if (event == Event::Return && model_menu->Focused()) {
            if (const auto* model = selected_model()) {
                selected_id = tab == 2 ? selected_file() : model->id;
                model_choice_required = false;
                notice.clear();
            }
            if (launch_control->Focusable()) launch_button->TakeFocus();
            else request_launch();
            return true;
        }
        if (event == Event::Return && context_input->Focused()) {
            json options;
            if (validate(options)) advanced_button->TakeFocus();
            return true;
        }
        return false;
    });
    if (state.agent.empty()) agent_menu->TakeFocus();
    else model_menu->TakeFocus();
    Loop loop(&screen, root);
    request_models();
    auto info = std::make_shared<json>(json::object());
    worker.submit([client, info] {
        try { *info = json::parse(client.make_request("/api/v1/system-info", "GET", "", "", 1000, 2000)); }
        catch (const std::exception&) {}
    }, [&, info] { system_info = std::move(*info); update_backend(); });
    if (tab == 2) request_presets();
    while (!loop.HasQuitted()) {
        if (worker.drain()) screen.PostEvent(Event::Custom);
        loop.RunOnce();
        if (!loop.HasQuitted()) worker.wait_for_results(std::chrono::milliseconds(16));
    }
    worker.stop();
    return accepted;
}

} // namespace lemon_cli
