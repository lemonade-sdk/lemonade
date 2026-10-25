#pragma once

#include <string>

namespace lemon {

// The rules a classification label list must satisfy, enforced identically on a
// `/v1/classify` request and on a routing policy's `labels`.
//
// These mirror ort-server's three NON-CONFIGURABLE checks (no control
// characters, no duplicates, nothing blank). Its size limits: `max_labels` and
// `max_label_chars` are deliberately NOT mirrored: they are derived per model
// from the token window and overridable through `manifest.json` or
// `--max-labels`, so a number pinned here would refuse requests a large-window
// router accepts. ort-server owns sizing and answers 400 with both numbers.

inline bool label_has_control_char(const std::string& label) {
    for (unsigned char c : label) {
        if (c < 0x20 || c == 0x7F) return true;
    }
    return false;
}

inline bool label_is_blank(const std::string& label) {
    return label.find_first_not_of(" \t\r\n") == std::string::npos;
}

} // namespace lemon
