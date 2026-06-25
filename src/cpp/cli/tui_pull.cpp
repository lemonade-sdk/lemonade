#include "lemon_cli/tui_pull.h"

#include "lemon_cli/hf_pull.h"

#include <lemon/utils/http_client.h>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace lemon_cli {
namespace {

using ftxui::Button;
using ftxui::CatchEvent;
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
using ftxui::separator;
using ftxui::text;
using ftxui::vbox;
using ftxui::vscroll_indicator;

namespace Container = ftxui::Container;
using json = nlohmann::json;

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool contains_case_insensitive(const std::string& value, const std::string& needle) {
    return to_lower(value).find(to_lower(needle)) != std::string::npos;
}

std::string url_encode(const std::string& s) {
    std::ostringstream out;
    out << std::hex << std::uppercase << std::setfill('0');
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out << static_cast<char>(c);
        } else {
            out << '%' << std::setw(2) << static_cast<int>(c);
        }
    }
    return out.str();
}

std::string human_size(uint64_t bytes) {
    if (bytes == 0) {
        return "unknown";
    }
    constexpr double gb = 1024.0 * 1024.0 * 1024.0;
    constexpr double mb = 1024.0 * 1024.0;
    std::ostringstream out;
    out << std::fixed << std::setprecision(1);
    if (bytes >= static_cast<uint64_t>(gb)) {
        out << (static_cast<double>(bytes) / gb) << " GB";
    } else {
        out << (static_cast<double>(bytes) / mb) << " MB";
    }
    return out.str();
}

std::string normalize_user_model_name(std::string name) {
    if (name.rfind("user.", 0) == 0) {
        return name;
    }
    return "user." + name;
}

std::string variant_label(const json& variant) {
    const std::string name = variant.value("name", "");
    const size_t files = variant.contains("files") && variant["files"].is_array()
        ? variant["files"].size()
        : 1;
    const uint64_t size = variant.value("size_bytes", static_cast<uint64_t>(0));
    return name + "  " + std::to_string(files) + (files == 1 ? " file  " : " files  ") +
           human_size(size);
}

