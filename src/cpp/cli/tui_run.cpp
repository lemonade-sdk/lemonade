#include "lemon_cli/tui_run.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
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
using ftxui::gauge;
using ftxui::hbox;
using ftxui::paragraph;
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

std::string shorten_middle(const std::string& value, size_t max_length) {
    if (value.size() <= max_length || max_length < 8) {
        return value;
    }
    const size_t head = (max_length - 3) / 2;
    const size_t tail = max_length - 3 - head;
    return value.substr(0, head) + "..." + value.substr(value.size() - tail);
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

bool has_label(const lemonade::ModelInfo& model, const std::string& label) {
    return std::find(model.labels.begin(), model.labels.end(), label) != model.labels.end();
}

std::string searchable_text(const lemonade::ModelInfo& model) {
    std::string text = model.id + " " + model.checkpoint + " " + model.recipe + " ";
    text += model.downloaded ? "downloaded local " : "missing not-downloaded ";
    for (const auto& label : model.labels) {
        text += label + " ";
    }
    static const std::vector<std::string> prefixes = {"user.", "builtin.", "extra."};
    for (const auto& prefix : prefixes) {
        if (model.id.rfind(prefix, 0) == 0) {
            text += model.id.substr(prefix.size()) + " ";
            break;
        }
    }
    return text;
}

std::vector<int> filter_models(const std::vector<lemonade::ModelInfo>& models,
                               const std::string& query,
                               int mode) {
    std::vector<int> filtered;
    for (size_t i = 0; i < models.size(); ++i) {
        const auto& model = models[i];
        if (mode == 0 && !model.downloaded) {
            continue;
        }
        if (mode == 2 && !has_label(model, "hot")) {
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

std::vector<std::string> model_menu_entries(const std::vector<lemonade::ModelInfo>& models,
                                            const std::vector<int>& filtered) {
    std::vector<std::string> entries;
    entries.reserve(filtered.size());
    for (int index : filtered) {
        const auto& model = models[static_cast<size_t>(index)];
        std::string status = model.downloaded ? "local" : "pull";
        entries.push_back(shorten_middle(model.id, 42) + "  [" + status + "]");
    }
    if (entries.empty()) {
        entries.push_back("No models match the current filter");
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

Element detail_row(const std::string& label, const std::string& value) {
    return hbox({text(label) | bold, paragraph(value.empty() ? "-" : value) | flex});
}

}  // namespace

bool run_tui(lemonade::LemonadeClient& client, RunTuiState& state) {
    std::vector<lemonade::ModelInfo> models = client.get_models(true);
    if (models.empty()) {
        return false;
    }

    std::string search;
    int mode = 0;
    int selected = 0;
    int focus = 0;
    bool accepted = false;
    bool save_options = state.save_options;
    bool pin_model = state.pinned.value_or(false);
    bool set_pin = state.pinned.has_value();
    bool chat_cli = state.chat_cli;
    std::string ctx_size;
    std::string llamacpp_backend;
    std::string llamacpp_device;

    std::vector<std::string> modes = {"Downloaded", "All", "Hot"};
    std::vector<int> filtered = filter_models(models, search, mode);
    std::vector<std::string> entries = model_menu_entries(models, filtered);

    if (!state.model.empty()) {
        auto it = std::find_if(filtered.begin(), filtered.end(), [&](int index) {
            return models[static_cast<size_t>(index)].id == state.model;
        });
        if (it != filtered.end()) {
            selected = static_cast<int>(std::distance(filtered.begin(), it));
        } else {
            search = state.model;
            mode = 1;
        }
    }

    auto refresh_filter = [&] {
        filtered = filter_models(models, search, mode);
        entries = model_menu_entries(models, filtered);
        if (selected >= static_cast<int>(entries.size())) {
            selected = static_cast<int>(entries.size()) - 1;
        }
        if (selected < 0) {
            selected = 0;
        }
    };

    auto selected_model = [&]() -> const lemonade::ModelInfo* {
        if (filtered.empty() || selected >= static_cast<int>(filtered.size())) {
            return nullptr;
        }
        return &models[static_cast<size_t>(filtered[static_cast<size_t>(selected)])];
    };

    auto search_input = Input(&search, "Search models, labels, recipes");
    auto mode_toggle = Toggle(&modes, &mode);
    auto menu = Menu(&entries, &selected, MenuOption::Vertical());
    auto save_checkbox = Checkbox("Save options for future runs", &save_options);
    auto set_pin_checkbox = Checkbox("Set pin state", &set_pin);
    auto pin_checkbox = Checkbox("Pinned", &pin_model);
    auto chat_checkbox = Checkbox("Open chat CLI instead of browser", &chat_cli);
    auto ctx_input = Input(&ctx_size, "auto");
    auto backend_input = Input(&llamacpp_backend, "auto");
    auto device_input = Input(&llamacpp_device, "auto");
    auto options_container = Container::Vertical({save_checkbox, set_pin_checkbox, pin_checkbox,
                                                  chat_checkbox, ctx_input, backend_input,
                                                  device_input});

    ScreenInteractive* screen_ptr = nullptr;
    auto load_button = Button("Load", [&] {
        if (selected_model() != nullptr) {
            accepted = true;
            screen_ptr->ExitLoopClosure()();
        }
    });
    auto quit_button = Button("Quit", [&] {
        accepted = false;
        screen_ptr->ExitLoopClosure()();
    });
    auto actions_container = Container::Horizontal({load_button, quit_button});

    std::vector<Component> focusable = {
        search_input,
        mode_toggle,
        menu,
        options_container,
        actions_container,
    };

    auto layout = Container::Vertical({
        search_input,
        mode_toggle,
        menu,
        options_container,
        actions_container,
    });

    auto renderer = Renderer(layout, [&] {
        refresh_filter();

        const lemonade::ModelInfo* model = selected_model();
        Element details = text("No model selected") | dim;
        if (model != nullptr) {
            const std::string labels = labels_to_string(model->labels);
            details = vbox({
                detail_row("Model: ", model->id),
                detail_row("Recipe: ", model->recipe),
                detail_row("Status: ", model->downloaded ? "downloaded" : "missing, will pull before load"),
                detail_row("Checkpoint: ", model->checkpoint),
                detail_row("Labels: ", labels),
            });
        }

        Element pin_element = pin_checkbox->Render();
        if (!set_pin) {
            pin_element = pin_element | dim;
        }

        Element options = vbox({
            save_checkbox->Render(),
            set_pin_checkbox->Render(),
            pin_element,
            chat_checkbox->Render(),
            hbox({text("ctx_size      "), ctx_input->Render()}),
            hbox({text("llamacpp      "), backend_input->Render()}),
            hbox({text("device        "), device_input->Render()}),
        });

        const double visible_count = static_cast<double>(std::max<size_t>(1, filtered.size()));
        const double all_count = static_cast<double>(std::max<size_t>(1, models.size()));

        Element model_list = menu->Render() | vscroll_indicator | frame;

        return vbox({
            text("Lemonade Run") | bold,
            hbox({
                section_box("Search", search_input->Render(), focus == 0) | flex,
                section_box("Mode", mode_toggle->Render(), focus == 1),
            }),
            hbox({
                section_box("Models", model_list, focus == 2) | flex,
                section_box("Details", details, false) | flex,
            }) | flex,
            hbox({
                section_box("Options", options, focus == 3) | flex,
                section_box("Actions", hbox({load_button->Render(), text(" "), quit_button->Render()}), focus == 4),
            }),
            hbox({
                text("Matches "),
                text(std::to_string(filtered.size()) + "/" + std::to_string(models.size())),
                text(" "),
                gauge(visible_count / all_count) | flex,
                text("  Tab next  Shift+Tab prev  / search  Enter load  q quit") | dim,
            }),
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
            focus = 0;
            search_input->TakeFocus();
            return true;
        }
        if (event == Event::Return && focus == 2 && selected_model() != nullptr) {
            accepted = true;
            screen_ptr->ExitLoopClosure()();
            return true;
        }
        if (event == Event::Character('q') && focus != 0 && focus != 3) {
            accepted = false;
            screen_ptr->ExitLoopClosure()();
            return true;
        }
        return false;
    });

    auto screen = ScreenInteractive::TerminalOutput();
    screen_ptr = &screen;
    search_input->TakeFocus();
    screen.Loop(root);

    if (!accepted) {
        return false;
    }

    const lemonade::ModelInfo* model = selected_model();
    if (model == nullptr) {
        return false;
    }

    state.model = model->id;
    state.save_options = save_options;
    state.pinned = set_pin ? std::optional<bool>(pin_model) : std::nullopt;
    state.chat_cli = chat_cli;
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
    if (!llamacpp_device.empty()) {
        state.recipe_options["llamacpp_device"] = llamacpp_device;
    }
    return true;
}

}  // namespace lemon_cli
