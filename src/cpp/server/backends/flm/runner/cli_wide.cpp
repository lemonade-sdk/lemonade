/*
*  Copyright (c) 2025 by Contributors
*  \file cli_wide.cpp
*  \brief CLI interactive input implementation using Windows Console API
*  \author FastFlowLM Team
*  \date 2025-06-24
*  \version 0.9.24
*/
#include "cli_wide.hpp"
#include <chrono>
#include <cstdint>
#include <thread>
#include <iostream>
#include <algorithm>
#include <locale>
#include <codecvt>

#ifdef _WIN32
/// \brief Constructor - Initialize console
CLIWide::CLIWide() {
    // Get console handles
    hConsoleInput = GetStdHandle(STD_INPUT_HANDLE);
    hConsoleOutput = GetStdHandle(STD_OUTPUT_HANDLE);

    // Store original modes
    GetConsoleMode(hConsoleInput, &originalInputMode);
    GetConsoleMode(hConsoleOutput, &originalOutputMode);

    // Set input mode for raw input to capture all Unicode characters
    // But keep ENABLE_PROCESSED_INPUT to handle Ctrl+C properly
    DWORD inputMode = ENABLE_PROCESSED_INPUT | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT | ENABLE_EXTENDED_FLAGS;
    SetConsoleMode(hConsoleInput, inputMode);

    // Set output mode for UTF-8
    DWORD outputMode = ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hConsoleOutput, outputMode);

    // Set console to UTF-8 mode
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

/// \brief Destructor - Restore console modes
CLIWide::~CLIWide() {
    // Restore original console modes
    SetConsoleMode(hConsoleInput, originalInputMode);
    SetConsoleMode(hConsoleOutput, originalOutputMode);
}

/// \brief Convert wide char to UTF-8
std::string CLIWide::wchar_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();

    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    if (size_needed == 0) {
        return std::string();
    }

    std::string strTo(size_needed, 0);
    int result = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    if (result == 0) {
        return std::string();
    }

    return strTo;
}

/// \brief Convert UTF-8 to wide char
std::wstring CLIWide::utf8_to_wchar(const std::string& str) {
    if (str.empty()) return std::wstring();

    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

/// \brief Read console input with timeout
bool CLIWide::read_console_input(INPUT_RECORD& inputRecord) {
    DWORD eventsRead;
    return ReadConsoleInput(hConsoleInput, &inputRecord, 1, &eventsRead) && eventsRead > 0;
}



/// \brief Check if this is a paste operation
bool CLIWide::is_paste_operation() {
    // Check if there are more input events pending
    DWORD numEvents = 0;
    GetNumberOfConsoleInputEvents(hConsoleInput, &numEvents);

    // Consider it a paste operation if there are multiple events
    // For paste operations, there are typically many events queued up
    // Be more lenient - even 2 events could indicate a paste operation
    return numEvents > 1;  // Changed back to > 1 for better sensitivity
}

/// \brief Get interactive input with arrow key support
std::string CLIWide::get_interactive_input() {
    InputState state;
    state.lines.push_back("");  // Start with one empty line
    state.is_wrapped_line.push_back(false);  // First line is not wrapped
    state.utf8_buffer.clear();  // Initialize UTF-8 buffer

    bool receiving_unicode_char = false;

    while (true) {
        print_prompt_and_line(state);

        while (true) {
            // Use ReadConsoleInput for all input
            INPUT_RECORD inputRecord;
            if (!read_console_input(inputRecord)) continue;

            // Get key event record
            KEY_EVENT_RECORD keyEvent;
            if (inputRecord.EventType == KEY_EVENT) {
                keyEvent = inputRecord.Event.KeyEvent;
            }
            else{
                continue;
            }

            // Only handle key down events
            if ((!keyEvent.bKeyDown) && (keyEvent.wVirtualKeyCode != 0x12)) continue;

            KeyAction action = KeyAction::CONTINUE;

            // Handle different key types
            if (keyEvent.wVirtualKeyCode == VK_UP || keyEvent.wVirtualKeyCode == VK_DOWN ||
                keyEvent.wVirtualKeyCode == VK_LEFT || keyEvent.wVirtualKeyCode == VK_RIGHT) {
                action = handle_arrow_keys(keyEvent, state);
            } else if (keyEvent.wVirtualKeyCode == VK_HOME || keyEvent.wVirtualKeyCode == VK_END ||
                       keyEvent.wVirtualKeyCode == VK_DELETE) {
                action = handle_navigation_keys(keyEvent, state);
            } else if (keyEvent.wVirtualKeyCode == VK_F1 || keyEvent.wVirtualKeyCode == VK_F2 ||
                       keyEvent.wVirtualKeyCode == VK_F3 || keyEvent.wVirtualKeyCode == VK_F4 ||
                       keyEvent.wVirtualKeyCode == VK_F5 || keyEvent.wVirtualKeyCode == VK_F6 ||
                       keyEvent.wVirtualKeyCode == VK_F7 || keyEvent.wVirtualKeyCode == VK_F8 ||
                       keyEvent.wVirtualKeyCode == VK_F9 || keyEvent.wVirtualKeyCode == VK_F10 ||
                       keyEvent.wVirtualKeyCode == VK_F11 || keyEvent.wVirtualKeyCode == VK_F12) {
                action = handle_extended_key(keyEvent, state);
            } else {
                action = handle_regular_key(keyEvent, state);
            }

            switch (action) {
                case KeyAction::CONTINUE:
                    continue;
                case KeyAction::NEW_LINE:
                    print_prompt_and_line(state);
                    continue;
                case KeyAction::BREAK_INNER_LOOP:
                    break;
                case KeyAction::BREAK_OUTER_LOOP:
                    goto outer_loop_end;
                case KeyAction::RETURN_INPUT:
                    // Safety check: ensure vectors stay in sync
                    if (state.lines.size() != state.is_wrapped_line.size()) {
                        size_t min_size = (state.lines.size() < state.is_wrapped_line.size()) ? state.lines.size() : state.is_wrapped_line.size();
                        state.lines.resize(min_size);
                        state.is_wrapped_line.resize(min_size);
                    }

                    // Assemble all lines into final output
                    std::string full_input;
                    for (size_t i = 0; i < state.lines.size(); i++) {
                        if (i > 0) {
                            // Only add \n if this line was NOT created by automatic wrapping
                            if (!state.is_wrapped_line[i]) {
                                full_input += "\n";
                            }
                        }
                        full_input += state.lines[i];
                    }
                    return full_input;
            }

            if (action == KeyAction::BREAK_INNER_LOOP) break;
        }

        if (!should_continue_input(state)) break;
    }

outer_loop_end:
    // Clean up multiple empty lines at the end before assembling
    while (state.lines.size() > 1 && state.lines.back().empty()) {
        state.lines.pop_back();
        state.is_wrapped_line.pop_back();
    }

    // Safety check: ensure vectors stay in sync
    if (state.lines.size() != state.is_wrapped_line.size()) {
        // If they get out of sync, fix it by truncating to the smaller size
        size_t min_size = (state.lines.size() < state.is_wrapped_line.size()) ? state.lines.size() : state.is_wrapped_line.size();
        state.lines.resize(min_size);
        state.is_wrapped_line.resize(min_size);
    }

    // Assemble all lines into final output
    std::string full_input;
    for (size_t i = 0; i < state.lines.size(); i++) {
        if (i > 0) {
            // Only add \n if this line was NOT created by automatic wrapping
            if (!state.is_wrapped_line[i]) {
                full_input += "\n";
            }
        }
        full_input += state.lines[i];
    }



    return full_input;
}

/// \brief Add command to history
void CLIWide::add_to_history(const std::string& command) {
    if (command.empty()) return;

    // Remove duplicate if it exists
    auto it = std::find(command_history.begin(), command_history.end(), command);
    if (it != command_history.end()) {
        command_history.erase(it);
    }

    // Add to end
    command_history.push_back(command);

    // Limit history size
    while (command_history.size() > max_history_size) {
        command_history.pop_front();
    }

    // Reset history navigation index
    history_index = -1;
}

/// \brief Move cursor to specific column
void CLIWide::move_cursor_to_column(int column) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
        COORD pos = {static_cast<SHORT>(column), csbi.dwCursorPosition.Y};
        SetConsoleCursorPosition(hConsoleOutput, pos);
    }
}

