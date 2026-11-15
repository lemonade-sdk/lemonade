#pragma once

// The device-name label rule, shared by the server (tree validation, the
// device-name check route) and the SDK (the mesh port's host routing). One
// implementation, so both sides fold a name to the same DNS label.

#include <string>
#include <string_view>

namespace nexus::tree {

/// Longest label device_label returns (one DNS label).
inline constexpr std::size_t kDeviceLabelMaxLength = 63;

/// A device name as a DNS label: every letter folded to lower case, every
/// run of characters other than ASCII letters and digits replaced by one
/// hyphen, no leading or trailing hyphen, at most kDeviceLabelMaxLength
/// characters. Names that fold to the same label are the same name on the
/// mesh port ("Work PC" and "work-pc" are both "work-pc"). Empty when the
/// name holds no letter or digit. Pure and thread-safe; never depends on the
/// locale.
[[nodiscard]] std::string device_label(std::string_view device_name);

}  // namespace nexus::tree