std::vector<int> filter_models(const std::vector<lemonade::ModelInfo>& models,
                               const std::string& query) {
    std::vector<int> filtered;
    for (size_t i = 0; i < models.size(); ++i) {
        const auto& model = models[i];
        std::string searchable = model.id + " " + model.checkpoint + " " + model.recipe + " ";
        searchable += model.downloaded ? "downloaded local " : "missing not-downloaded ";
        for (const auto& label : model.labels) {
            searchable += label + " ";
        }
        if (query.empty() || contains_case_insensitive(searchable, query)) {
            filtered.push_back(static_cast<int>(i));
        }
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

std::vector<std::string> built_in_entries(const std::vector<lemonade::ModelInfo>& models,
                                          const std::vector<int>& filtered) {
    std::vector<std::string> entries;
    for (int index : filtered) {
        const auto& model = models[static_cast<size_t>(index)];
        entries.push_back(model.id + "  [" + (model.downloaded ? "downloaded" : "missing") +
                          "]  " + (model.recipe.empty() ? "-" : model.recipe));
    }
    if (entries.empty()) {
        entries.push_back("No models match the current filter");
    }
    return entries;
}

std::vector<std::string> search_huggingface(const std::string& query, std::string& error) {
    std::vector<std::string> results;
    if (query.empty()) {
        return results;
    }

    try {
        std::string url = "https://huggingface.co/api/models?search=" + url_encode(query) +
                          "&limit=25";
        auto response = lemon::utils::HttpClient::get(url, {{"Accept", "application/json"}}, 20);
        if (response.status_code != 200) {
            error = "Hugging Face search failed: HTTP " + std::to_string(response.status_code);
            return results;
        }
        auto parsed = json::parse(response.body);
        if (!parsed.is_array()) {
            error = "Hugging Face search returned an unexpected response";
            return results;
        }
        for (const auto& item : parsed) {
            std::string id = item.value("modelId", item.value("id", ""));
            if (id.empty()) {
                continue;
            }
            if (contains_case_insensitive(id, "GGUF")) {
                results.push_back(id);
            }
        }
        if (results.empty()) {
            for (const auto& item : parsed) {
                std::string id = item.value("modelId", item.value("id", ""));
                if (!id.empty()) {
                    results.push_back(id);
                }
            }
        }
    } catch (const std::exception& e) {
        error = e.what();
    }
    return results;
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

using json = nlohmann::json;

struct PullProgressState {
    std::mutex mutex;
    std::string model_name;
    std::string file;
    std::string status = "starting";
    std::string error;
    int file_index = 0;
    int total_files = 0;
    uint64_t bytes_downloaded = 0;
    uint64_t bytes_total = 0;
    uint64_t total_download_size = 0;
    bool success = false;
    bool done = false;
    bool cancel_requested = false;
};

std::string format_bytes(uint64_t bytes) {
    constexpr double gb = 1024.0 * 1024.0 * 1024.0;
    constexpr double mb = 1024.0 * 1024.0;
    constexpr double kb = 1024.0;
    std::ostringstream out;
    out << std::fixed << std::setprecision(1);
    if (bytes >= static_cast<uint64_t>(gb)) {
        out << (static_cast<double>(bytes) / gb) << " GB";
    } else if (bytes >= static_cast<uint64_t>(mb)) {
        out << (static_cast<double>(bytes) / mb) << " MB";
    } else if (bytes >= static_cast<uint64_t>(kb)) {
        out << (static_cast<double>(bytes) / kb) << " KB";
    } else {
        out << bytes << " B";
    }
    return out.str();
}

void apply_pull_progress_event(const std::string& event_type,
                               const std::string& event_data,
                               PullProgressState& state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (event_type == "complete") {
        state.status = "completed";
        state.success = true;
        state.done = true;
        return;
    }
    if (event_type == "error") {
        try {
            auto parsed = json::parse(event_data);
            state.error = parsed.value("error", event_data);
        } catch (const std::exception&) {
            state.error = event_data;
        }
        state.status = "error";
        state.done = true;
        return;
    }

    try {
        auto parsed = json::parse(event_data);
        if (parsed.contains("file") && parsed["file"].is_string()) {
            state.file = parsed["file"].get<std::string>();
            state.file_index = parsed.value("file_index", 0);
            state.total_files = parsed.value("total_files", 0);
            state.bytes_downloaded = parsed.value("bytes_downloaded", static_cast<uint64_t>(0));
            state.bytes_total = parsed.value("bytes_total", static_cast<uint64_t>(0));
            state.total_download_size =
                parsed.value("total_download_size", static_cast<uint64_t>(0));
            uint64_t previous =
                parsed.value("bytes_previously_downloaded", static_cast<uint64_t>(0));
            state.status = (state.bytes_total > 0 && previous == state.bytes_total)
                ? "already downloaded"
                : "downloading";
        }
        if (parsed.contains("error") && parsed["error"].is_string()) {
            state.error = parsed["error"].get<std::string>();
            state.status = "error";
        }
        if (parsed.contains("complete")) {
            state.status = "completed";
            state.success = true;
            state.done = true;
        }
    } catch (const std::exception&) {
    }
}

int pull_progress_tui(lemonade::LemonadeClient& client,
                      const PullTuiResult& pull,
                      bool upgrade) {
    PullProgressState state;
    state.model_name = pull.display_name.empty()
        ? pull.request.value("model_name", std::string())
        : pull.display_name;

    json request_body = pull.request;
    request_body["stream"] = true;
    if (!request_body.contains("do_not_upgrade")) {
        request_body["do_not_upgrade"] = !upgrade;
    }

    ScreenInteractive* screen_ptr = nullptr;
    std::atomic<bool> worker_done{false};

    std::thread worker([&] {
        try {
            client.make_request(
                "/api/v1/pull",
                "POST",
                request_body.dump(),
                "application/json",
                [&](const std::string& event_type, const std::string& event_data) {
                    apply_pull_progress_event(event_type, event_data, state);
                    if (screen_ptr != nullptr) {
                        screen_ptr->PostEvent(Event::Custom);
                    }
                },
                86400000,
                86400000,
                [&] {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    return state.cancel_requested;
                });
        } catch (const lemonade::HttpError& e) {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.error = lemonade::extract_server_error_message(e);
            state.status = "error";
            state.done = true;
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.error = e.what();
            state.status = "error";
            state.done = true;
        }
        worker_done = true;
        if (screen_ptr != nullptr) {
            screen_ptr->PostEvent(Event::Custom);
        }
    });

    auto cancel_button = Button("Cancel", [&] {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.cancel_requested = true;
        state.status = "cancelling";
    });
    auto close_button = Button("Close", [&] { screen_ptr->ExitLoopClosure()(); });
    auto actions = Container::Horizontal({cancel_button, close_button});

    auto renderer = Renderer(actions, [&] {
        PullProgressState snapshot;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            snapshot.model_name = state.model_name;
            snapshot.file = state.file;
            snapshot.status = state.status;
            snapshot.error = state.error;
            snapshot.file_index = state.file_index;
            snapshot.total_files = state.total_files;
            snapshot.bytes_downloaded = state.bytes_downloaded;
            snapshot.bytes_total = state.bytes_total;
            snapshot.total_download_size = state.total_download_size;
            snapshot.success = state.success;
            snapshot.done = state.done;
            snapshot.cancel_requested = state.cancel_requested;
        }

        double progress = 0.0;
        if (snapshot.bytes_total > 0) {
            progress = static_cast<double>(snapshot.bytes_downloaded) /
                       static_cast<double>(snapshot.bytes_total);
            if (progress > 1.0) {
                progress = 1.0;
            }
        }

        std::string file_label = snapshot.file.empty() ? "Waiting for first file" : snapshot.file;
        std::string count_label = snapshot.total_files > 0
            ? "[" + std::to_string(snapshot.file_index) + "/" +
                  std::to_string(snapshot.total_files) + "] "
            : "";
        std::string bytes_label = snapshot.bytes_total > 0
            ? format_bytes(snapshot.bytes_downloaded) + " / " + format_bytes(snapshot.bytes_total)
            : "";

        Element body = vbox({
            text("Pulling " + snapshot.model_name) | bold,
            separator(),
            hbox({text("Status: ") | bold, text(snapshot.status)}),
            hbox({text(count_label), text(file_label)}),
            gauge(progress),
            text(bytes_label),
        });
        if (snapshot.total_download_size > 0) {
            body = vbox({body, text("Total: " + format_bytes(snapshot.total_download_size))});
        }
        if (!snapshot.error.empty()) {
            body = vbox({body, text(snapshot.error) | color(ftxui::Color::Red)});
        }
        if (snapshot.done) {
            body = vbox({body, text("Done. Press Close or q.") | dim});
        }

        return vbox({
            body | border,
            hbox({cancel_button->Render(), text(" "), close_button->Render()}),
        });
    });

    auto root = CatchEvent(renderer, [&](Event event) {
        if (event == Event::Character('q')) {
            bool done = false;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                done = state.done;
            }
            if (done) {
                screen_ptr->ExitLoopClosure()();
                return true;
            }
        }
        if (event == Event::Custom && worker_done.load()) {
            bool done = false;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                done = state.done;
            }
            if (done) {
                return true;
            }
        }
        return false;
    });

    auto screen = ScreenInteractive::TerminalOutput();
    screen_ptr = &screen;
    screen.Loop(root);

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.done) {
            state.cancel_requested = true;
        }
    }
    if (worker.joinable()) {
        worker.join();
    }

    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.success) {
        return 0;
    }
    if (!state.error.empty()) {
        std::cerr << "Error pulling model: " << state.error << std::endl;
    }
    return 1;
}