/// \brief Clear line from cursor to end
void CLIWide::clear_line_from_cursor() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
        DWORD written;
        int chars_to_clear = csbi.dwSize.X - csbi.dwCursorPosition.X;
        FillConsoleOutputCharacter(hConsoleOutput, ' ', chars_to_clear, csbi.dwCursorPosition, &written);
        FillConsoleOutputAttribute(hConsoleOutput, csbi.wAttributes, chars_to_clear, csbi.dwCursorPosition, &written);
    }
}

/// \brief Set cursor position to specific column
void CLIWide::set_cursor_position(int column) {
    move_cursor_to_column(column);
}

/// \brief Get display width of UTF-8 string
size_t CLIWide::get_utf8_display_width(const std::string& str) {
    size_t width = 0;
    for (size_t i = 0; i < str.length(); ) {
        unsigned char c = str[i];
        if ((c & 0x80) == 0) {
            // ASCII character
            width += 1;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            // 2-byte UTF-8 sequence
            width += 1;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            // 3-byte UTF-8 sequence (might be wide characters)
            width += 2;
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            // 4-byte UTF-8 sequence
            width += 1;
            i += 4;
        } else {
            // Invalid UTF-8, skip
            i += 1;
        }
    }
    return width;
}

/// \brief Get UTF-8 character boundaries around cursor position
std::pair<size_t, size_t> CLIWide::get_utf8_char_boundaries(const std::string& str, size_t cursor_pos) {
    if (cursor_pos == 0) return {0, 0};
    if (cursor_pos >= str.length()) return {str.length(), str.length()};

    // Find start of the character that cursor_pos is in or just after
    size_t char_start = cursor_pos;

    // If we're in the middle of a multi-byte character, find its start
    while (char_start > 0 && (static_cast<unsigned char>(str[char_start]) & 0xC0) == 0x80) {
        char_start--;
    }

    // Find end of the character starting at char_start
    size_t char_end = char_start;
    if (char_end < str.length()) {
        unsigned char c = str[char_end];
        if ((c & 0x80) == 0) {
            char_end += 1;  // ASCII
        } else if ((c & 0xE0) == 0xC0) {
            char_end += 2;  // 2-byte UTF-8
        } else if ((c & 0xF0) == 0xE0) {
            char_end += 3;  // 3-byte UTF-8
        } else if ((c & 0xF8) == 0xF0) {
            char_end += 4;  // 4-byte UTF-8
        } else {
            char_end += 1;  // Invalid, treat as single byte
        }
    }

    // Ensure we don't exceed string bounds
    if (char_end > str.length()) {
        char_end = str.length();
    }

    return {char_start, char_end};
}

/// \brief Visualize tabs as "... " for display
std::string CLIWide::visualize_tabs(const std::string& str) {
    std::string result;
    for (size_t i = 0; i < str.length(); ) {
        unsigned char c = str[i];
        if (c == '\t') {
            result += "... ";
            i += 1;
        } else if ((c & 0x80) == 0) {
            // ASCII character
            result += c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            // 2-byte UTF-8 sequence
            if (i + 1 < str.length()) {
                result += str.substr(i, 2);
                i += 2;
            } else {
                result += c; // Incomplete sequence, treat as single byte
                i += 1;
            }
        } else if ((c & 0xF0) == 0xE0) {
            // 3-byte UTF-8 sequence
            if (i + 2 < str.length()) {
                result += str.substr(i, 3);
                i += 3;
            } else {
                result += c; // Incomplete sequence, treat as single byte
                i += 1;
            }
        } else if ((c & 0xF8) == 0xF0) {
            // 4-byte UTF-8 sequence
            if (i + 3 < str.length()) {
                result += str.substr(i, 4);
                i += 4;
            } else {
                result += c; // Incomplete sequence, treat as single byte
                i += 1;
            }
        } else {
            // Invalid UTF-8, treat as single byte
            result += c;
            i += 1;
        }
    }
    return result;
}

