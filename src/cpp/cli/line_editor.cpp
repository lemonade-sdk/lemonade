#include "lemon_cli/line_editor.h"

#include <algorithm>
#include <iostream>

#ifndef _WIN32
    #include <cerrno>
    #include <poll.h>
    #include <sys/ioctl.h>
    #include <termios.h>
    #include <unistd.h>
#endif

namespace lemon_cli {

namespace {
constexpr size_t kMaxHistory = 500;
}

std::string to_utf8(const std::u32string& text) {
    std::string out;
    for (char32_t cp : text) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

std::u32string from_utf8(const std::string& text) {
    std::u32string out;
    for (size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        if (lead >= 0x80 && lead < 0xC0) {
            ++i;
            continue;
        }
        const size_t extra = lead < 0x80 ? 0 : lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : 1;
        char32_t cp = extra == 0 ? lead : lead & (0x3F >> extra);
        for (size_t k = 1; k <= extra && i + k < text.size(); ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        }
        out += cp;
        i += extra + 1;
    }
    return out;
}

void LineEditor::add_history(const std::string& line) {
    if (line.empty() || line.find('\n') != std::string::npos) return;
    if (!history_.empty() && history_.back() == line) return;
    history_.push_back(line);
    if (history_.size() > kMaxHistory) history_.erase(history_.begin());
}

#ifdef _WIN32

RawTerminal::RawTerminal() = default;
RawTerminal::~RawTerminal() = default;
KeyEvent RawTerminal::read_key() { return {Key::Eof, 0}; }
bool RawTerminal::input_pending() const { return false; }
bool RawTerminal::available() { return false; }
int RawTerminal::width() { return 80; }
int RawTerminal::height() { return 24; }

bool LineEditor::supported() {
    return false;
}

LineEditor::Result LineEditor::read_line(const std::string& prompt, int, std::string& out,
                                         bool remember) {
    std::cout << prompt << std::flush;
    if (!std::getline(std::cin, out)) return Result::Eof;
    if (remember) add_history(out);
    return Result::Line;
}

#else

namespace {

constexpr int kEscapeWaitMs = 40;
constexpr int kEofByte = -1;
constexpr int kTimeoutByte = -2;

// 1 when input is ready, 0 on timeout.
int wait_for_input(int timeout_ms) {
    pollfd p{STDIN_FILENO, POLLIN, 0};
    while (true) {
        int r = poll(&p, 1, timeout_ms);
        if (r < 0 && errno == EINTR) continue;
        return r > 0 ? 1 : 0;
    }
}

// Returns a byte, kEofByte when stdin closed, or kTimeoutByte when `timeout_ms` (if >= 0) passes.
int read_byte(int timeout_ms = -1) {
    if (timeout_ms >= 0 && wait_for_input(timeout_ms) == 0) return kTimeoutByte;
    unsigned char c = 0;
    while (true) {
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n == 1) return c;
        if (n < 0 && errno == EINTR) continue;
        return kEofByte;
    }
}

} // namespace

bool RawTerminal::available() {
    return isatty(STDIN_FILENO) != 0 && isatty(STDOUT_FILENO) != 0;
}

RawTerminal::RawTerminal() {
    auto* saved = new termios();
    if (tcgetattr(STDIN_FILENO, saved) != 0) {
        delete saved;
        return;
    }
    termios raw = *saved;
    raw.c_iflag &= ~static_cast<tcflag_t>(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &raw) != 0) {
        delete saved;
        return;
    }
    saved_ = saved;
    ok_ = true;
}

RawTerminal::~RawTerminal() {
    if (!ok_) return;
    auto* saved = static_cast<termios*>(saved_);
    tcsetattr(STDIN_FILENO, TCSADRAIN, saved);
    delete saved;
}

bool RawTerminal::input_pending() const {
    return wait_for_input(0) != 0;
}

int RawTerminal::width() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    return 80;
}

int RawTerminal::height() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return ws.ws_row;
    return 24;
}

KeyEvent RawTerminal::read_key() {
    while (true) {
        const int c = read_byte();
        if (c == kEofByte) return {Key::Eof, 0};

        switch (c) {
            case '\r':
            case '\n': return {Key::Enter, 0};
            case 0x03: return {Key::Interrupt, 0};
            case 0x04: return {Key::CtrlD, 0};
            case 0x7f:
            case 0x08: return {Key::Backspace, 0};
            case 0x01: return {Key::CtrlA, 0};
            case 0x02: return {Key::CtrlB, 0};
            case 0x05: return {Key::CtrlE, 0};
            case 0x06: return {Key::CtrlF, 0};
            case 0x0b: return {Key::CtrlK, 0};
            case 0x0e: return {Key::CtrlN, 0};
            case 0x10: return {Key::CtrlP, 0};
            case 0x15: return {Key::CtrlU, 0};
            case 0x17: return {Key::CtrlW, 0};
            default: break;
        }

        if (c == 0x1b) {
            const int next = read_byte(kEscapeWaitMs);
            if (next == kTimeoutByte || next == kEofByte) return {Key::Escape, 0};
            if (next != '[' && next != 'O') continue;

            std::string params;
            int final_byte = 0;
            while (true) {
                final_byte = read_byte(kEscapeWaitMs);
                if (final_byte < 0) break;
                if (final_byte >= 0x40 && final_byte <= 0x7e) break;
                params += static_cast<char>(final_byte);
            }
            const bool modified = params.find(';') != std::string::npos;
            switch (final_byte) {
                case 'A': return {Key::Up, 0};
                case 'B': return {Key::Down, 0};
                case 'C': return {modified ? Key::WordRight : Key::Right, 0};
                case 'D': return {modified ? Key::WordLeft : Key::Left, 0};
                case 'H': return {Key::Home, 0};
                case 'F': return {Key::End, 0};
                case '~':
                    if (params == "1" || params == "7") return {Key::Home, 0};
                    if (params == "4" || params == "8") return {Key::End, 0};
                    if (params == "3") return {Key::Delete, 0};
                    if (params == "5") return {Key::PageUp, 0};
                    if (params == "6") return {Key::PageDown, 0};
                    break;
                default: break;
            }
            continue;
        }

        if (c >= 0x80 && c < 0xC0) continue;
        if (c < 0x20) continue;

        const size_t extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        std::string bytes(1, static_cast<char>(c));
        for (size_t k = 0; k < extra; ++k) {
            const int more = read_byte();
            if (more < 0) break;
            bytes += static_cast<char>(more);
        }
        const std::u32string decoded = from_utf8(bytes);
        if (decoded.empty()) continue;
        return {Key::Char, decoded[0]};
    }
}

