#include "lemon_cli/tui_launch.h"

#include "lemon_cli/recipe_import.h"

#include <lemon/model_types.h>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace lemon_cli {
namespace {

using ftxui::Button;
using ftxui::CatchEvent;
using ftxui::Checkbox;
using ftxui::Component;
using ftxui::Element;
using ftxui::Event;
using ftxui::Input;
using ftxui::Menu;
using ftxui::MenuOption;
using ftxui::Renderer;
using ftxui::ScreenInteractive;
using ftxui::Toggle;
using ftxui::border;
using ftxui::bold;
using ftxui::color;
using ftxui::dim;
using ftxui::flex;
using ftxui::frame;
using ftxui::hbox;
using ftxui::separator;
using ftxui::text;
using ftxui::vbox;
using ftxui::vscroll_indicator;

namespace Container = ftxui::Container;

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool contains_case_insensitive(const std::string& value, const std::string& needle) {
    return to_lower(value).find(to_lower(needle)) != std::string::npos;
}

bool has_label(const lemonade::ModelInfo& model, const std::string& label) {
    return std::find(model.labels.begin(), model.labels.end(), label) != model.labels.end();
}

bool is_launch_recipe(const std::string& recipe) {
    static const std::unordered_set<std::string> recipes = {"flm", "llamacpp", "vllm"};
    return recipes.count(recipe) > 0;
}

bool is_llm(const lemonade::ModelInfo& model) {
    return is_launch_recipe(model.recipe) &&
           lemon::get_model_type_from_labels(model.labels) == lemon::ModelType::LLM;
}

bool is_tool_calling_llm(const lemonade::ModelInfo& model) {
    return is_llm(model) && has_label(model, "tool-calling");
}

std::string labels_to_string(const std::vector<std::string>& labels) {
    std::ostringstream out;
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << labels[i];
    }
    return out.str();
}

std::string searchable_text(const lemonade::ModelInfo& model) {
    std::string text = model.id + " " + model.checkpoint + " " + model.recipe + " ";
    text += model.downloaded ? "downloaded local " : "missing not-downloaded ";
    for (const auto& label : model.labels) {
        text += label + " ";
    }
    return text;
}

std::vector<int> filter_models(const std::vector<lemonade::ModelInfo>& models,
                               const std::string& query,
                               int mode) {
    std::vector<int> filtered;
    for (size_t i = 0; i < models.size(); ++i) {
        const auto& model = models[i];
        if (!is_llm(model)) {
            continue;
        }
        if (mode == 0 && !(model.downloaded && is_tool_calling_llm(model))) {
            continue;
        }
        if (mode == 1 && !(has_label(model, "hot") && is_tool_calling_llm(model))) {
            continue;
        }
        if (!query.empty() && !contains_case_insensitive(searchable_text(model), query)) {
            continue;
        }
        filtered.push_back(static_cast<int>(i));
    }
    std::sort(filtered.begin(), filtered.end(), [&](int lhs_index, int rhs_index) {
        const auto& lhs = models[static_cast<size_t>(lhs_index)];
        const auto& rhs = models[static_cast<size_t>(rhs_index)];
        if (lhs.downloaded != rhs.downloaded) {
            return lhs.downloaded;
        }
        return lhs.id < rhs.id;
    });
    return filtered;
}

std::vector<std::string> model_entries(const std::vector<lemonade::ModelInfo>& models,
                                       const std::vector<int>& filtered) {
    std::vector<std::string> entries;
    for (int index : filtered) {
        const auto& model = models[static_cast<size_t>(index)];
        entries.push_back(model.id + "  [" + (model.downloaded ? "downloaded" : "missing") +
                          "]  " + (model.recipe.empty() ? "-" : model.recipe));
    }
    if (entries.empty()) {
        entries.push_back("No launch-compatible models match the current filter");
    }
    return entries;
}

std::vector<int> filter_recipe_files(const std::vector<std::string>& recipes,
                                     const std::string& query) {
    std::vector<int> filtered;
    for (size_t i = 0; i < recipes.size(); ++i) {
        if (query.empty() || contains_case_insensitive(recipes[i], query)) {
            filtered.push_back(static_cast<int>(i));
        }
    }
    return filtered;
}

std::vector<std::string> recipe_entries(const std::vector<std::string>& recipes,
                                        const std::vector<int>& filtered,
                                        const std::string& error) {
    std::vector<std::string> entries;
    for (int index : filtered) {
        entries.push_back(recipes[static_cast<size_t>(index)]);
    }
    if (entries.empty()) {
        entries.push_back(error.empty() ? "No recipes match the current filter" : error);
    }
    return entries;
}

