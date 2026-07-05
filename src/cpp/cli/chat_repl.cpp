#include "lemon_cli/chat_repl.h"
#include "lemon_cli/model_selection.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
    #include <io.h>
    #include <windows.h>
#else
    #include <sys/ioctl.h>
    #include <unistd.h>
#endif

namespace lemon_cli {

namespace {

using json = nlohmann::json;

std::atomic<bool> g_interrupted{false};

#ifdef _WIN32
BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT) {
        g_interrupted = true;
        return TRUE;
    }
    return FALSE;
}
#else
void posix_sigint_handler(int) {
    g_interrupted = true;
}
#endif

void install_interrupt_handler() {
#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    std::signal(SIGINT, posix_sigint_handler);
#endif
}

void uninstall_interrupt_handler() {
#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
#else
    std::signal(SIGINT, SIG_DFL);
#endif
}

// =============================================================================
// Terminal UI helpers
// =============================================================================

struct TermUi {
    bool ansi = false;     // ANSI escape sequences are safe to emit
    bool unicode = true;   // box-drawing chars are safe to emit
    int width = 80;        // detected console width
};

bool stdout_is_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

int detect_console_width() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &csbi)) {
        int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        if (w > 0) return w;
    }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
#endif
    return 80;
}

TermUi init_term_ui() {
    TermUi ui;
    bool tty = stdout_is_tty();
    ui.ansi = tty;
    ui.unicode = tty;
    ui.width = detect_console_width();
    if (ui.width < 40) ui.width = 40;

#ifdef _WIN32
    // Enable ANSI escape sequence processing and UTF-8 output for the console.
    if (tty) {
        SetConsoleOutputCP(CP_UTF8);
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
            if (!(mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
                if (!SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
                    ui.ansi = false;
                }
            }
        } else {
            ui.ansi = false;
        }
    }
    // Respect the NO_COLOR convention.
    if (const char* nc = std::getenv("NO_COLOR")) {
        if (nc[0] != '\0') ui.ansi = false;
    }
#else
    if (const char* nc = std::getenv("NO_COLOR")) {
        if (nc[0] != '\0') ui.ansi = false;
    }
#endif
    return ui;
}

// Color/style helpers — return empty strings when ANSI is disabled so callers
// can blindly concatenate.
struct Style {
    const TermUi& ui;
    explicit Style(const TermUi& t) : ui(t) {}

    const char* reset()   const { return ui.ansi ? "\033[0m"  : ""; }
    const char* bold()    const { return ui.ansi ? "\033[1m"  : ""; }
    const char* dim()     const { return ui.ansi ? "\033[2m"  : ""; }
    const char* italic()  const { return ui.ansi ? "\033[3m"  : ""; }
    const char* yellow()  const { return ui.ansi ? "\033[38;5;221m" : ""; }
    const char* yellow_b() const { return ui.ansi ? "\033[1;38;5;221m" : ""; }
    const char* cyan()    const { return ui.ansi ? "\033[38;5;87m"  : ""; }
    const char* cyan_b()  const { return ui.ansi ? "\033[1;38;5;87m" : ""; }
    const char* green()   const { return ui.ansi ? "\033[38;5;120m" : ""; }
    const char* red()     const { return ui.ansi ? "\033[1;31m" : ""; }
    const char* gray()    const { return ui.ansi ? "\033[38;5;245m" : ""; }
    // Soft mauve used for the reasoning channel.
    const char* mauve()   const { return ui.ansi ? "\033[38;5;177m" : ""; }
    // Dim + italic mauve used when streaming reasoning text.
    const char* reasoning_text() const {
        return ui.ansi ? "\033[2;3;38;5;177m" : "";
    }
};

