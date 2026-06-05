/*!
 *  Copyright (c) 2025 by Contributors
 * \file cli_wide.hpp
 * \brief CLI interactive input handling
 * \author FastFlowLM Team
 * \date 2025-06-24
 *  \version 0.9.24
 */
#pragma once
#include <vector>
#include <deque>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

/// \brief CLIWide class for interactive input handling
class CLIWide {
    public:
        CLIWide();
        ~CLIWide();

        /// \brief Get interactive input with arrow key support
        std::string get_interactive_input();

        /// \brief Add command to history
        void add_to_history(const std::string& command);

    private:
        // Command history for arrow key navigation
        std::deque<std::string> command_history;
        int history_index = -1;
        static constexpr size_t max_history_size = 100;

#ifdef _WIN32
        // Console handles
        HANDLE hConsoleInput;
        HANDLE hConsoleOutput;
        DWORD originalInputMode;
        DWORD originalOutputMode;

        /// \brief Input state structure
        struct InputState {
            std::vector<std::string> lines;
            std::vector<bool> is_wrapped_line;  // Track which lines were created by wrapping
            int current_line_index = 0;
            size_t cursor_pos = 0;
            int history_nav_index = -1;
            bool paste_mode = false;
            int prompt_len = 4;

            // UTF-8 input accumulation
            std::string utf8_buffer;
            int expected_utf8_bytes = 0;
        };

        /// \brief Key handling methods
        enum class KeyAction {
            CONTINUE,
            NEW_LINE,
            BREAK_INNER_LOOP,
            BREAK_OUTER_LOOP,
            RETURN_INPUT
        };

        KeyAction handle_extended_key(const KEY_EVENT_RECORD& keyEvent, InputState& state);
        KeyAction handle_regular_key(const KEY_EVENT_RECORD& keyEvent, InputState& state);
        KeyAction handle_arrow_keys(const KEY_EVENT_RECORD& keyEvent, InputState& state);
        KeyAction handle_navigation_keys(const KEY_EVENT_RECORD& keyEvent, InputState& state);
        void handle_history_navigation(bool up, InputState& state);
        void handle_multiline_navigation(bool up, InputState& state);
        bool should_continue_input(const InputState& state);
        void print_prompt_and_line(const InputState& state);
        void redraw_line_from_cursor(const InputState& state);

        /// \brief Helper functions for enhanced input
        void move_cursor_to_column(int column);
        void clear_line_from_cursor();
        void set_cursor_position(int column);
        size_t get_utf8_display_width(const std::string& str);
        std::pair<size_t, size_t> get_utf8_char_boundaries(const std::string& str, size_t cursor_pos);
        std::string visualize_tabs(const std::string& str);
        size_t get_visual_cursor_position(const std::string& str, size_t cursor_pos);

        /// \brief Line wrapping functionality
        int get_console_width();
        bool needs_line_wrap(const std::string& line, int prompt_len);
        void create_new_line_and_jump(InputState& state);
        void handle_backspace_with_wrapping(InputState& state);

        /// \brief UTF-8 conversion utilities
        std::string wchar_to_utf8(const std::wstring& wstr);
        std::wstring utf8_to_wchar(const std::string& str);

        /// \brief Console input utilities
        bool read_console_input(INPUT_RECORD& inputRecord);
        bool is_paste_operation();
#endif
};