/// \brief Get visual cursor position accounting for tab visualization
size_t CLIWide::get_visual_cursor_position(const std::string& str, size_t cursor_pos) {
    size_t visual_pos = 0;
    for (size_t i = 0; i < cursor_pos && i < str.length(); ) {
        unsigned char c = str[i];
        if (c == '\t') {
            visual_pos += 4; // "... " is 4 characters
            i += 1;
        } else if ((c & 0x80) == 0) {
            // ASCII character
            visual_pos += 1;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            // 2-byte UTF-8 sequence
            visual_pos += 1;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            // 3-byte UTF-8 sequence (might be wide characters)
            visual_pos += 2;
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            // 4-byte UTF-8 sequence
            visual_pos += 1;
            i += 4;
        } else {
            // Invalid UTF-8, treat as single byte
            visual_pos += 1;
            i += 1;
        }
    }
    return visual_pos;
}

/// \brief Print prompt and current line
void CLIWide::print_prompt_and_line(const InputState& state) {
    bool is_first = (state.current_line_index == 0);
    std::cout << (is_first ? ">>> " : "... ");
    std::cout.flush();

    // For fresh input, start with cursor at end
    if (state.lines[state.current_line_index].empty()) {
        const_cast<InputState&>(state).cursor_pos = 0;
    } else {
        // Display current line with tab visualization
        std::cout << visualize_tabs(state.lines[state.current_line_index]);
        // Set cursor position to end of line if not already set
        if (state.cursor_pos > state.lines[state.current_line_index].length()) {
            const_cast<InputState&>(state).cursor_pos = state.lines[state.current_line_index].length();
        }
    }

    // Set cursor position after printing the line
    set_cursor_position(state.prompt_len + get_visual_cursor_position(state.lines[state.current_line_index], state.cursor_pos));
}

/// \brief Handle extended (function) keys
CLIWide::KeyAction CLIWide::handle_extended_key(const KEY_EVENT_RECORD& keyEvent, InputState& state) {
    // Function keys - currently just continue
    return KeyAction::CONTINUE;
}