std::string repeat(const std::string& s, int n) {
    if (n <= 0) return "";
    std::string out;
    out.reserve(s.size() * static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

// Three-valued knob for whether reasoning is forced on, forced off, or left to
// the model's default. We only put `enable_thinking` in the request body when
// the user has explicitly chosen On or Off — sending nothing keeps current
// behavior for every model.
enum class ReasoningMode { Default, On, Off };

const char* reasoning_mode_label(ReasoningMode m) {
    switch (m) {
        case ReasoningMode::On:  return "on";
        case ReasoningMode::Off: return "off";
        case ReasoningMode::Default:
        default:                 return "default (model decides)";
    }
}

void apply_reasoning_to_request(json& req, ReasoningMode mode) {
    switch (mode) {
        case ReasoningMode::On:  req["enable_thinking"] = true;  break;
        case ReasoningMode::Off: req["enable_thinking"] = false; break;
        case ReasoningMode::Default: break;
    }
}

// Background "waiting for response" indicator. The spinner draws on the
// current line via carriage return and clears itself on stop(). It only
// animates when ANSI is supported so dumb terminals/log files stay clean.
class Spinner {
public:
    Spinner() = default;
    ~Spinner() { stop(); }
    Spinner(const Spinner&) = delete;
    Spinner& operator=(const Spinner&) = delete;

    void start(const TermUi& ui, const std::string& label) {
        if (running_.load()) return;
        if (!ui.ansi) return;  // no animation without ANSI; line would be noisy
        ui_ = &ui;
        label_ = label;
        running_ = true;
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        if (ui_ != nullptr && ui_->ansi) {
            // Erase the spinner line and park the cursor at column 0.
            std::cout << "\r\033[2K" << std::flush;
        }
    }

private:
    void run() {
        static const char* frames_unicode[] = {
            "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
            "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
            "\xe2\xa0\x87", "\xe2\xa0\x8f"
        };
        static const char* frames_ascii[] = {"|", "/", "-", "\\"};
        const char** frames = ui_->unicode ? frames_unicode : frames_ascii;
        const int n = ui_->unicode ? 10 : 4;

        Style s(*ui_);
        std::unique_lock<std::mutex> lk(mu_);
        int i = 0;
        while (running_.load()) {
            std::cout << "\r  " << s.cyan() << frames[i] << s.reset()
                      << " " << s.dim() << label_ << s.reset()
                      << std::flush;
            i = (i + 1) % n;
            cv_.wait_for(lk, std::chrono::milliseconds(90),
                         [this] { return !running_.load(); });
        }
    }

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex mu_;
    std::condition_variable cv_;
    const TermUi* ui_ = nullptr;
    std::string label_;
};

// Chat banner
const char* kBannerWide[] = {
    "  ██╗     ███████╗███╗   ███╗ ██████╗ ███╗   ██╗ █████╗ ██████╗ ███████╗",
    "  ██║     ██╔════╝████╗ ████║██╔═══██╗████╗  ██║██╔══██╗██╔══██╗██╔════╝",
    "  ██║     █████╗  ██╔████╔██║██║   ██║██╔██╗ ██║███████║██║  ██║█████╗  ",
    "  ██║     ██╔══╝  ██║╚██╔╝██║██║   ██║██║╚██╗██║██╔══██║██║  ██║██╔══╝  ",
    "  ███████╗███████╗██║ ╚═╝ ██║╚██████╔╝██║ ╚████║██║  ██║██████╔╝███████╗",
    "  ╚══════╝╚══════╝╚═╝     ╚═╝ ╚═════╝ ╚═╝  ╚══╝╚═╝  ╚═╝╚═════╝ ╚══════╝",
    nullptr,
};

struct LemonRow {
    const char* art;
    enum Color { Yellow, Green, Gray } color;
};
const LemonRow kLemonRows[] = {
    {"    ╭╮      ", LemonRow::Green },   // leaf top
    {"    ╰╮      ", LemonRow::Green },   // leaf stem
    {" ╭───┴───╮  ", LemonRow::Yellow},   // top of lemon
    {" │ ██━██ │  ", LemonRow::Gray  },   // sunglasses
    {" │  ╰─╯  │  ", LemonRow::Yellow},   // smile
    {"  ╰─────╯   ", LemonRow::Yellow},   // bottom
};
constexpr int kLemonRowCount = sizeof(kLemonRows) / sizeof(kLemonRows[0]);
constexpr int kLemonVisualWidth = 12;
constexpr int kBannerVisualWidth = 74;
constexpr int kBannerLemonGap = 3;

void print_banner(const TermUi& ui) {
    Style s(ui);
    std::cout << "\n";

    bool wide_banner = ui.unicode && ui.width >= 76;
    bool show_lemon  = wide_banner &&
        ui.width >= kBannerVisualWidth + kBannerLemonGap + kLemonVisualWidth;

    auto lemon_color = [&](LemonRow::Color c) -> const char* {
        switch (c) {
            case LemonRow::Green:  return s.green();
            case LemonRow::Gray:   return s.gray();
            case LemonRow::Yellow:
            default:               return s.yellow_b();
        }
    };

    if (wide_banner) {
        for (int i = 0; kBannerWide[i] != nullptr; ++i) {
            std::cout << s.yellow_b() << kBannerWide[i] << s.reset();
            if (show_lemon && i < kLemonRowCount) {
                std::cout << std::string(kBannerLemonGap, ' ')
                          << lemon_color(kLemonRows[i].color)
                          << kLemonRows[i].art
                          << s.reset();
            }
            std::cout << "\n";
        }
    } else {
        std::cout << "  " << s.yellow_b() << "Lemonade chat" << s.reset() << "\n";
        std::cout << "  " << s.dim() << "local LLMs, in your terminal" << s.reset() << "\n";
    }
    std::cout << "\n";
}

void print_welcome(const TermUi& ui, const std::string& model) {
    Style s(ui);
    std::cout << "  Welcome to " << s.yellow_b() << "Lemonade chat"
              << s.reset() << " " << s.dim() << "— local LLMs, in your terminal."
              << s.reset() << "\n\n";

    std::cout << "  " << s.bold() << "Tips for getting started:" << s.reset() << "\n";
    const char* bullet = ui.unicode ? "•" : "*";
    std::cout << "    " << s.cyan() << bullet << s.reset()
              << " Ask anything in natural language\n";
    std::cout << "    " << s.cyan() << bullet << s.reset()
              << " Try " << s.dim() << "\"write a haiku about lemons\"" << s.reset()
              << " or " << s.dim() << "\"explain transformers\"" << s.reset() << "\n";
    std::cout << "    " << s.cyan() << bullet << s.reset()
              << " " << s.dim() << "/help" << s.reset()
              << " for commands, " << s.dim() << "/exit" << s.reset() << " to quit\n";
    std::cout << "    " << s.cyan() << bullet << s.reset()
              << " " << s.dim() << "/model <name>" << s.reset()
              << " to switch models on the fly\n";
}

void print_section_divider(const TermUi& ui, const std::string& label = "") {
    Style s(ui);
    std::string dash = ui.unicode ? "─" : "-";
    if (label.empty()) {
        std::cout << "  " << s.gray() << repeat(dash, ui.width - 4) << s.reset() << "\n";
    } else {
        std::string mid = " " + label + " ";
        int avail = ui.width - 2 - 4 - static_cast<int>(mid.size());
        if (avail < 2) avail = 2;
        std::cout << "  "
                  << s.gray() << repeat(dash, 3) << s.reset()
                  << s.dim() << mid << s.reset()
                  << s.gray() << repeat(dash, avail) << s.reset() << "\n";
    }
}

void print_input_prompt(const TermUi& ui, bool continuation = false) {
    Style s(ui);
    if (continuation) {
        std::cout << "  " << s.dim() << "." << s.reset() << " " << std::flush;
    } else {
        std::cout << "  " << s.cyan_b() << ">" << s.reset() << " " << std::flush;
    }
}

// Sticky-style command hint above input prompts
void print_hint_bar(const TermUi& ui, const std::string& model, bool multiline) {
    Style s(ui);
    const char* sep = ui.unicode ? " \xc2\xb7 " : " | ";  // ·
    std::string dash = ui.unicode ? "\xe2\x94\x80" : "-"; // ─

    // Build status line: "  ─── model ────────────  ? /help for shortcuts"
    std::string hint = "? /help for shortcuts";
    std::string label = " " + (model.empty() ? std::string("chat") : model) + " ";
    int avail = ui.width - 2 /*indent*/ - 4 /*dashes around label*/
                - static_cast<int>(label.size())
                - static_cast<int>(hint.size()) - 2 /*two-space gap*/;
    if (avail < 2) avail = 2;

    std::cout << "\n"
              << "  "
              << s.gray() << repeat(dash, 3) << s.reset()
              << s.cyan_b() << label << s.reset()
              << s.gray() << repeat(dash, avail) << s.reset()
              << "  " << s.dim() << hint << s.reset() << "\n"
              << "  " << s.dim();
    if (multiline) {
        std::cout << "Submit: blank line" << sep
                  << "Clear line: Esc / Ctrl-U" << sep
                  << "Ctrl-C: cancel buffer (exits if empty)";
    } else {
        std::cout << "Clear line: Esc / Ctrl-U" << sep
                  << "Stop response or exit: Ctrl-C";
    }
    std::cout << s.reset() << "\n\n";
}

void print_error(const TermUi& ui, const std::string& message) {
    Style s(ui);
    const char* mark = ui.unicode ? "✖" : "x";
    std::cerr << "  " << s.red() << mark << " " << message << s.reset() << std::endl;
}

void print_info(const TermUi& ui, const std::string& message) {
    Style s(ui);
    std::cout << "  " << s.dim() << message << s.reset() << std::endl;
}

void print_help(const TermUi& ui) {
    Style s(ui);
    std::cout << "\n  " << s.bold() << "Slash commands" << s.reset() << "\n";

    struct Row { const char* cmd; const char* desc; };
    Row rows[] = {
        {"/help",                       "Show this help"},
        {"/exit, /quit",                "Leave the chat"},
        {"/clear",                      "Clear conversation history (keeps system prompt)"},
        {"/system <text>",              "Set/replace system prompt (also clears history)"},
        {"/model <name>",               "Switch active model (loads if needed, clears history)"},
        {"/load <name>",                "Load a model server-side without switching active chat"},
        {"/unload [name]",              "Unload one model, or all loaded models if omitted"},
        {"/list, /models",              "List chat models (downloaded and available to pull)"},
        {"  --all, -a",                 "Also include non-chat models (audio, image, embeddings, ...)"},
        {"  --downloaded, -d",          "Only show models already downloaded locally"},
        {"/ps",                         "Show currently loaded models"},
        {"/stats",                      "Show usage from the last assistant turn"},
        {"/think [on|off]",             "Show or set reasoning mode (no arg = show)"},
        {"/multiline",                  "Toggle multiline input (end with a blank line)"},
    };
    const int col = 24;
    for (const auto& r : rows) {
        std::cout << "    " << s.cyan() << r.cmd << s.reset();
        int pad = col - static_cast<int>(std::string(r.cmd).size());
        if (pad < 1) pad = 1;
        std::cout << std::string(pad, ' ') << s.dim() << r.desc << s.reset() << "\n";
    }
    std::cout << "\n  " << s.dim()
              << "Anything not starting with '/' is sent as a user message. "
              << "Esc / Ctrl-U clears the current line. "
              << "Ctrl-C stops a streaming response, cancels a multi-line buffer, or exits at an empty prompt."
              << s.reset() << "\n\n";
}

bool ensure_model_loaded(const TermUi& ui, lemonade::LemonadeClient& client, const std::string& model) {
    json info;
    try {
        info = client.get_model_info(model);
    } catch (const std::exception& e) {
        print_error(ui, std::string("failed to fetch model info for '") + model + "': " + e.what());
        return false;
    }
    if (info.empty()) {
        print_error(ui, std::string("unknown model '") + model + "'");
        return false;
    }

    bool downloaded = info.value("downloaded", false);
    if (!downloaded) {
        print_info(ui, std::string("Model '") + model + "' is not downloaded. Pulling\u2026");
        json pull_request;
        pull_request["model_name"] = model;
        if (client.pull_model(pull_request) != 0) {
            print_error(ui, std::string("failed to pull '") + model + "'");
            return false;
        }
    }

    json empty_recipe_options = json::object();
    if (client.load_model(model, empty_recipe_options, /*save_options=*/false) != 0) {
        print_error(ui, std::string("failed to load '") + model + "'");
        return false;
    }
    return true;
}

// Returns the model name reported by /health as `model_loaded`, or empty.
std::string fetch_currently_loaded_model(lemonade::LemonadeClient& client) {
    try {
        std::string body = client.make_request("/api/v1/health", "GET", "", "", 1000, 1000);
        json health = json::parse(body);
        if (health.contains("model_loaded") && health["model_loaded"].is_string()) {
            return health["model_loaded"].get<std::string>();
        }
    } catch (const std::exception&) {
        // best-effort
    }
    return {};
}

struct TurnResult {
    bool ok = false;
    bool interrupted = false;
    int prompt_tokens = 0;
    int completion_tokens = 0;
    double seconds = 0.0;
};

// Non-streaming version: returns the full assistant text via assistant_out.
TurnResult blocking_chat_turn(const TermUi& ui,
                              lemonade::LemonadeClient& client,
                              const std::string& model,
                              const json& messages,
                              ReasoningMode reasoning,
                              std::string& assistant_out) {
    TurnResult result;
    json request_body;
    request_body["model"] = model;
    request_body["messages"] = messages;
    request_body["stream"] = false;
    apply_reasoning_to_request(request_body, reasoning);

    Style s(ui);
    Spinner spinner;
    spinner.start(ui, "Generating\xe2\x80\xa6");  // "Generating…"

    auto start = std::chrono::steady_clock::now();
    try {
        std::string body = client.make_request("/api/v1/chat/completions", "POST",
                                               request_body.dump(), "application/json",
                                               /*connection_timeout_ms=*/30000,
                                               /*read_timeout_ms=*/600000);
        spinner.stop();
        json response = json::parse(body);
        if (response.contains("choices") && response["choices"].is_array() &&
            !response["choices"].empty()) {
            const auto& choice = response["choices"][0];
            const auto& message = choice.contains("message") ? choice["message"]
                                                              : json::object();

            // Render reasoning above the answer (dim italic mauve), kept out
            // of `assistant_out` so it never leaks back into chat history.
            if (message.contains("reasoning_content") &&
                message["reasoning_content"].is_string()) {
                const std::string thought =
                    message["reasoning_content"].get<std::string>();
                if (!thought.empty()) {
                    const char* glyph = ui.unicode ? "\xe2\x96\x8c" : "|";  // ▍
                    const char* ellipsis = ui.unicode ? "\xe2\x80\xa6" : "...";
                    std::cout << "\n  " << s.mauve() << glyph << s.reset()
                              << " " << s.dim() << s.italic() << "thinking"
                              << ellipsis << s.reset()
                              << "\n  " << s.reasoning_text();
                    for (char c : thought) {
                        std::cout << c;
                        if (c == '\n') std::cout << "  ";
                    }
                    std::cout << s.reset() << "\n";
                }
            }

            if (message.contains("content") && message["content"].is_string()) {
                assistant_out = message["content"].get<std::string>();
                std::cout << "\n  ";
                for (char c : assistant_out) {
                    std::cout << c;
                    if (c == '\n') std::cout << "  ";
                }
                std::cout << std::endl;
            }
        }
        if (response.contains("usage") && response["usage"].is_object()) {
            result.prompt_tokens = response["usage"].value("prompt_tokens", 0);
            result.completion_tokens = response["usage"].value("completion_tokens", 0);
        }
        result.ok = true;
    } catch (const std::exception& e) {
        spinner.stop();
        print_error(ui, e.what());
    }
    spinner.stop();
    auto end = std::chrono::steady_clock::now();
    result.seconds = std::chrono::duration<double>(end - start).count();
    result.interrupted = g_interrupted.load();
    return result;
}

// Stream + capture: keep the assistant text so the caller can update history.
TurnResult stream_chat_turn_capture(const TermUi& ui,
                                    lemonade::LemonadeClient& client,
                                    const std::string& model,
                                    const json& messages,
                                    ReasoningMode reasoning,
                                    std::string& assistant_out) {
    TurnResult result;

    json request_body;
    request_body["model"] = model;
    request_body["messages"] = messages;
    request_body["stream"] = true;
    request_body["stream_options"] = {{"include_usage", true}};
    apply_reasoning_to_request(request_body, reasoning);

    bool finished_cleanly = false;
    bool content_started = false;
    bool reasoning_started = false;
    bool reasoning_closed = false;
    int prompt_tokens = 0;
    int completion_tokens = 0;

    Style s(ui);
    Spinner spinner;
    spinner.start(ui, "Generating\xe2\x80\xa6");  // "Generating…"
    auto start = std::chrono::steady_clock::now();

    auto write_indented = [](const std::string& piece) {
        for (char c : piece) {
            std::cout << c;
            if (c == '\n') {
                std::cout << "  ";
            }
        }
    };

    auto start_reasoning_block = [&]() {
        const char* glyph = ui.unicode ? "\xe2\x96\x8c" : "|";  // ▍
        const char* ellipsis = ui.unicode ? "\xe2\x80\xa6" : "..."; // …
        std::cout << "\n  " << s.mauve() << glyph << s.reset()
                  << " " << s.dim() << s.italic() << "thinking" << ellipsis
                  << s.reset() << "\n  " << s.reasoning_text();
    };

    auto close_reasoning_block = [&]() {
        std::cout << s.reset();
        reasoning_closed = true;
    };

    auto start_content_block = [&]() {
        // Separate the assistant's answer from the (optional) reasoning
        // block above it with a blank line; otherwise just push it onto its
        // own line below the prompt.
        if (reasoning_started) {
            std::cout << "\n";
        }
        std::cout << "\n  ";
        content_started = true;
    };

    // Two-space left margin for output, matching the rest of the UI. We inject
    // the margin once at first content, and after every newline in the stream.
    auto callback = [&](const std::string& /*event_type*/, const std::string& event_data) {
        if (g_interrupted) {
            return;
        }
        if (event_data == "[DONE]") {
            finished_cleanly = true;
            return;
        }
        try {
            json chunk = json::parse(event_data);
            if (chunk.contains("choices") && chunk["choices"].is_array() &&
                !chunk["choices"].empty()) {
                const auto& choice = chunk["choices"][0];
                const auto& delta = choice.contains("delta") ? choice["delta"]
                                                              : json::object();

                // Reasoning channel — rendered above the answer in dim italic
                // mauve, kept out of `assistant_out` so it doesn't pollute
                // chat history.
                if (delta.contains("reasoning_content") &&
                    delta["reasoning_content"].is_string()) {
                    const std::string piece =
                        delta["reasoning_content"].get<std::string>();
                    if (!piece.empty()) {
                        spinner.stop();
                        if (!reasoning_started) {
                            start_reasoning_block();
                            reasoning_started = true;
                        }
                        write_indented(piece);
                        std::cout.flush();
                    }
                }

                // Main content channel.
                if (delta.contains("content") && delta["content"].is_string()) {
                    const std::string piece = delta["content"].get<std::string>();
                    if (!piece.empty()) {
                        spinner.stop();
                        if (reasoning_started && !reasoning_closed) {
                            close_reasoning_block();
                        }
                        assistant_out += piece;
                        if (!content_started) {
                            start_content_block();
                        }
                        write_indented(piece);
                        std::cout.flush();
                    }
                }
            }
            if (chunk.contains("usage") && chunk["usage"].is_object()) {
                prompt_tokens = chunk["usage"].value("prompt_tokens", prompt_tokens);
                completion_tokens = chunk["usage"].value("completion_tokens", completion_tokens);
            }
        } catch (const json::exception&) {
        }
    };

    try {
        client.make_request("/api/v1/chat/completions", "POST",
                            request_body.dump(), "application/json",
                            callback,
                            /*connection_timeout_ms=*/30000,
                            /*read_timeout_ms=*/600000,
                            /*should_abort=*/[]{ return g_interrupted.load(); });
    } catch (const std::exception& e) {
        spinner.stop();
        if (!g_interrupted) {
            std::cout << std::endl;
            print_error(ui, e.what());
        }
    }
    spinner.stop();

    // If reasoning streamed but content never arrived (e.g. interrupted mid-
    // thought) make sure we clear the active style so the prompt redraws clean.
    if (reasoning_started && !reasoning_closed) {
        close_reasoning_block();
    }

    auto end = std::chrono::steady_clock::now();
    result.seconds = std::chrono::duration<double>(end - start).count();
    result.prompt_tokens = prompt_tokens;
    result.completion_tokens = completion_tokens;
    result.interrupted = g_interrupted.load();
    result.ok = finished_cleanly && !result.interrupted;
    if (content_started || reasoning_started) {
        std::cout << std::endl;
    }
    return result;
}

void print_stats(const TermUi& ui, const TurnResult& last) {
    if (last.completion_tokens <= 0 || last.seconds <= 0.0) {
        return;
    }
    double tok_per_s = static_cast<double>(last.completion_tokens) / last.seconds;
    Style s(ui);
    const char* dash = ui.unicode ? "\xe2\x80\x94" : "--";  // — (U+2014)
    const char* sep  = ui.unicode ? " \xc2\xb7 "   : " | ";  // ·

    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(1);
    oss << last.completion_tokens << " tokens" << sep
        << tok_per_s << " tok/s" << sep
        << last.prompt_tokens << " prompt";

    std::cout << "  " << s.dim() << dash << " " << oss.str() << " " << dash
              << s.reset() << "\n";
}

// Read either a single line or when multiline mode is on, accumulate lines
// until a blank line is entered (submit). Ctrl-C while buffering to discard the
// in-progress lines and returning empty (the main loop will then redraw a fresh
// prompt). Returns false on EOF.
bool read_user_input(const TermUi& ui, bool multiline, std::string& out) {
    out.clear();
    if (!multiline) {
        if (!std::getline(std::cin, out)) {
            return false;
        }
        return true;
    }
    std::string line;
    bool got_any = false;
    while (std::getline(std::cin, line)) {
        if (g_interrupted.load()) {
            // Ctrl-C while composing: drop the buffered lines and consume the
            // interrupt so the main loop doesn't also treat it as "exit".
            g_interrupted = false;
            print_info(ui, "multi-line buffer cleared");
            out.clear();
            return true;
        }
        if (line.empty()) {
            return true;
        }
        if (got_any) out += "\n";
        out += line;
        got_any = true;
        print_input_prompt(ui, /*continuation=*/true);
    }
    if (g_interrupted.load() && got_any) {
        // EOF arrived because Ctrl-C tore the read down (POSIX EINTR path).
        // Same treatment: clear and continue rather than exit.
        g_interrupted = false;
        print_info(ui, "multi-line buffer cleared");
        out.clear();
        return true;
    }
    return got_any;
}

void rebuild_messages(json& messages, const std::string& system_prompt) {
    messages = json::array();
    if (!system_prompt.empty()) {
        messages.push_back({{"role", "system"}, {"content", system_prompt}});
    }
}

std::pair<std::string, std::string> split_command(const std::string& line) {
    size_t sp = line.find(' ');
    if (sp == std::string::npos) {
        return {line, ""};
    }
    std::string cmd = line.substr(0, sp);
    std::string rest = line.substr(sp + 1);
    while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
    return {cmd, rest};
}

// Print the model list, defaulting to chat models only. Pass include_non_chat=true
// (the /list --all opt-in) to also show audio/image/tts/embeddings/reranking.
void print_model_list(const TermUi& ui,
                      lemonade::LemonadeClient& client,
                      const std::string& active_model,
                      bool include_non_chat,
                      bool downloaded_only) {
    Style s(ui);

    std::vector<lemonade::ModelInfo> models;
    try {
        models = client.get_models(/*show_all=*/!downloaded_only);
    } catch (const std::exception& e) {
        print_error(ui, e.what());
        return;
    }

    static const std::vector<std::string> non_chat = {
        "embeddings", "reranking", "transcription", "image", "tts",
        "upscaling", "edit"
    };
    auto is_non_chat = [&](const lemonade::ModelInfo& m) {
        for (const auto& lbl : m.labels) {
            for (const auto& nl : non_chat) {
                if (lbl == nl) return true;
            }
        }
        return false;
    };

    // Best-effort: which models are currently loaded server-side?
    std::vector<std::string> loaded;
    try {
        std::string body = client.make_request("/api/v1/health", "GET", "", "", 1000, 1000);
        json health = json::parse(body);
        if (health.contains("all_models_loaded") && health["all_models_loaded"].is_array()) {
            for (const auto& m : health["all_models_loaded"]) {
                if (m.is_string()) {
                    loaded.push_back(m.get<std::string>());
                } else if (m.is_object() && m.contains("model_name") &&
                           m["model_name"].is_string()) {
                    loaded.push_back(m["model_name"].get<std::string>());
                }
            }
        }
    } catch (const std::exception&) {
        // ignore — loaded badge is optional
    }
    auto is_loaded = [&](const std::string& id) {
        for (const auto& l : loaded) {
            if (l == id) return true;
        }
        return false;
    };

    int shown = 0;
    int hidden_non_chat = 0;
    for (const auto& m : models) {
        if (!include_non_chat && is_non_chat(m)) {
            ++hidden_non_chat;
            continue;
        }

        const char* status_color = m.downloaded ? s.green() : s.gray();
        const char* status_glyph = ui.unicode
            ? (m.downloaded ? "\xe2\x97\x8f" : "\xe2\x97\x8b")  // U+25CF ● / U+25CB ○
            : (m.downloaded ? "*"            : "-");

        std::cout << "    " << status_color << status_glyph << s.reset() << " ";

        if (m.id == active_model) {
            std::cout << s.cyan_b() << m.id << s.reset();
        } else {
            std::cout << m.id;
        }

        std::vector<std::string> tags;
        if (is_loaded(m.id))   tags.push_back("loaded");
        if (m.id == active_model) tags.push_back("active");
        if (!m.recipe.empty()) tags.push_back(m.recipe);
        if (!tags.empty()) {
            std::cout << "  " << s.dim() << "(";
            for (size_t i = 0; i < tags.size(); ++i) {
                if (i) std::cout << ", ";
                std::cout << tags[i];
            }
            std::cout << ")" << s.reset();
        }
        std::cout << "\n";
        ++shown;
    }

    if (shown == 0) {
        if (downloaded_only) {
            print_info(ui, "no downloaded models match the filter");
        } else {
            print_info(ui, "no models match the filter");
        }
    } else {
        std::ostringstream foot;
        foot << shown << " " << (include_non_chat ? "model" : "chat model")
             << (shown == 1 ? "" : "s");
        if (!include_non_chat && hidden_non_chat > 0) {
            foot << " " << (ui.unicode ? "\xc2\xb7" : "*") << " "
                 << hidden_non_chat << " non-chat hidden (use /list --all)";
        }
        std::cout << "  " << s.dim() << foot.str() << s.reset() << "\n";
    }
}

} // namespace

int run_chat_repl(lemonade::LemonadeClient& client, const ChatOptions& options) {
    TermUi ui = init_term_ui();

    std::string active_model = options.initial_model;
    std::string system_prompt = options.system_prompt;
    bool stream = options.stream;

    if (active_model.empty()) {
        active_model = fetch_currently_loaded_model(client);
    }
    if (active_model.empty()) {
        if (!resolve_model_if_missing(client, active_model, "chat", /*show_all=*/true)) {
            return 1;
        }
    }

    if (!ensure_model_loaded(ui, client, active_model)) {
        return 1;
    }

    json messages;
    rebuild_messages(messages, system_prompt);

    print_banner(ui);
    print_welcome(ui, active_model);

    install_interrupt_handler();

    bool multiline = false;
    ReasoningMode reasoning_mode = ReasoningMode::Default;
    TurnResult last_turn;
    int exit_code = 0;

    while (true) {
        // Ctrl-C at the prompt exits the REPL. If it fires mid-stream, the
        // stream stops first; we land here at the top of the loop, see the
        // flag, and bail out.
        if (g_interrupted) {
            std::cout << std::endl;
            break;
        }

        print_hint_bar(ui, active_model, multiline);
        print_input_prompt(ui);
        std::string input;
        if (!read_user_input(ui, multiline, input)) {
            std::cout << std::endl;
            break;
        }

        if (input.empty()) {
            continue;
        }

        if (input[0] == '/') {
            auto [cmd, rest] = split_command(input);
            if (cmd == "/exit" || cmd == "/quit") {
                break;
            } else if (cmd == "/help") {
                print_help(ui);
            } else if (cmd == "/clear") {
                rebuild_messages(messages, system_prompt);
                print_info(ui, "history cleared");
            } else if (cmd == "/system") {
                system_prompt = rest;
                rebuild_messages(messages, system_prompt);
                print_info(ui, "system prompt updated; history cleared");
            } else if (cmd == "/model") {
                if (rest.empty()) {
                    print_info(ui, "Usage: /model <name>");
                    continue;
                }
                if (ensure_model_loaded(ui, client, rest)) {
                    active_model = rest;
                    rebuild_messages(messages, system_prompt);
                    print_info(ui, "switched to " + active_model + "; history cleared");
                }
            } else if (cmd == "/load") {
                if (rest.empty()) {
                    print_info(ui, "Usage: /load <name>");
                    continue;
                }
                ensure_model_loaded(ui, client, rest);
            } else if (cmd == "/unload") {
                try {
                    client.unload_model(rest);
                } catch (const std::exception& e) {
                    print_error(ui, e.what());
                }
            } else if (cmd == "/list" || cmd == "/models") {
                bool include_non_chat = false;
                bool downloaded_only = false;
                std::istringstream iss(rest);
                std::string tok;
                while (iss >> tok) {
                    if (tok == "--all" || tok == "-a")            include_non_chat = true;
                    else if (tok == "--downloaded" || tok == "-d") downloaded_only = true;
                    else {
                        print_info(ui, std::string("unknown option: ") + tok
                                          + " (accepted: --all, --downloaded)");
                        tok.clear();
                        break;
                    }
                }
                print_model_list(ui, client, active_model,
                                 include_non_chat, downloaded_only);
            } else if (cmd == "/ps") {
                try {
                    std::string body = client.make_request("/api/v1/health", "GET",
                                                           "", "", 1000, 1000);
                    json health = json::parse(body);
                    if (!health.contains("all_models_loaded") ||
                        !health["all_models_loaded"].is_array() ||
                         health["all_models_loaded"].empty()) {
                        print_info(ui, "no models loaded");
                    } else {
                        Style s(ui);
                        for (const auto& m : health["all_models_loaded"]) {
                            std::string name;
                            std::string recipe;
                            std::string device;
                            if (m.is_string()) {
                                name = m.get<std::string>();
                            } else if (m.is_object()) {
                                if (m.contains("model_name") && m["model_name"].is_string())
                                    name = m["model_name"].get<std::string>();
                                if (m.contains("recipe") && m["recipe"].is_string())
                                    recipe = m["recipe"].get<std::string>();
                                if (m.contains("device") && m["device"].is_string())
                                    device = m["device"].get<std::string>();
                            }
                            if (name.empty()) continue;

                            std::cout << "    ";
                            if (name == active_model) {
                                std::cout << s.cyan_b() << name << s.reset();
                            } else {
                                std::cout << name;
                            }
                            std::vector<std::string> tags;
                            if (name == active_model) tags.push_back("active");
                            if (!recipe.empty()) tags.push_back(recipe);
                            if (!device.empty()) tags.push_back(device);
                            if (!tags.empty()) {
                                std::cout << "  " << s.dim() << "(";
                                for (size_t i = 0; i < tags.size(); ++i) {
                                    if (i) std::cout << ", ";
                                    std::cout << tags[i];
                                }
                                std::cout << ")" << s.reset();
                            }
                            std::cout << "\n";
                        }
                    }
                } catch (const std::exception& e) {
                    print_error(ui, e.what());
                }
            } else if (cmd == "/stats") {
                if (last_turn.completion_tokens <= 0) {
                    print_info(ui, "no completed turn yet");
                } else {
                    print_stats(ui, last_turn);
                }
            } else if (cmd == "/think" || cmd == "/reasoning" || cmd == "/reason") {
                // Trim trailing whitespace from rest.
                while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t')) {
                    rest.pop_back();
                }
                if (rest.empty()) {
                    print_info(ui, std::string("reasoning is ")
                                          + reasoning_mode_label(reasoning_mode));
                } else if (rest == "on" || rest == "true" || rest == "1") {
                    reasoning_mode = ReasoningMode::On;
                    print_info(ui, "reasoning on (model may emit a thinking trace)");
                } else if (rest == "off" || rest == "false" || rest == "0" ||
                           rest == "no") {
                    reasoning_mode = ReasoningMode::Off;
                    print_info(ui, "reasoning off (suppressing thinking traces)");
                } else if (rest == "default" || rest == "auto" || rest == "reset") {
                    reasoning_mode = ReasoningMode::Default;
                    print_info(ui, "reasoning back to model default");
                } else {
                    print_info(ui, "Usage: /think [on|off|default]");
                }
            } else if (cmd == "/multiline") {
                multiline = !multiline;
                print_info(ui, std::string("multiline ") + (multiline ? "on" : "off"));
            } else {
                print_info(ui, "Unknown command: " + cmd + "  (try /help)");
            }
            continue;
        }

        // Regular user message.
        messages.push_back({{"role", "user"}, {"content", input}});

        std::string assistant_text;
        TurnResult turn = stream
            ? stream_chat_turn_capture(ui, client, active_model, messages,
                                       reasoning_mode, assistant_text)
            : blocking_chat_turn(ui, client, active_model, messages,
                                 reasoning_mode, assistant_text);

        if (turn.interrupted) {
            print_info(ui, "interrupted");
            g_interrupted = false;
        }

        if (assistant_text.empty()) {
            // Server returned no content (likely an error already printed).
            // Drop the user turn so the next attempt isn't poisoned.
            if (!messages.empty()) messages.erase(messages.size() - 1);
            continue;
        }

        messages.push_back({{"role", "assistant"}, {"content", assistant_text}});
        last_turn = turn;
        if (turn.ok) {
            print_stats(ui, turn);
            std::cout << std::endl;
        }
    }

    uninstall_interrupt_handler();
    return exit_code;
}

} // namespace lemon_cli