Element section_box(const std::string& title, Element content, bool focused) {
    auto header = text((focused ? "> " : "  ") + title) | bold;
    Element body = vbox({header, separator(), content});
    if (focused) {
        body = body | color(ftxui::Color::Cyan);
    }
    return body | border;
}

}  // namespace

bool launch_tui(lemonade::LemonadeClient& client, LaunchTuiState& state) {
    std::vector<lemonade::ModelInfo> models = client.get_models(true);
    if (models.empty()) {
        return false;
    }

    std::vector<std::string> agents = {"claude", "codex", "opencode", "pi"};
    int agent_selected = 0;
    auto agent_it = std::find(agents.begin(), agents.end(), state.agent);
    if (agent_it != agents.end()) {
        agent_selected = static_cast<int>(std::distance(agents.begin(), agent_it));
    }

    std::vector<std::string> modes = {"Recipes", "Downloaded", "Recommended", "All"};
    int mode = state.model.empty() ? 0 : 3;
    int focus = 0;
    bool accepted = false;
    std::string search = state.model;
    int selected_model = 0;
    std::string ctx_size;
    std::string llamacpp_backend;
    std::string agent_args = state.agent_args;
    std::string codex_provider = state.codex_model_provider.empty()
        ? "lemonade"
        : state.codex_model_provider;
    bool codex_use_user_config = state.codex_use_user_config;

    std::vector<std::string> recipes;
    std::string recipe_error;
    if (!list_remote_recipe_files("coding-agents", recipes, recipe_error) && recipe_error.empty()) {
        recipe_error = "Failed to load coding-agent recipes";
    }

    std::vector<int> filtered = filter_models(models, search, 0);
    std::vector<int> filtered_recipes = filter_recipe_files(recipes, search);
    std::vector<std::string> entries = recipe_entries(recipes, filtered_recipes, recipe_error);

    auto refresh_filter = [&] {
        if (mode == 0) {
            filtered_recipes = filter_recipe_files(recipes, search);
            entries = recipe_entries(recipes, filtered_recipes, recipe_error);
        } else {
            filtered = filter_models(models, search, mode - 1);
            entries = model_entries(models, filtered);
        }
        if (selected_model >= static_cast<int>(entries.size())) {
            selected_model = static_cast<int>(entries.size()) - 1;
        }
        if (selected_model < 0) {
            selected_model = 0;
        }
    };

    auto selected_info = [&]() -> const lemonade::ModelInfo* {
        if (mode == 0) {
            return nullptr;
        }
        if (filtered.empty() || selected_model >= static_cast<int>(filtered.size())) {
            return nullptr;
        }
        return &models[static_cast<size_t>(filtered[static_cast<size_t>(selected_model)])];
    };

    auto agent_menu = Menu(&agents, &agent_selected, MenuOption::Vertical());
    auto search_input = Input(&search, "Search launch models");
    auto mode_toggle = Toggle(&modes, &mode);
    auto model_menu = Menu(&entries, &selected_model, MenuOption::Vertical());
    auto ctx_input = Input(&ctx_size, "auto");
    auto backend_input = Input(&llamacpp_backend, "auto");
    auto agent_args_input = Input(&agent_args, "extra agent args");
    auto codex_provider_input = Input(&codex_provider, "lemonade");
    auto codex_config_checkbox = Checkbox("Use existing Codex provider config", &codex_use_user_config);

    ScreenInteractive* screen_ptr = nullptr;
    auto launch_button = Button("Launch", [&] {
        if ((mode == 0 && !filtered_recipes.empty() &&
             selected_model < static_cast<int>(filtered_recipes.size())) ||
            selected_info() != nullptr) {
            accepted = true;
            screen_ptr->ExitLoopClosure()();
        }
    });
    auto quit_button = Button("Quit", [&] {
        accepted = false;
        screen_ptr->ExitLoopClosure()();
    });

    auto agent_section = Container::Vertical({agent_menu});
    auto model_section = Container::Vertical({search_input, mode_toggle, model_menu});
    auto options_section = Container::Vertical({ctx_input, backend_input, agent_args_input,
                                                codex_provider_input, codex_config_checkbox});
    auto actions_section = Container::Horizontal({launch_button, quit_button});
    std::vector<Component> focusable = {
        agent_section,
        model_section,
        options_section,
        actions_section,
    };
    auto layout = Container::Vertical({agent_section, model_section, options_section, actions_section});

    auto renderer = Renderer(layout, [&] {
        refresh_filter();
        const auto* model = selected_info();
        Element details = text("No model selected") | dim;
        if (mode == 0) {
            if (!filtered_recipes.empty() && selected_model < static_cast<int>(filtered_recipes.size())) {
                details = vbox({
                    hbox({text("Recipe: ") | bold, text(recipes[static_cast<size_t>(
                        filtered_recipes[static_cast<size_t>(selected_model)])])}),
                    hbox({text("Directory: ") | bold, text("coding-agents")}),
                    text("Importing registers the model, then launches the selected agent") | dim,
                });
            } else {
                details = text(recipe_error.empty() ? "No recipe selected" : recipe_error) | dim;
            }
        } else if (model != nullptr) {
            details = vbox({
                hbox({text("Model: ") | bold, text(model->id)}),
                hbox({text("Recipe: ") | bold, text(model->recipe.empty() ? "-" : model->recipe)}),
                hbox({text("Status: ") | bold, text(model->downloaded ? "downloaded" : "missing, will load/download in background")}),
                hbox({text("Labels: ") | bold, text(labels_to_string(model->labels))}),
            });
            if (agents[static_cast<size_t>(agent_selected)] == "codex" &&
                contains_case_insensitive(model->id, "qwen3.5")) {
                details = vbox({details, text("Warning: Qwen 3.5 models are currently avoided for Codex") | color(ftxui::Color::Yellow)});
            }
        }

        Element options = vbox({
            hbox({text("ctx_size      "), ctx_input->Render()}),
            hbox({text("llamacpp      "), backend_input->Render()}),
            hbox({text("agent args    "), agent_args_input->Render()}),
            hbox({text("codex provider"), codex_provider_input->Render()}),
            codex_config_checkbox->Render(),
        });

        Element model_list = model_menu->Render() | vscroll_indicator | frame | flex;

        return vbox({
            text("Lemonade Launch") | bold,
            hbox({
                section_box("Agent", agent_menu->Render(), focus == 0),
                section_box("Models", vbox({search_input->Render(), mode_toggle->Render(), model_list}), focus == 1) | flex,
                section_box("Details", details, false) | flex,
            }) | flex,
            hbox({
                section_box("Options", options, focus == 2) | flex,
                section_box("Actions", hbox({launch_button->Render(), text(" "), quit_button->Render()}), focus == 3),
            }),
            text("Tab next  Shift+Tab previous  / search  Enter launch  q quit") | dim,
        });
    });

    auto root = CatchEvent(renderer, [&](Event event) {
        if (event == Event::Tab) {
            focus = (focus + 1) % static_cast<int>(focusable.size());
            focusable[static_cast<size_t>(focus)]->TakeFocus();
            return true;
        }
        if (event == Event::TabReverse) {
            focus = (focus + static_cast<int>(focusable.size()) - 1) %
                    static_cast<int>(focusable.size());
            focusable[static_cast<size_t>(focus)]->TakeFocus();
            return true;
        }
        if (event == Event::Character('/')) {
            focus = 1;
            search_input->TakeFocus();
            return true;
        }
        if (event == Event::Return && (focus == 1 || focus == 3) &&
            ((mode == 0 && !filtered_recipes.empty() &&
              selected_model < static_cast<int>(filtered_recipes.size())) ||
             selected_info() != nullptr)) {
            accepted = true;
            screen_ptr->ExitLoopClosure()();
            return true;
        }
        if (event == Event::Character('q') && focus != 1 && focus != 2) {
            accepted = false;
            screen_ptr->ExitLoopClosure()();
            return true;
        }
        return false;
    });

    auto screen = ScreenInteractive::TerminalOutput();
    screen_ptr = &screen;
    agent_section->TakeFocus();
    screen.Loop(root);
    if (!accepted) {
        return false;
    }

    state.agent = agents[static_cast<size_t>(agent_selected)];
    if (mode == 0) {
        if (filtered_recipes.empty() || selected_model >= static_cast<int>(filtered_recipes.size())) {
            return false;
        }
        state.recipe_dir = "coding-agents";
        state.recipe_file = recipes[static_cast<size_t>(
            filtered_recipes[static_cast<size_t>(selected_model)])];
        state.model.clear();
    } else {
        const auto* model = selected_info();
        if (model == nullptr) {
            return false;
        }
        state.recipe_dir.clear();
        state.recipe_file.clear();
        state.model = model->id;
    }
    state.agent_args = agent_args;
    state.codex_model_provider = codex_provider;
    state.codex_use_user_config = codex_use_user_config;
    if (!state.recipe_options.is_object()) {
        state.recipe_options = nlohmann::json::object();
    }
    if (!ctx_size.empty()) {
        try {
            state.recipe_options["ctx_size"] = std::stoi(ctx_size);
        } catch (const std::exception&) {
        }
    }
    if (!llamacpp_backend.empty()) {
        state.recipe_options["llamacpp_backend"] = llamacpp_backend;
    }
    return true;
}

}  // namespace lemon_cli