/// \brief Handle regular character keys
CLIWide::KeyAction CLIWide::handle_regular_key(const KEY_EVENT_RECORD& keyEvent, InputState& state) {
    WORD vkCode = keyEvent.wVirtualKeyCode;
    WCHAR unicodeChar = keyEvent.uChar.UnicodeChar;
    DWORD controlKeyState = keyEvent.dwControlKeyState;

    // Handle TAB key
    if (vkCode == VK_TAB) {
        // Check if adding tab would cause wrapping
        std::string test_line = state.lines[state.current_line_index];
        test_line.insert(state.cursor_pos, "\t");

        if (needs_line_wrap(test_line, state.prompt_len)) {
            // Line would wrap, so create new line first, then insert tab
            create_new_line_and_jump(state);
            // Now insert the tab in the new line
            state.lines[state.current_line_index].insert(state.cursor_pos, "\t");
            state.cursor_pos += 1;

            // Redraw the new line with the tab
            int visual_cursor_col = state.prompt_len + get_visual_cursor_position(state.lines[state.current_line_index], state.cursor_pos);
            move_cursor_to_column(state.prompt_len);
            clear_line_from_cursor();
            std::cout << visualize_tabs(state.lines[state.current_line_index]);
            set_cursor_position(visual_cursor_col);
        } else {
            // No wrapping needed, insert tab normally
            size_t insertion_pos = state.cursor_pos;
            state.lines[state.current_line_index].insert(state.cursor_pos, "\t");
            state.cursor_pos += 1;

            // Redraw line from the insertion point with tab visualization
            int visual_cursor_col = state.prompt_len + get_visual_cursor_position(state.lines[state.current_line_index], state.cursor_pos);
            move_cursor_to_column(state.prompt_len + get_visual_cursor_position(state.lines[state.current_line_index], insertion_pos));
            clear_line_from_cursor();
            std::cout << visualize_tabs(state.lines[state.current_line_index].substr(insertion_pos));
            set_cursor_position(visual_cursor_col);
        }

        state.history_nav_index = -1;
        return KeyAction::CONTINUE;
    }

    // Handle Enter key (including Shift + Enter detection)
    if (vkCode == VK_RETURN) {
        bool shift_pressed = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        std::cout << std::endl;

        if (shift_pressed) {
            // Shift + Enter: Create new line directly
            state.lines.push_back("");
            state.is_wrapped_line.push_back(false);  // User-created line, not wrapped
            state.current_line_index++;
            state.cursor_pos = 0;
            state.prompt_len = 4; // "... " for continuation lines

            // Safety check: ensure vectors stay in sync
            if (state.lines.size() != state.is_wrapped_line.size()) {
                size_t min_size = (state.lines.size() < state.is_wrapped_line.size()) ? state.lines.size() : state.is_wrapped_line.size();
                state.lines.resize(min_size);
                state.is_wrapped_line.resize(min_size);
            }

            return KeyAction::NEW_LINE;
        }

        if (state.lines[state.current_line_index].empty()) {
            // Empty line - check if this is end of input
            if (state.lines.size() == 1 && state.lines[0].empty()) {
                return KeyAction::RETURN_INPUT;  // Empty input
            }
            // For multi-line input, let the paste detection logic decide
            // Don't immediately exit on empty line - let should_continue_input() handle it
            return KeyAction::BREAK_INNER_LOOP;  // Break out of inner loop to check for paste
        }

        // Add to history if it's the first line and not a duplicate
        if (state.current_line_index == 0 && !state.lines[state.current_line_index].empty() &&
            (command_history.empty() || command_history.back() != state.lines[state.current_line_index])) {
            add_to_history(state.lines[state.current_line_index]);
        }

        // Check if this is a command (starts with /)
        if (state.current_line_index == 0 && !state.lines[state.current_line_index].empty() &&
            state.lines[state.current_line_index][0] == '/') {
            return KeyAction::RETURN_INPUT;  // Commands are always single-line
        }

        return KeyAction::BREAK_INNER_LOOP;  // Break out of inner loop to check for paste
    }

    // Handle backspace
    if (vkCode == VK_BACK) {
        handle_backspace_with_wrapping(state);
        return KeyAction::CONTINUE;
    }

    // Handle Ctrl+C
    if (vkCode == 'C' && (controlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED))) {
        std::cout << "^C\n";
        // Return "/bye" by setting the line content
        // clear first
        state.lines.clear();
        state.is_wrapped_line.clear();
        state.lines.push_back("/bye");
        state.is_wrapped_line.push_back(false);  // Command line, not wrapped
        state.current_line_index = 0;
        state.cursor_pos = 0;
        state.prompt_len = 4;

        // Safety check: ensure vectors stay in sync
        if (state.lines.size() != state.is_wrapped_line.size()) {
            size_t min_size = (state.lines.size() < state.is_wrapped_line.size()) ? state.lines.size() : state.is_wrapped_line.size();
            state.lines.resize(min_size);
            state.is_wrapped_line.resize(min_size);
        }

        return KeyAction::RETURN_INPUT;
    }

    // Handle Ctrl+D (EOF equivalent)
    if (vkCode == 'D' && (controlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED))) {
        if (state.lines.size() == 1 && state.lines[0].empty()) {
            state.lines[0] = "/bye";
            state.is_wrapped_line[0] = false;  // Command line, not wrapped

            // Safety check: ensure vectors stay in sync
            if (state.lines.size() != state.is_wrapped_line.size()) {
                size_t min_size = (state.lines.size() < state.is_wrapped_line.size()) ? state.lines.size() : state.is_wrapped_line.size();
                state.lines.resize(min_size);
                state.is_wrapped_line.resize(min_size);
            }

            return KeyAction::RETURN_INPUT;
        }
        return KeyAction::BREAK_OUTER_LOOP;
    }

    // Handle regular character input
    if (unicodeChar >= 32 || unicodeChar == 9) { // Printable characters or tab
        // Check if we're in paste mode
        if (is_paste_operation()) {
            state.paste_mode = true;
        }

        if (state.paste_mode) {
            // In paste mode, collect bytes in buffer and process complete UTF-8 characters
            // Convert Unicode character to UTF-8 and add to buffer
            state.utf8_buffer += unicodeChar;

            // Try to extract complete UTF-8 characters from buffer
            std::string complete_chars;
            size_t pos = 0;

            while (pos < state.utf8_buffer.length()) {
                unsigned char byte = static_cast<unsigned char>(state.utf8_buffer[pos]);
                size_t char_len = 1;

                // Determine expected character length
                if ((byte & 0x80) == 0) {
                    char_len = 1;  // ASCII
                } else if ((byte & 0xE0) == 0xC0) {
                    char_len = 2;  // 2-byte UTF-8
                } else if ((byte & 0xF0) == 0xE0) {
                    char_len = 3;  // 3-byte UTF-8
                } else if ((byte & 0xF8) == 0xF0) {
                    char_len = 4;  // 4-byte UTF-8
                } else {
                    // Invalid UTF-8 start byte, skip it
                    pos++;
                    continue;
                }

                // Check if we have the complete character
                if (pos + char_len <= state.utf8_buffer.length()) {
                    // Validate UTF-8 continuation bytes
                    bool valid = true;
                    for (size_t i = 1; i < char_len; i++) {
                        if ((static_cast<unsigned char>(state.utf8_buffer[pos + i]) & 0xC0) != 0x80) {
                            valid = false;
                            break;
                        }
                    }

                    if (valid) {
                        complete_chars += state.utf8_buffer.substr(pos, char_len);
                        pos += char_len;
                    } else {
                        // Invalid UTF-8 sequence, skip the start byte
                        pos++;
                    }
                } else {
                    // Incomplete character, break and keep remaining bytes in buffer
                    break;
                }
            }

            // Remove processed characters from buffer
            if (pos > 0) {
                state.utf8_buffer = state.utf8_buffer.substr(pos);
            }

            // Insert complete characters
            if (!complete_chars.empty()) {
                // Check if adding these characters would cause wrapping
                std::string test_line = state.lines[state.current_line_index];
                test_line.insert(state.cursor_pos, complete_chars);

                if (needs_line_wrap(test_line, state.prompt_len)) {
                    // Line would wrap, so create new line first, then insert characters
                    create_new_line_and_jump(state);
                    // Now insert the characters in the new line
                    state.lines[state.current_line_index].insert(state.cursor_pos, complete_chars);
                    state.cursor_pos += complete_chars.length();

                    // Redraw the new line with the characters
                    int visual_cursor_col = state.prompt_len + get_visual_cursor_position(state.lines[state.current_line_index], state.cursor_pos);

                    // Get current cursor position and move to correct position
                    CONSOLE_SCREEN_BUFFER_INFO csbi;
                    if (GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
                        COORD pos = {static_cast<SHORT>(state.prompt_len), csbi.dwCursorPosition.Y};
                        SetConsoleCursorPosition(hConsoleOutput, pos);
                        clear_line_from_cursor();
                        std::cout << visualize_tabs(state.lines[state.current_line_index]);

                        // Position cursor at the correct final position
                        COORD final_pos = {static_cast<SHORT>(visual_cursor_col), csbi.dwCursorPosition.Y};
                        SetConsoleCursorPosition(hConsoleOutput, final_pos);
                    }
                } else {
                    // No wrapping needed, insert characters normally
                    size_t insertion_pos = state.cursor_pos;
                    state.lines[state.current_line_index].insert(state.cursor_pos, complete_chars);
                    state.cursor_pos += complete_chars.length();

                    // Redraw line from the insertion point with tab visualization
                    int visual_cursor_col = state.prompt_len + get_visual_cursor_position(state.lines[state.current_line_index], state.cursor_pos);
                    int insertion_visual_col = state.prompt_len + get_visual_cursor_position(state.lines[state.current_line_index], insertion_pos);

                    // Get current cursor position and move to insertion point
                    CONSOLE_SCREEN_BUFFER_INFO csbi;
                    if (GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
                        COORD pos = {static_cast<SHORT>(insertion_visual_col), csbi.dwCursorPosition.Y};
                        SetConsoleCursorPosition(hConsoleOutput, pos);
                        clear_line_from_cursor();
                        std::cout << visualize_tabs(state.lines[state.current_line_index].substr(insertion_pos));

                        // Position cursor at the correct final position
                        COORD final_pos = {static_cast<SHORT>(visual_cursor_col), csbi.dwCursorPosition.Y};
                        SetConsoleCursorPosition(hConsoleOutput, final_pos);
                    }
                }
            }

        } else {
            // Normal typing mode - simplified UTF-8 handling
            // Check if adding this character would cause wrapping
            std::string test_line = state.lines[state.current_line_index];
            test_line.insert(state.cursor_pos, std::string(1, unicodeChar));

            if (needs_line_wrap(test_line, state.prompt_len)) {
                // Line would wrap, so create new line first, then insert character
                create_new_line_and_jump(state);
                // Now insert the character in the new line
                state.lines[state.current_line_index].insert(state.cursor_pos, std::string(1, unicodeChar));
                state.cursor_pos += 1;

                // Redraw the new line with the character
                int visual_cursor_col = state.prompt_len + get_visual_cursor_position(state.lines[state.current_line_index], state.cursor_pos);

                // Get current cursor position and move to correct position
                CONSOLE_SCREEN_BUFFER_INFO csbi;
                if (GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
                    COORD pos = {static_cast<SHORT>(state.prompt_len), csbi.dwCursorPosition.Y};
                    SetConsoleCursorPosition(hConsoleOutput, pos);
                    clear_line_from_cursor();
                    std::cout << visualize_tabs(state.lines[state.current_line_index]);

                    // Position cursor at the correct final position
                    COORD final_pos = {static_cast<SHORT>(visual_cursor_col), csbi.dwCursorPosition.Y};
                    SetConsoleCursorPosition(hConsoleOutput, final_pos);
                }
            } else {
                // No wrapping needed, insert character normally
                size_t insertion_pos = state.cursor_pos;
                state.lines[state.current_line_index].insert(state.cursor_pos, std::string(1, unicodeChar));
                state.cursor_pos += 1;

                // Redraw line from the insertion point with tab visualization
                int visual_cursor_col = state.prompt_len + get_visual_cursor_position(state.lines[state.current_line_index], state.cursor_pos);
                int insertion_visual_col = state.prompt_len + get_visual_cursor_position(state.lines[state.current_line_index], insertion_pos);

                // Get current cursor position and move to insertion point
                CONSOLE_SCREEN_BUFFER_INFO csbi;
                if (GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
                    COORD pos = {static_cast<SHORT>(insertion_visual_col), csbi.dwCursorPosition.Y};
                    SetConsoleCursorPosition(hConsoleOutput, pos);
                    clear_line_from_cursor();
                    std::cout << visualize_tabs(state.lines[state.current_line_index].substr(insertion_pos));

                    // Position cursor at the correct final position
                    COORD final_pos = {static_cast<SHORT>(visual_cursor_col), csbi.dwCursorPosition.Y};
                    SetConsoleCursorPosition(hConsoleOutput, final_pos);
                }
            }
        }

        // Reset history navigation when user types
        state.history_nav_index = -1;
    }

    return KeyAction::CONTINUE;
}

