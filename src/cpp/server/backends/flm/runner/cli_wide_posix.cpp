/*
*  Copyright (c) 2025 by Contributors
*  \file cli_wide_posix.cpp
*  \brief CLI interactive input implementation for non-Windows platforms
*  \author FastFlowLM Team
*  \date 2026-02-03
*  \version 0.9.24
*/
#include "cli_wide.hpp"
#include <iostream>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <thread>
#ifndef _WIN32
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif
#ifdef FASTFLOWLM_USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

#ifndef _WIN32

CLIWide::CLIWide() = default;

CLIWide::~CLIWide() = default;

namespace {
std::string join_lines(const std::vector<std::string>& lines) {
    std::string merged;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            merged += "\n";
        }
        merged += lines[i];
    }
    return merged;
}

bool stdin_has_pending_input(int wait_ms) {
    pollfd pfd{};
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;

    int ready = poll(&pfd, 1, wait_ms);
    if (ready <= 0 || !(pfd.revents & POLLIN)) {
        return false;
    }

    int pending_bytes = 0;
    if (ioctl(STDIN_FILENO, FIONREAD, &pending_bytes) == 0) {
        return pending_bytes > 0;
    }

    return true;
}

bool wait_for_pending_input(int checks, int delay_ms) {
    for (int i = 0; i < checks; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        if (stdin_has_pending_input(0)) {
            return true;
        }
    }
    return false;
}

void trim_trailing_empty_lines(std::vector<std::string>& lines) {
    while (lines.size() > 1 && lines.back().empty()) {
        lines.pop_back();
    }
}

void pop_last_utf8_char(std::string& text) {
    if (text.empty()) {
        return;
    }

    size_t pos = text.size() - 1;
    while (pos > 0 && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80) {
        --pos;
    }
    text.erase(pos);
}

bool read_single_byte(unsigned char& out_byte) {
    ssize_t count = read(STDIN_FILENO, &out_byte, 1);
    return count == 1;
}

std::string read_escape_sequence() {
    std::string seq;
    seq.push_back('\x1b');

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(40);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!stdin_has_pending_input(0)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        unsigned char next = 0;
        if (!read_single_byte(next)) {
            break;
        }

        seq.push_back(static_cast<char>(next));
        if ((next >= 'A' && next <= 'Z') || (next >= 'a' && next <= 'z') || next == '~') {
            break;
        }
    }

    return seq;
}

void redraw_input_line(const std::string& prompt, const std::string& line) {
    std::cout << "\r" << prompt << line << "\x1b[K" << std::flush;
}

class RawTerminalGuard {
public:
    RawTerminalGuard() {
        active = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &original) == 0;
        if (!active) {
            return;
        }

        termios raw = original;
        raw.c_lflag &= static_cast<unsigned long>(~(ICANON | ECHO));
        raw.c_iflag &= static_cast<unsigned long>(~(IXON | ICRNL));
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    ~RawTerminalGuard() {
        if (active) {
            tcsetattr(STDIN_FILENO, TCSANOW, &original);
        }
    }

    bool is_active() const {
        return active;
    }

private:
    termios original{};
    bool active = false;
};
}

