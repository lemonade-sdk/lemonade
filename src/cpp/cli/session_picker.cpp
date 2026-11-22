#include "lemon_cli/session_picker.h"

#include "lemon_cli/line_editor.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iostream>

namespace lemon_cli {

namespace {

constexpr int kMaxWidth = 110;
constexpr int kReservedRows = 9;

// Cuts or pads to `width` code points. Wide characters are counted as one column.
std::string fit(const std::string& text, size_t width) {
    std::u32string cps = from_utf8(text);
    if (cps.size() > width) {
        cps.resize(width > 0 ? width - 1 : 0);
        if (width > 0) cps += U'\u2026';
    }
    cps.resize(width, U' ');
    return to_utf8(cps);
}

std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

} // namespace

PickerResult run_session_picker(const std::vector<PickerRow>& rows, const PickerStyle& style,
                                const std::string& initial_filter,
                                const std::string& selected_id) {
    PickerResult result;
    RawTerminal terminal;
    if (!terminal.ok()) return result;

    const int term_width = RawTerminal::width();
    const int term_height = RawTerminal::height();
    const size_t content_width = static_cast<size_t>(std::max(40, std::min(term_width, kMaxWidth) - 4));
    const size_t max_rows = static_cast<size_t>(std::clamp(term_height - kReservedRows, 3, 12));

    std::string filter = initial_filter;
    bool filtering = false;
    bool confirming = false;
    size_t sel = 0;
    size_t top = 0;
    int drawn = 0;

    auto visible = [&]() {
        std::vector<const PickerRow*> out;
        const std::string needle = lowered(filter);
        for (const auto& row : rows) {
            if (needle.empty() || row.haystack.find(needle) != std::string::npos) {
                out.push_back(&row);
            }
        }
        return out;
    };

    {
        auto list = visible();
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i]->id == selected_id) sel = i;
        }
    }

    const char* reset = style.ansi ? "\033[0m" : "";
    const char* dim = style.ansi ? "\033[2m" : "";
    const char* title = style.ansi ? "\033[1;38;5;221m" : "";
    const char* highlight = style.ansi ? "\033[1;38;5;87m\033[48;5;236m" : "";
    const char* warn = style.ansi ? "\033[1;38;5;177m" : "";
    const char* sep = style.unicode ? " \xc2\xb7 " : " | ";
    const std::string dash = style.unicode ? "\xe2\x94\x80" : "-";
    const std::string marker = style.unicode ? "\xe2\x96\xb8 " : "> ";

    auto build_frame = [&]() {
        const auto list = visible();
        if (!list.empty() && sel >= list.size()) sel = list.size() - 1;
        if (sel < top) top = sel;
        if (sel >= top + max_rows) top = sel - max_rows + 1;

        std::vector<std::string> lines;
        std::string header = "  " + std::string(title) + "Sessions" + reset + " " + dim +
                             std::to_string(list.size()) + " of " + std::to_string(rows.size());
        if (!filter.empty() && !filtering) header += "   filter: " + filter;
        header += reset;
        lines.push_back(header);

        std::string rule;
        for (size_t i = 0; i < content_width; ++i) rule += dash;
        lines.push_back("  " + std::string(dim) + rule + reset);

        const bool show_model = content_width >= 100;
        const size_t fixed = 2 + 16 + 2 + 7 + 2 + 8 + (show_model ? 20 : 0);
        const size_t label_width = content_width > fixed + 10 ? std::min<size_t>(content_width - fixed, 40) : 10;
        for (size_t i = 0; i < max_rows; ++i) {
            const size_t index = top + i;
            if (index >= list.size()) {
                lines.push_back("");
                continue;
            }
            const PickerRow& row = *list[index];
            char msgs[16];
            std::snprintf(msgs, sizeof(msgs), "%3zu msg", row.msgs);
            const bool selected = index == sel;
            std::string text = (selected ? marker : "  ") +
                               fit((row.current ? "* " : "") + row.label, label_width) + "  " +
                               fit(row.when, 16) + "  " + msgs +
                               (show_model ? "  " + fit(row.model, 18) : std::string()) + "  " +
                               fit(row.id, 8);
            text = fit(text, content_width);
            lines.push_back("  " + std::string(selected ? highlight : "") + text + reset);
        }

        lines.push_back("  " + std::string(dim) + rule + reset);
        if (list.empty()) {
            lines.push_back("  " + std::string(dim) + "no session matches" + reset);
            lines.push_back("");
        } else {
            const PickerRow& cur = *list[sel];
            lines.push_back("  " + std::string(dim) + "first " + reset +
                            fit(cur.first, content_width - 6));
            lines.push_back("  " + std::string(dim) + "last  " + reset +
                            fit(cur.last, content_width - 6));
        }
        lines.push_back("");

        if (confirming && !list.empty()) {
            lines.push_back("  " + std::string(warn) + "Delete '" + list[sel]->label +
                            "'? [y/N]" + reset);
        } else if (filtering) {
            lines.push_back("  " + std::string(title) + "/" + reset + filter + "_   " + dim +
                            "Enter keep" + sep + "Esc clear" + reset);
        } else {
            const std::string arrows = style.unicode ? "\xe2\x86\x91\xe2\x86\x93 move" : "Up/Down move";
            lines.push_back("  " + std::string(dim) +
                            fit(arrows + sep + "Enter open" + sep + "/ filter" + sep +
                                    "d delete" + sep + "r rename" + sep + "Esc cancel",
                                content_width) + reset);
        }
        return lines;
    };

    auto draw = [&]() {
        std::string out;
        if (drawn > 0) out += "\033[" + std::to_string(drawn) + "A";
        out += "\r";
        const auto lines = build_frame();
        for (const auto& line : lines) out += line + "\033[K\n";
        drawn = static_cast<int>(lines.size());
        std::cout << out << std::flush;
    };

    auto close = [&]() {
        std::string out;
        if (drawn > 0) out += "\033[" + std::to_string(drawn) + "A";
        out += "\r\033[J\033[?25h";
        std::cout << out << std::flush;
    };

    auto finish = [&](PickerResult::Kind kind) {
        const auto list = visible();
        result.kind = kind;
        result.filter = filter;
        if (!list.empty()) result.id = list[std::min(sel, list.size() - 1)]->id;
        close();
        return result;
    };

    std::cout << "\033[?25l" << std::flush;
    draw();
    while (true) {
        const KeyEvent ev = terminal.read_key();
        const size_t count = visible().size();

        if (confirming) {
            confirming = false;
            if (ev.key == Key::Char && (ev.ch == U'y' || ev.ch == U'Y')) {
                return finish(PickerResult::Kind::Delete);
            }
        } else if (filtering) {
            switch (ev.key) {
                case Key::Enter: filtering = false; break;
                case Key::Escape:
                    filter.clear();
                    filtering = false;
                    break;
                case Key::Backspace:
                    if (!filter.empty()) {
                        std::u32string cps = from_utf8(filter);
                        cps.pop_back();
                        filter = to_utf8(cps);
                    }
                    sel = 0;
                    break;
                case Key::CtrlU:
                    filter.clear();
                    sel = 0;
                    break;
                case Key::Char:
                    filter += to_utf8(std::u32string(1, ev.ch));
                    sel = 0;
                    break;
                case Key::Interrupt:
                case Key::Eof: return finish(PickerResult::Kind::Cancel);
                default: break;
            }
        } else {
            switch (ev.key) {
                case Key::Up:
                case Key::CtrlP:
                    if (sel > 0) --sel;
                    break;
                case Key::Down:
                case Key::CtrlN:
                    if (sel + 1 < count) ++sel;
                    break;
                case Key::PageUp: sel = sel > max_rows ? sel - max_rows : 0; break;
                case Key::PageDown: sel = std::min(sel + max_rows, count > 0 ? count - 1 : 0); break;
                case Key::Home: sel = 0; break;
                case Key::End: sel = count > 0 ? count - 1 : 0; break;
                case Key::Enter:
                    if (count > 0) return finish(PickerResult::Kind::Open);
                    break;
                case Key::Escape:
                    if (!filter.empty()) {
                        filter.clear();
                        sel = 0;
                    } else {
                        return finish(PickerResult::Kind::Cancel);
                    }
                    break;
                case Key::Interrupt:
                case Key::Eof: return finish(PickerResult::Kind::Cancel);
                case Key::Char:
                    if (ev.ch == U'/') filtering = true;
                    else if (ev.ch == U'q') return finish(PickerResult::Kind::Cancel);
                    else if (ev.ch == U'd' && count > 0) confirming = true;
                    else if (ev.ch == U'r' && count > 0) return finish(PickerResult::Kind::Rename);
                    break;
                default: break;
            }
        }
        if (!terminal.input_pending()) draw();
    }
}

} // namespace lemon_cli