/// \brief Handle arrow keys (up/down/left/right)
CLIWide::KeyAction CLIWide::handle_arrow_keys(const KEY_EVENT_RECORD& keyEvent, InputState& state) {
    WORD vkCode = keyEvent.wVirtualKeyCode;

    if (vkCode == VK_UP) {
        handle_history_navigation(true, state);
        handle_multiline_navigation(true, state);
    } else if (vkCode == VK_DOWN) {
        handle_history_navigation(false, state);
        handle_multiline_navigation(false, state);
    } else if (vkCode == VK_LEFT) {
        if (state.cursor_pos > 0) {
            // Normal left movement within current line
            state.cursor_pos--;
            // Handle UTF-8 continuation bytes
            while (state.cursor_pos > 0 && (static_cast<unsigned char>(state.lines[state.current_line_index][state.cursor_pos]) & 0xC0) == 0x80) {
                state.cursor_pos--;
            }
            // Position cursor at the correct position
            {
                CONSOLE_SCREEN_BUFFER_INFO csbi;
                if (GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
                    int visual_cursor_col = state.prompt_len + get_visual_cursor_position(state.lines[state.current_line_index], state.cursor_pos);
                    COORD pos = {static_cast<SHORT>(visual_cursor_col), csbi.dwCursorPosition.Y};
                    SetConsoleCursorPosition(hConsoleOutput, pos);
                }
            }
        } else if (state.lines.size() > 1 && state.current_line_index > 0) {
            // At beginning of line and we have multiple lines - move to end of previous line
            state.current_line_index--;
            state.cursor_pos = state.lines[state.current_line_index].length();
            state.prompt_len = (state.current_line_index == 0) ? 4 : 4; // ">>> " or "... "

            // Move cursor up one row and position it correctly
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            if (GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
                SHORT new_y = static_cast<SHORT>(csbi.dwCursorPosition.Y - 1);

                // Move cursor up one row
                COORD pos = {0, new_y};
                SetConsoleCursorPosition(hConsoleOutput, pos);

                // Clear and redraw the previous line
                clear_line_from_cursor();
                bool is_first = (state.current_line_index == 0);
                std::cout << (is_first ? ">>> " : "... ") << visualize_tabs(state.lines[state.current_line_index]);

                // Position cursor at the end of the previous line
                int visual_cursor_col = state.prompt_len + get_visual_cursor_position(state.lines[state.current_line_index], state.cursor_pos);
                COORD cursor_pos = {static_cast<SHORT>(visual_cursor_col), new_y};
                SetConsoleCursorPosition(hConsoleOutput, cursor_pos);
            }
        }
    } else if (vkCode == VK_RIGHT) {
        if (state.cursor_pos < state.lines[state.current_line_index].length()) {
            // Normal right movement within current line
            state.cursor_pos++;
            // Handle UTF-8 continuation bytes
            while (state.cursor_pos < state.lines[state.current_line_index].length() && (static_cast<unsigned char>(state.lines[state.current_line_index][state.cursor_pos]) & 0xC0) == 0x80) {
                state.cursor_pos++;
            }
            // Position cursor at the correct position
            {
                CONSOLE_SCREEN_BUFFER_INFO csbi;
                if (GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
                    int visual_cursor_col = state.prompt_len + get_visual_cursor_position(state.lines[state.current_line_index], state.cursor_pos);
                    COORD pos = {static_cast<SHORT>(visual_cursor_col), csbi.dwCursorPosition.Y};
                    SetConsoleCursorPosition(hConsoleOutput, pos);
                }
            }
        } else if (state.lines.size() > 1 && state.current_line_index < static_cast<int>(state.lines.size()) - 1) {
            // At end of line and we have multiple lines - move to beginning of next line
            state.current_line_index++;
            state.cursor_pos = 0;
            state.prompt_len = (state.current_line_index == 0) ? 4 : 4; // ">>> " or "... "

            // Move cursor down one row and position it correctly
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            if (GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
                SHORT new_y = static_cast<SHORT>(csbi.dwCursorPosition.Y + 1);

                // Move cursor down one row
                COORD pos = {0, new_y};
                SetConsoleCursorPosition(hConsoleOutput, pos);

                // Clear and redraw the next line
                clear_line_from_cursor();
                bool is_first = (state.current_line_index == 0);
                std::cout << (is_first ? ">>> " : "... ") << visualize_tabs(state.lines[state.current_line_index]);

                // Position cursor at the beginning of the next line
                int visual_cursor_col = state.prompt_len + get_visual_cursor_position(state.lines[state.current_line_index], state.cursor_pos);
                COORD cursor_pos = {static_cast<SHORT>(visual_cursor_col), new_y};
                SetConsoleCursorPosition(hConsoleOutput, cursor_pos);
            }
        }
    }

    return KeyAction::CONTINUE;
}