std::string CLIWide::get_interactive_input() {
#ifdef FASTFLOWLM_USE_READLINE
    std::vector<std::string> lines;
    bool paste_mode = false;

    while (true) {
        const char* prompt = lines.empty() ? ">>> " : "... ";
        char* line = readline(prompt);
        if (!line) {
            if (lines.empty()) {
                return "/bye";
            }
            std::cout << std::endl;
            break;
        }

        std::string input(line);
        free(line);

        if (lines.empty() && !input.empty() && input[0] == '/') {
            add_to_history(input);
            return input;
        }

        bool has_continuation = !input.empty() && input.back() == '\\';
        if (has_continuation) {
            input.pop_back();
        }

        lines.push_back(input);

        if (has_continuation) {
            continue;
        }

        if (paste_mode) {
            if (wait_for_pending_input(5, 15)) {
                continue;
            }
            paste_mode = false;
            trim_trailing_empty_lines(lines);
            break;
        }

        if (wait_for_pending_input(1, 30)) {
            paste_mode = true;
            continue;
        }

        if (lines.size() == 1 && lines[0].empty()) {
            return std::string();
        }

        trim_trailing_empty_lines(lines);
        break;
    }

    std::string merged = join_lines(lines);
    if (!merged.empty()) {
        add_to_history(merged);
    }
    return merged;
#else
    RawTerminalGuard raw_guard;
    if (!raw_guard.is_active()) {
        std::string fallback;
        std::cout << ">>> " << std::flush;
        if (!std::getline(std::cin, fallback)) {
            return "/bye";
        }
        if (!fallback.empty()) {
            add_to_history(fallback);
        }
        return fallback;
    }

    std::vector<std::string> lines;
    std::string current_line;
    bool paste_mode = false;
    std::string prompt = ">>> ";
    std::cout << prompt << std::flush;

    while (true) {
        unsigned char byte = 0;
        if (!read_single_byte(byte)) {
            std::cout << std::endl;
            if (lines.empty() && current_line.empty()) {
                return "/bye";
            }
            break;
        }

        if (byte == 0x1b) {
            read_escape_sequence();
            continue;
        }

        if (byte == 0x03) {
            std::cout << "^C\n";
            return "/bye";
        }

        if (byte == 0x04) {
            if (lines.empty() && current_line.empty()) {
                std::cout << std::endl;
                return "/bye";
            }
            break;
        }

        if (byte == 0x7f || byte == 0x08) {
            if (!current_line.empty()) {
                pop_last_utf8_char(current_line);
                redraw_input_line(prompt, current_line);
            }
            continue;
        }

        if (byte == '\r' || byte == '\n') {
            bool has_continuation = !current_line.empty() && current_line.back() == '\\';
            if (has_continuation) {
                current_line.pop_back();
            }

            if (has_continuation) {
                lines.push_back(current_line);
                current_line.clear();
                prompt = "... ";
                std::cout << "\n" << prompt << std::flush;
                continue;
            }

            if (paste_mode) {
                if (wait_for_pending_input(5, 15)) {
                    lines.push_back(current_line);
                    current_line.clear();
                    prompt = "... ";
                    std::cout << "\n" << prompt << std::flush;
                    continue;
                }

                if (!current_line.empty()) {
                    lines.push_back(current_line);
                }
                current_line.clear();
                paste_mode = false;
                std::cout << std::endl;
                break;
            }

            if (wait_for_pending_input(2, 20)) {
                paste_mode = true;
                lines.push_back(current_line);
                current_line.clear();
                prompt = "... ";
                std::cout << "\n" << prompt << std::flush;
                continue;
            }

            if (lines.empty() && current_line.empty()) {
                std::cout << std::endl;
                return std::string();
            }

            if (lines.empty() && !current_line.empty() && current_line[0] == '/') {
                std::string command = current_line;
                std::cout << std::endl;
                add_to_history(command);
                return command;
            }

            lines.push_back(current_line);
            current_line.clear();
            std::cout << std::endl;
            break;
        }

        current_line.push_back(static_cast<char>(byte));
        std::cout << static_cast<char>(byte) << std::flush;
    }

    trim_trailing_empty_lines(lines);
    std::string merged = join_lines(lines);
    if (!merged.empty()) {
        add_to_history(merged);
    }
    return merged;
#endif
}

void CLIWide::add_to_history(const std::string& command) {
    if (command.empty()) return;

    auto it = std::find(command_history.begin(), command_history.end(), command);
    if (it != command_history.end()) {
        command_history.erase(it);
    }

    command_history.push_back(command);
    if (command_history.size() > max_history_size) {
        command_history.pop_front();
    }
    history_index = -1;

#ifdef FASTFLOWLM_USE_READLINE
    HIST_ENTRY* latest = (history_length > 0) ? history_get(history_length) : nullptr;
    if (!latest || !latest->line || command != latest->line) {
        add_history(command.c_str());
    }
#endif
}

#endif
