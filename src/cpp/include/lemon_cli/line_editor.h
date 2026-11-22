#pragma once

#include <string>
#include <vector>

namespace lemon_cli {

enum class Key {
    Unknown,
    Char,
    Enter,
    Backspace,
    Delete,
    Up,
    Down,
    Left,
    Right,
    WordLeft,
    WordRight,
    Home,
    End,
    PageUp,
    PageDown,
    Escape,
    Interrupt,
    Eof,
    CtrlA,
    CtrlB,
    CtrlD,
    CtrlE,
    CtrlF,
    CtrlK,
    CtrlN,
    CtrlP,
    CtrlU,
    CtrlW,
};

struct KeyEvent {
    Key key = Key::Unknown;
    char32_t ch = 0;  // Set for Key::Char.
};

// Puts a POSIX terminal into raw mode while alive and decodes key presses. On Windows, or when
// stdin or stdout is not a terminal, ok() is false and nothing is changed.
class RawTerminal {
public:
    RawTerminal();
    ~RawTerminal();
    RawTerminal(const RawTerminal&) = delete;
    RawTerminal& operator=(const RawTerminal&) = delete;

    bool ok() const { return ok_; }
    KeyEvent read_key();
    bool input_pending() const;

    static bool available();
    static int width();
    static int height();

private:
    bool ok_ = false;
    void* saved_ = nullptr;
};

std::string to_utf8(const std::u32string& text);
std::u32string from_utf8(const std::string& text);

// A small single-line editor for terminals: arrow-key history, cursor movement and the usual
// Ctrl shortcuts. It is only available on POSIX when both stdin and stdout are terminals; the
// Windows console already provides its own line history.
class LineEditor {
public:
    enum class Result { Line, Eof, Interrupted };

    static bool supported();

    // `prompt` may contain escape sequences; `prompt_width` is its visible width.
    // Ctrl-C ends the read with Interrupted. `remember` adds a non-empty line to the history.
    Result read_line(const std::string& prompt, int prompt_width, std::string& out,
                     bool remember = true);

    void add_history(const std::string& line);

private:
    std::vector<std::string> history_;
};

} // namespace lemon_cli