bool pull_tui(lemonade::LemonadeClient& client,
              const std::string& initial_model,
              PullTuiResult& result) {
    std::vector<lemonade::ModelInfo> models = client.get_models(true);
    std::vector<std::string> source_modes = {"Built-in", "Hugging Face"};
    int source_mode = initial_model.find('/') == std::string::npos ? 0 : 1;
    int focus = 0;
    bool accepted = false;
    std::string model_search = source_mode == 0 ? initial_model : "";
    std::string hf_search = source_mode == 1 ? normalize_huggingface_checkpoint_arg(initial_model) : "";
    std::string registration_name;
    std::string status = "Search built-ins or Hugging Face repositories";
    std::vector<int> filtered_models = filter_models(models, model_search);
    std::vector<std::string> model_entries = built_in_entries(models, filtered_models);
    int model_selected = 0;
    std::vector<std::string> hf_results;
    int hf_selected = 0;
    json variants_response;
    std::vector<std::string> variant_entries = {"Search and select a Hugging Face repo"};
    int variant_selected = 0;

    auto selected_checkpoint = [&]() -> std::string {
        if (source_mode == 1 && !hf_results.empty() &&
            hf_selected < static_cast<int>(hf_results.size())) {
            return hf_results[static_cast<size_t>(hf_selected)];
        }
        return normalize_huggingface_checkpoint_arg(hf_search);
    };

    auto refresh_built_ins = [&] {
        filtered_models = filter_models(models, model_search);
        model_entries = built_in_entries(models, filtered_models);
        if (model_selected >= static_cast<int>(model_entries.size())) {
            model_selected = static_cast<int>(model_entries.size()) - 1;
        }
        if (model_selected < 0) {
            model_selected = 0;
        }
    };

    auto load_variants = [&] {
        const std::string checkpoint = selected_checkpoint();
        if (checkpoint.find('/') == std::string::npos) {
            status = "Enter a Hugging Face repo id like owner/repo";
            return;
        }
        try {
            std::string body = client.make_request(
                "/api/v1/pull/variants?checkpoint=" + url_encode(checkpoint), "GET");
            variants_response = json::parse(body);
            variant_entries.clear();
            if (variants_response.value("repo_kind", "") == "collection") {
                size_t component_count = variants_response.value("component_count", static_cast<size_t>(0));
                variant_entries.push_back("Omni collection  " + std::to_string(component_count) +
                                          (component_count == 1 ? " component" : " components"));
            } else if (variants_response.contains("variants") && variants_response["variants"].is_array()) {
                for (const auto& variant : variants_response["variants"]) {
                    variant_entries.push_back(variant_label(variant));
                }
            }
            if (variant_entries.empty()) {
                variant_entries.push_back("No variants found");
            }
            variant_selected = 0;
            std::string suggested = variants_response.value("suggested_name", checkpoint);
            if (variants_response.value("repo_kind", "") == "collection") {
                registration_name = suggested;
            } else if (variants_response.contains("variants") &&
                       variants_response["variants"].is_array() &&
                       !variants_response["variants"].empty()) {
                registration_name = suggested + "-" +
                    variants_response["variants"][0].value("name", std::string());
            }
            status = "Variants loaded for " + checkpoint;
        } catch (const lemonade::HttpError& e) {
            status = lemonade::extract_server_error_message(e);
        } catch (const std::exception& e) {
            status = e.what();
        }
    };

    auto build_result = [&]() -> bool {
        if (source_mode == 0) {
            if (filtered_models.empty() || model_selected >= static_cast<int>(filtered_models.size())) {
                status = "Select a built-in model first";
                return false;
            }
            const auto& model = models[static_cast<size_t>(
                filtered_models[static_cast<size_t>(model_selected)])];
            result.request = {{"model_name", model.id}};
            result.display_name = model.id;
            return true;
        }

        const std::string checkpoint = selected_checkpoint();
        if (variants_response.empty()) {
            load_variants();
        }
        if (variants_response.empty()) {
            return false;
        }
        std::string model_name = registration_name.empty()
            ? variants_response.value("suggested_name", checkpoint)
            : registration_name;
        result.display_name = model_name;
        result.request = json::object();
        result.request["model_name"] = normalize_user_model_name(model_name);
        std::string recipe = variants_response.value("recipe", std::string("llamacpp"));
        result.request["recipe"] = recipe;
        if (variants_response.value("repo_kind", "") == "collection") {
            result.request["checkpoints"] = json::object();
            result.request["checkpoints"]["main"] = checkpoint;
            result.request["recipe"] = "collection.omni";
            return true;
        }
        if (!variants_response.contains("variants") || !variants_response["variants"].is_array() ||
            variants_response["variants"].empty()) {
            status = "No variants are available for this repository";
            return false;
        }
        if (variant_selected >= static_cast<int>(variants_response["variants"].size())) {
            variant_selected = 0;
        }
        const auto& variant = variants_response["variants"][static_cast<size_t>(variant_selected)];
        std::string variant_name = variant.value("name", std::string());
        result.request["checkpoint"] = checkpoint + ":" + variant_name;
        if (variants_response.contains("suggested_labels") &&
            variants_response["suggested_labels"].is_array() &&
            !variants_response["suggested_labels"].empty()) {
            result.request["labels"] = variants_response["suggested_labels"];
        }
        if (variants_response.contains("mmproj_files") &&
            variants_response["mmproj_files"].is_array() &&
            !variants_response["mmproj_files"].empty()) {
            result.request["mmproj"] = variants_response["mmproj_files"][0];
        }
        return true;
    };

    auto source_toggle = Toggle(&source_modes, &source_mode);
    auto model_input = Input(&model_search, "Search registered models");
    auto hf_input = Input(&hf_search, "Search Hugging Face or type owner/repo");
    auto model_menu = Menu(&model_entries, &model_selected, MenuOption::Vertical());
    auto hf_menu = Menu(&hf_results, &hf_selected, MenuOption::Vertical());
    auto variant_menu = Menu(&variant_entries, &variant_selected, MenuOption::Vertical());
    auto name_input = Input(&registration_name, "user model name");

    ScreenInteractive* screen_ptr = nullptr;
    auto search_button = Button("Search HF", [&] {
        std::string error;
        hf_results = search_huggingface(hf_search, error);
        hf_selected = 0;
        status = error.empty()
            ? "Found " + std::to_string(hf_results.size()) + " Hugging Face repositories"
            : error;
    });
    auto inspect_button = Button("Inspect", [&] { load_variants(); });
    auto pull_button = Button("Pull", [&] {
        if (build_result()) {
            accepted = true;
            screen_ptr->ExitLoopClosure()();
        }
    });
    auto quit_button = Button("Quit", [&] {
        accepted = false;
        screen_ptr->ExitLoopClosure()();
    });

    auto search_controls = Container::Vertical({source_toggle, model_input, hf_input, search_button});
    auto result_controls = Container::Vertical({model_menu, hf_menu});
    auto variant_controls = Container::Vertical({variant_menu, inspect_button});
    auto registration_controls = Container::Vertical({name_input});
    auto actions = Container::Horizontal({pull_button, quit_button});
    std::vector<Component> focusable = {
        search_controls,
        result_controls,
        variant_controls,
        registration_controls,
        actions,
    };
    auto layout = Container::Vertical({
        search_controls,
        result_controls,
        variant_controls,
        registration_controls,
        actions,
    });

    auto renderer = Renderer(layout, [&] {
        refresh_built_ins();
        Element search_section = vbox({
            source_toggle->Render(),
            source_mode == 0 ? model_input->Render() : hf_input->Render(),
            source_mode == 1 ? search_button->Render() : text("Built-ins come from /models") | dim,
        });
        Element results_section =
            (source_mode == 0 ? model_menu->Render() : hf_menu->Render()) | vscroll_indicator | frame;
        Element variants_section = source_mode == 0
            ? text("Built-in pulls do not need variant selection") | dim
            : vbox({variant_menu->Render() | vscroll_indicator | frame | flex,
                    inspect_button->Render()});
        Element registration_section = source_mode == 0
            ? text("Registered model name is used as-is") | dim
            : name_input->Render();
        return vbox({
            text("Lemonade Pull") | bold,
            hbox({
                section_box("Source / Search", search_section, focus == 0) | flex,
                section_box("Results", results_section, focus == 1) | flex,
            }) | flex,
            hbox({
                section_box("Variants", variants_section, focus == 2) | flex,
                section_box("Registration", registration_section, focus == 3) | flex,
            }) | flex,
            hbox({
                section_box("Actions", hbox({pull_button->Render(), text(" "), quit_button->Render()}), focus == 4),
                section_box("Status", text(status), false) | flex,
            }),
            text("Tab next  Shift+Tab previous  / search  Enter inspect/pull  q quit") | dim,
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
            if (source_mode == 0) {
                model_input->TakeFocus();
            } else {
                hf_input->TakeFocus();
            }
            return true;
        }
        if (event == Event::Return) {
            if (source_mode == 1 && focus == 1) {
                load_variants();
                focus = 2;
                variant_controls->TakeFocus();
                return true;
            }
            if (focus == 2 || focus == 4) {
                if (build_result()) {
                    accepted = true;
                    screen_ptr->ExitLoopClosure()();
                }
                return true;
            }
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
    search_controls->TakeFocus();
    screen.Loop(root);
    return accepted;
}

}  // namespace lemon_cli