/// \brief Handle navigation keys (home, end, delete)
CLIWide::KeyAction CLIWide::handle_navigation_keys(const KEY_EVENT_RECORD& keyEvent, InputState& state) {
    WORD vkCode = keyEvent.wVirtualKeyCode;

    switch (vkCode) {
        case VK_HOME:
            state.cursor_pos = 0;
            // Position cursor at the beginning of the line
            {
                CONSOLE_SCREEN_BUFFER_INFO csbi;
                if (GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
                    COORD pos = {static_cast<SHORT>(state.prompt_len), csbi.dwCursorPosition.Y};
                    SetConsoleCursorPosition(hConsoleOutput, pos);
                }
            }
            break;

        case VK_END:
            state.cursor_pos = state.lines[state.current_line_index].length();
            // Position cursor at the end of the line
            {
                CONSOLE_SCREEN_BUFFER_INFO csbi;
                if (GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
                    int visual_cursor_col = state.prompt_len + get_visual_cursor_position(state.lines[state.current_line_index], state.cursor_pos);
                    COORD pos = {static_cast<SHORT>(visual_cursor_col), csbi.dwCursorPosition.Y};
                    SetConsoleCursorPosition(hConsoleOutput, pos);
                }
            }
            break;

        case VK_DELETE:
            if (state.cursor_pos < state.lines[state.current_line_index].length()) {
                auto boundaries = get_utf8_char_boundaries(state.lines[state.current_line_index], state.cursor_pos);
                size_t char_end = boundaries.second;
                state.lines[state.current_line_index].erase(state.cursor_pos, char_end - state.cursor_pos);
                redraw_line_from_cursor(state);
            }
            break;
    }

    return KeyAction::CONTINUE;
}

/// \brief Handle history navigation
void CLIWide::handle_history_navigation(bool up, InputState& state) {
    if (state.lines.size() > 1) return;  // Only in single-line mode

    if (up) {
        if (!command_history.empty()) {
            if (state.history_nav_index == -1) {
                state.history_nav_index = command_history.size() - 1;
            } else if (state.history_nav_index > 0) {
                state.history_nav_index--;
            }

            // Clear current line and replace with history entry
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            if (GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
                COORD pos = {static_cast<SHORT>(state.prompt_len), csbi.dwCursorPosition.Y};
                SetConsoleCursorPosition(hConsoleOutput, pos);
                clear_line_from_cursor();
                state.lines[state.current_line_index] = command_history[state.history_nav_index];
                state.cursor_pos = state.lines[state.current_line_index].length();
                std::cout << visualize_tabs(state.lines[state.current_line_index]);
                std::cout.flush();
            }
        }
    } else {
        if (!command_history.empty() && state.history_nav_index != -1) {
            if (state.history_nav_index < static_cast<int>(command_history.size()) - 1) {
                state.history_nav_index++;
                // Clear current line and replace with history entry
                CONSOLE_SCREEN_BUFFER_INFO csbi;
                if (GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
                    COORD pos = {static_cast<SHORT>(state.prompt_len), csbi.dwCursorPosition.Y};
                    SetConsoleCursorPosition(hConsoleOutput, pos);
                    clear_line_from_cursor();
                    state.lines[state.current_line_index] = command_history[state.history_nav_index];
                    state.cursor_pos = state.lines[state.current_line_index].length();
                    std::cout << visualize_tabs(state.lines[state.current_line_index]);
                    std::cout.flush();
                }
            } else {
                // Go to empty line (beyond history)
                state.history_nav_index = -1;
                CONSOLE_SCREEN_BUFFER_INFO csbi;
                if (GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
                    COORD pos = {static_cast<SHORT>(state.prompt_len), csbi.dwCursorPosition.Y};
                    SetConsoleCursorPosition(hConsoleOutput, pos);
                    clear_line_from_cursor();
                    state.lines[state.current_line_index].clear();
                    state.cursor_pos = 0;
                }
            }
        }
    }
}

/// \brief Handle multiline navigation
void CLIWide::handle_multiline_navigation(bool up, InputState& state) {
    if (state.lines.size() <= 1) return;  // Only in multi-line mode

    if (up) {
        if (state.current_line_index > 0) {
            state.current_line_index--;
            state.cursor_pos = (state.cursor_pos < state.lines[state.current_line_index].length()) ? state.cursor_pos : state.lines[state.current_line_index].length();
            state.prompt_len = (state.current_line_index == 0) ? 4 : 4; // ">>> " or "... "

            // Redraw the current line
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            if (GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
                COORD pos = {0, csbi.dwCursorPosition.Y};
                SetConsoleCursorPosition(hConsoleOutput, pos);
                clear_line_from_cursor();
                bool is_first = (state.current_line_index == 0);
                std::cout << (is_first ? ">>> " : "... ") << visualize_tabs(state.lines[state.current_line_index]);

                // Position cursor at the correct position
                int visual_cursor_col = state.prompt_len + get_visual_cursor_position(state.lines[state.current_line_index], state.cursor_pos);
                COORD final_pos = {static_cast<SHORT>(visual_cursor_col), csbi.dwCursorPosition.Y};
                SetConsoleCursorPosition(hConsoleOutput, final_pos);
            }
        }
    } else {
        if (state.current_line_index < static_cast<int>(state.lines.size()) - 1) {
            state.current_line_index++;
            state.cursor_pos = (state.cursor_pos < state.lines[state.current_line_index].length()) ? state.cursor_pos : state.lines[state.current_line_index].length();
            state.prompt_len = (state.current_line_index == 0) ? 4 : 4; // ">>> " or "... "

            // Redraw the current line
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            if (GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
                COORD pos = {0, csbi.dwCursorPosition.Y};
                SetConsoleCursorPosition(hConsoleOutput, pos);
                clear_line_from_cursor();
                bool is_first = (state.current_line_index == 0);
                std::cout << (is_first ? ">>> " : "... ") << visualize_tabs(state.lines[state.current_line_index]);

                // Position cursor at the correct position
                int visual_cursor_col = state.prompt_len + get_visual_cursor_position(state.lines[state.current_line_index], state.cursor_pos);
                COORD final_pos = {static_cast<SHORT>(visual_cursor_col), csbi.dwCursorPosition.Y};
                SetConsoleCursorPosition(hConsoleOutput, final_pos);
            }
        }
    }
}

