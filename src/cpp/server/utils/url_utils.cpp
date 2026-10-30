#include <lemon/utils/url_utils.h>

#include <algorithm>
#include <cctype>
#include <charconv>

namespace lemon::utils {

void parse_target_url(std::string_view input_host, std::string& out_clean_host, int& out_port, bool& out_is_ssl, bool override_default_port) {
    std::string_view remaining = input_host;
    bool has_scheme = false;

    // Helper for case-insensitive scheme matching
    auto starts_with_case_insensitive = [](std::string_view str, std::string_view prefix) {
        if (str.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(str[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) {
                return false;
            }
        }
        return true;
    };

    if (starts_with_case_insensitive(remaining, "https://")) {
        out_is_ssl = true;
        remaining.remove_prefix(8);
        has_scheme = true;
    } else if (starts_with_case_insensitive(remaining, "http://")) {
        out_is_ssl = false;
        remaining.remove_prefix(7);
        has_scheme = true;
    }

    // Strip path, query parameters, or fragments
    size_t limit_pos = remaining.find_first_of("/?#");
    if (limit_pos != std::string_view::npos) {
        remaining = remaining.substr(0, limit_pos);
    }

    // Parse port and host
    size_t close_bracket = remaining.find(']');
    size_t colon_pos = std::string_view::npos;
    if (remaining.rfind('[', 0) == 0 && close_bracket != std::string_view::npos) {
        size_t post_bracket_colon = remaining.find(':', close_bracket);
        if (post_bracket_colon != std::string_view::npos) {
            colon_pos = post_bracket_colon;
        }
    } else {
        // Only treat the last colon as a port separator if there is at most one colon.
        // Multiple colons without brackets indicate a raw IPv6 address.
        size_t colon_count = 0;
        for (char c : remaining) {
            if (c == ':') colon_count++;
        }
        if (colon_count <= 1) {
            colon_pos = remaining.rfind(':');
        }
    }

    if (colon_pos != std::string_view::npos) {
        out_clean_host = std::string(remaining.substr(0, colon_pos));
        std::string_view port_str = remaining.substr(colon_pos + 1);
        int parsed_port = 0;
        auto [ptr, ec] = std::from_chars(port_str.data(), port_str.data() + port_str.size(), parsed_port);
        if (ec == std::errc{} && ptr == port_str.data() + port_str.size() && parsed_port >= 1 && parsed_port <= 65535) {
            out_port = parsed_port;
        } else {
            out_port = -1; // Flag invalid port
        }
    } else {
        out_clean_host = std::string(remaining);
        if (has_scheme && override_default_port) {
            out_port = out_is_ssl ? 443 : 80;
        }
    }
}

std::string bracket_host_if_ipv6(std::string_view host) {
    if (host.find(':') != std::string_view::npos && host.front() != '[') {
        return "[" + std::string(host) + "]";
    }
    return std::string(host);
}

} // namespace lemon::utils
