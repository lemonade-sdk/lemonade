#include <LemonadeNexus/Tree/DeviceLabel.hpp>

namespace nexus::tree {

namespace {

// ASCII-only classification. Tree validation runs this on every Tier 1
// server, so the result must not follow the process locale.
bool ascii_alnum(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

char ascii_lower(unsigned char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
}

}  // namespace

std::string device_label(std::string_view device_name) {
    std::string label;
    for (const unsigned char c : device_name) {
        if (ascii_alnum(c)) label += ascii_lower(c);
        else if (!label.empty() && label.back() != '-') label += '-';
    }
    while (!label.empty() && label.back() == '-') label.pop_back();
    if (label.size() > kDeviceLabelMaxLength) label.resize(kDeviceLabelMaxLength);
    while (!label.empty() && label.back() == '-') label.pop_back();
    return label;
}

}  // namespace nexus::tree