/// \brief Determine if input should continue
bool CLIWide::should_continue_input(const InputState& state) {
    // If this is the first line and it's empty, continue
    if (state.current_line_index == 0 && state.lines[0].empty()) {
        return true;
    }

    // Check if this is a command (starts with /)
    if (state.current_line_index == 0 && !state.lines[0].empty() && state.lines[0][0] == '/') {
        return false;  // Commands are always single-line
    }

    // Check for backslash continuation on current line
    if (!state.lines[state.current_line_index].empty() && state.lines[state.current_line_index].back() == '\\') {
            // Remove the backslash and continue
            const_cast<InputState&>(state).lines[state.current_line_index].pop_back();
            const_cast<InputState&>(state).lines.push_back("");
            const_cast<InputState&>(state).is_wrapped_line.push_back(false);  // User-created line, not wrapped
            const_cast<InputState&>(state).current_line_index++;
            const_cast<InputState&>(state).cursor_pos = 0;

            // Safety check: ensure vectors stay in sync
            if (state.lines.size() != state.is_wrapped_line.size()) {
                size_t min_size = (state.lines.size() < state.is_wrapped_line.size()) ? state.lines.size() : state.is_wrapped_line.size();
                const_cast<InputState&>(state).lines.resize(min_size);
                const_cast<InputState&>(state).is_wrapped_line.resize(min_size);
            }

            return true;
        }

    // If we're already in paste mode, be more lenient about continuing
    if (state.paste_mode) {
        // In paste mode, wait longer and check multiple times for more input
        for (int i = 0; i < 5; i++) {  // Increased from 3 to 5 checks
            std::this_thread::sleep_for(std::chrono::milliseconds(15));  // Reduced from 20ms to 15ms
            if (is_paste_operation()) {
                // More input is coming
                const_cast<InputState&>(state).lines.push_back("");
                const_cast<InputState&>(state).is_wrapped_line.push_back(false);  // User-created line, not wrapped
                const_cast<InputState&>(state).current_line_index++;
                const_cast<InputState&>(state).cursor_pos = 0;

                // Safety check: ensure vectors stay in sync
                if (state.lines.size() != state.is_wrapped_line.size()) {
                    size_t min_size = (state.lines.size() < state.is_wrapped_line.size()) ? state.lines.size() : state.is_wrapped_line.size();
                    const_cast<InputState&>(state).lines.resize(min_size);
                    const_cast<InputState&>(state).is_wrapped_line.resize(min_size);
                }

                return true;
            }
        }

        // No more input detected after multiple checks - end paste mode
        const_cast<InputState&>(state).paste_mode = false;
        const_cast<InputState&>(state).utf8_buffer.clear();

        // Clean up multiple empty lines at the end
        while (!state.lines.empty() && state.lines.back().empty()) {
            if (state.lines.size() > 1) {
                const_cast<InputState&>(state).lines.pop_back();
                const_cast<InputState&>(state).is_wrapped_line.pop_back();
            } else {
                break; // Keep at least one line
            }
        }

        // Safety check: ensure vectors stay in sync before returning
        if (state.lines.size() != state.is_wrapped_line.size()) {
            size_t min_size = (state.lines.size() < state.is_wrapped_line.size()) ? state.lines.size() : state.is_wrapped_line.size();
            const_cast<InputState&>(state).lines.resize(min_size);
            const_cast<InputState&>(state).is_wrapped_line.resize(min_size);
        }

        return false;
    }

    // Not in paste mode - check if this might be the start of a paste operation
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    if (is_paste_operation()) {
        // More input is coming - this is a paste operation
        const_cast<InputState&>(state).paste_mode = true;
        const_cast<InputState&>(state).lines.push_back("");
        const_cast<InputState&>(state).is_wrapped_line.push_back(false);  // User-created line, not wrapped
        const_cast<InputState&>(state).current_line_index++;
        const_cast<InputState&>(state).cursor_pos = 0;

        // Safety check: ensure vectors stay in sync
        if (state.lines.size() != state.is_wrapped_line.size()) {
            size_t min_size = (state.lines.size() < state.is_wrapped_line.size()) ? state.lines.size() : state.is_wrapped_line.size();
            const_cast<InputState&>(state).lines.resize(min_size);
            const_cast<InputState&>(state).is_wrapped_line.resize(min_size);
        }

        return true;
    } else {
        // Additional check: if we have multiple lines and the current line is empty,
        // this might be part of a paste operation that's still ongoing
        if (state.lines.size() > 1 && state.lines[state.current_line_index].empty()) {
            // Wait a bit more and check again
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (is_paste_operation()) {
                const_cast<InputState&>(state).paste_mode = true;
                const_cast<InputState&>(state).lines.push_back("");
                const_cast<InputState&>(state).is_wrapped_line.push_back(false);  // User-created line, not wrapped
                const_cast<InputState&>(state).current_line_index++;
                const_cast<InputState&>(state).cursor_pos = 0;

                // Safety check: ensure vectors stay in sync
                if (state.lines.size() != state.is_wrapped_line.size()) {
                    size_t min_size = (state.lines.size() < state.is_wrapped_line.size()) ? state.lines.size() : state.is_wrapped_line.size();
                    const_cast<InputState&>(state).lines.resize(min_size);
                    const_cast<InputState&>(state).is_wrapped_line.resize(min_size);
                }
                return true;
            }
        }

        // Not in paste mode - this was just a single Enter, finish input

        // Safety check: ensure vectors stay in sync before returning
        if (state.lines.size() != state.is_wrapped_line.size()) {
            size_t min_size = (state.lines.size() < state.is_wrapped_line.size()) ? state.lines.size() : state.is_wrapped_line.size();
            const_cast<InputState&>(state).lines.resize(min_size);
            const_cast<InputState&>(state).is_wrapped_line.resize(min_size);
        }

        return false;
    }
}

