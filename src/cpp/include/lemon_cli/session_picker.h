#pragma once

#include <string>
#include <vector>

namespace lemon_cli {

struct PickerRow {
    std::string id;
    std::string label;
    std::string when;
    std::string model;
    std::string first;     // First user message, one line.
    std::string last;      // Last message, one line.
    std::string haystack;  // Lowercased text that the filter searches.
    size_t msgs = 0;
    bool current = false;
};

struct PickerStyle {
    bool ansi = false;
    bool unicode = false;
};

struct PickerResult {
    enum class Kind { Cancel, Open, Delete, Rename };
    Kind kind = Kind::Cancel;
    std::string id;      // The highlighted session for Open, Delete and Rename.
    std::string filter;  // The filter that was active, so the caller can reopen with it.
};

// Lets the user browse `rows` with the arrow keys. It draws in place below the cursor and erases
// itself before returning. Requires RawTerminal::available().
PickerResult run_session_picker(const std::vector<PickerRow>& rows, const PickerStyle& style,
                                const std::string& filter, const std::string& selected_id);

} // namespace lemon_cli