bool LineEditor::supported() {
    return RawTerminal::available();
}

LineEditor::Result LineEditor::read_line(const std::string& prompt, int prompt_width,
                                         std::string& out, bool remember) {
    out.clear();
    RawTerminal terminal;
    if (!terminal.ok()) {
        std::cout << prompt << std::flush;
        if (!std::getline(std::cin, out)) return Result::Eof;
        if (remember) add_history(out);
        return Result::Line;
    }

    std::u32string buf;
    std::u32string stash;
    size_t cur = 0;
    size_t hist_pos = history_.size();

    auto is_space = [](char32_t c) { return c == U' ' || c == U'\t'; };
    auto redraw = [&]() {
        const int cols = RawTerminal::width();
        const size_t avail = static_cast<size_t>(std::max(1, cols - prompt_width - 1));
        const size_t start = cur >= avail ? cur - avail + 1 : 0;
        const size_t count = std::min(buf.size() - start, avail);
        const size_t col = static_cast<size_t>(prompt_width) + (cur - start);
        std::string frame = "\r" + prompt + to_utf8(buf.substr(start, count)) + "\033[K\r";
        if (col > 0) frame += "\033[" + std::to_string(col) + "C";
        std::cout << frame << std::flush;
    };
    auto set_buffer = [&](const std::u32string& text) {
        buf = text;
        cur = buf.size();
    };
    auto word_left = [&]() {
        while (cur > 0 && is_space(buf[cur - 1])) --cur;
        while (cur > 0 && !is_space(buf[cur - 1])) --cur;
    };
    auto word_right = [&]() {
        while (cur < buf.size() && is_space(buf[cur])) ++cur;
        while (cur < buf.size() && !is_space(buf[cur])) ++cur;
    };
    auto history_prev = [&]() {
        if (hist_pos == 0) return;
        if (hist_pos == history_.size()) stash = buf;
        --hist_pos;
        set_buffer(from_utf8(history_[hist_pos]));
    };
    auto history_next = [&]() {
        if (hist_pos >= history_.size()) return;
        ++hist_pos;
        set_buffer(hist_pos == history_.size() ? stash : from_utf8(history_[hist_pos]));
    };
    auto finish = [&](Result result) {
        std::cout << "\r\n" << std::flush;
        if (result == Result::Line) {
            out = to_utf8(buf);
            if (remember) add_history(out);
        }
        return result;
    };

    redraw();
    while (true) {
        const KeyEvent ev = terminal.read_key();
        switch (ev.key) {
            case Key::Eof: return finish(buf.empty() ? Result::Eof : Result::Line);
            case Key::Enter: return finish(Result::Line);
            case Key::Interrupt: return finish(Result::Interrupted);
            case Key::CtrlD:
                if (buf.empty()) return finish(Result::Eof);
                if (cur < buf.size()) buf.erase(cur, 1);
                break;
            case Key::Delete:
                if (cur < buf.size()) buf.erase(cur, 1);
                break;
            case Key::Backspace:
                if (cur > 0) buf.erase(--cur, 1);
                break;
            case Key::Home:
            case Key::CtrlA: cur = 0; break;
            case Key::End:
            case Key::CtrlE: cur = buf.size(); break;
            case Key::Left:
            case Key::CtrlB:
                if (cur > 0) --cur;
                break;
            case Key::Right:
            case Key::CtrlF:
                if (cur < buf.size()) ++cur;
                break;
            case Key::WordLeft: word_left(); break;
            case Key::WordRight: word_right(); break;
            case Key::CtrlK: buf.erase(cur); break;
            case Key::CtrlU:
            case Key::Escape:
                buf.clear();
                cur = 0;
                break;
            case Key::CtrlW: {
                const size_t end = cur;
                word_left();
                buf.erase(cur, end - cur);
                break;
            }
            case Key::Up:
            case Key::CtrlP: history_prev(); break;
            case Key::Down:
            case Key::CtrlN: history_next(); break;
            case Key::Char:
                buf.insert(cur, 1, ev.ch);
                ++cur;
                break;
            default: break;
        }

        if (!terminal.input_pending()) redraw();
    }
}

#endif

} // namespace lemon_cli