/// \brief Redraw line from cursor position
void CLIWide::redraw_line_from_cursor(const InputState& state) {
    int visual_cursor_col = state.prompt_len + get_visual_cursor_position(state.lines[state.current_line_index], state.cursor_pos);

    // Get current cursor position and move to correct position
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
        COORD pos = {static_cast<SHORT>(visual_cursor_col), csbi.dwCursorPosition.Y};
        SetConsoleCursorPosition(hConsoleOutput, pos);
        clear_line_from_cursor();
        std::cout << visualize_tabs(state.lines[state.current_line_index].substr(state.cursor_pos));

        // Position cursor at the correct final position
        COORD final_pos = {static_cast<SHORT>(visual_cursor_col), csbi.dwCursorPosition.Y};
        SetConsoleCursorPosition(hConsoleOutput, final_pos);
    }
}

/// \brief Get console width
int CLIWide::get_console_width() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
        return csbi.dwSize.X;
    }
    return 80; // Default fallback
}

/// \brief Check if line needs wrapping
bool CLIWide::needs_line_wrap(const std::string& line, int prompt_len) {
    int console_width = get_console_width();
    int line_width = prompt_len + get_utf8_display_width(visualize_tabs(line));
    return line_width >= console_width;
}

/// \brief Create a new line and move cursor to it
void CLIWide::create_new_line_and_jump(InputState& state) {
    // Create a new empty line after the current line
    state.lines.insert(state.lines.begin() + state.current_line_index + 1, "");
    state.is_wrapped_line.insert(state.is_wrapped_line.begin() + state.current_line_index + 1, true);  // Mark as wrapped
    state.current_line_index++;
    state.cursor_pos = 0;
    state.prompt_len = 4; // "... " for continuation lines

    // Safety check: ensure vectors stay in sync
    if (state.lines.size() != state.is_wrapped_line.size()) {
        size_t min_size = (state.lines.size() < state.is_wrapped_line.size()) ? state.lines.size() : state.is_wrapped_line.size();
        state.lines.resize(min_size);
        state.is_wrapped_line.resize(min_size);
    }

    // Print the new line
    std::cout << std::endl << "... ";
    std::cout.flush();

    // Position cursor at the beginning of the new line
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
        COORD pos = {static_cast<SHORT>(state.prompt_len), csbi.dwCursorPosition.Y};
        SetConsoleCursorPosition(hConsoleOutput, pos);
    }
}

/// \brief Handle backspace with line wrapping support
void CLIWide::handle_backspace_with_wrapping(InputState& state) {
    if (state.cursor_pos > 0) {
        // Normal backspace within the same line
        std::string& current_line = state.lines[state.current_line_index];

        // Check if we're deleting a tab character
        if (current_line[state.cursor_pos - 1] == '\t') {
            current_line.erase(state.cursor_pos - 1, 1);
            state.cursor_pos--;
        } else {
            // Find the start of the character to delete
            size_t delete_start = state.cursor_pos - 1;
            // Handle UTF-8 continuation bytes
            while (delete_start > 0 && (static_cast<unsigned char>(current_line[delete_start]) & 0xC0) == 0x80) {
                delete_start--;
            }

            current_line.erase(delete_start, state.cursor_pos - delete_start);
            state.cursor_pos = delete_start;
        }

        // Redraw line from cursor position with tab visualization
        int visual_cursor_col = state.prompt_len + get_visual_cursor_position(current_line, state.cursor_pos);

        // Get current cursor position and move to correct position
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
            COORD pos = {static_cast<SHORT>(visual_cursor_col), csbi.dwCursorPosition.Y};
            SetConsoleCursorPosition(hConsoleOutput, pos);
            clear_line_from_cursor();
            std::cout << visualize_tabs(current_line.substr(state.cursor_pos));

            // Position cursor at the correct final position
            COORD final_pos = {static_cast<SHORT>(visual_cursor_col), csbi.dwCursorPosition.Y};
            SetConsoleCursorPosition(hConsoleOutput, final_pos);
        }
    } else if (state.current_line_index > 0) {
        // Backspace at beginning of line - delete last character of previous line and current line
        std::string& current_line = state.lines[state.current_line_index];
        std::string& prev_line = state.lines[state.current_line_index - 1];

        // Remove the current line first
        state.lines.erase(state.lines.begin() + state.current_line_index);
        state.is_wrapped_line.erase(state.is_wrapped_line.begin() + state.current_line_index);

        // Move to previous line
        state.current_line_index--;
        state.prompt_len = (state.current_line_index == 0) ? 4 : 4; // ">>> " or "... "

        // Delete the last character of the previous line
        if (!prev_line.empty()) {
            // Find the start of the last character to delete
            size_t delete_start = prev_line.length() - 1;
            // Handle UTF-8 continuation bytes
            while (delete_start > 0 && (static_cast<unsigned char>(prev_line[delete_start]) & 0xC0) == 0x80) {
                delete_start--;
            }

            prev_line.erase(delete_start);
            state.cursor_pos = prev_line.length();
        } else {
            state.cursor_pos = 0;
        }

        // Move cursor up to the previous line and redraw the content
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
            // Move cursor up one line
            COORD pos = {0, static_cast<SHORT>(csbi.dwCursorPosition.Y - 1)};
            SetConsoleCursorPosition(hConsoleOutput, pos);
            clear_line_from_cursor();
            bool is_first = (state.current_line_index == 0);
            std::cout << (is_first ? ">>> " : "... ") << visualize_tabs(prev_line);

            // Position cursor at the correct position on the previous line
            int visual_cursor_col = state.prompt_len + get_visual_cursor_position(prev_line, state.cursor_pos);
            COORD cursor_pos = {static_cast<SHORT>(visual_cursor_col), static_cast<SHORT>(csbi.dwCursorPosition.Y - 1)};
            SetConsoleCursorPosition(hConsoleOutput, cursor_pos);
        }
    }
}

#endif
