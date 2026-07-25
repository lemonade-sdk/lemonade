#pragma once

#include <string>
#include <string_view>

namespace lemon::utils {

/**
 * @brief Parses a target URL or host string into its constituent parts.
 *
 * @param input_host The raw input string containing the host, URL, or IP address.
 * @param out_clean_host Output parameter for the parsed host (without scheme/port/path/query/fragment).
 * @param out_port Output parameter for the parsed port. Set to -1 if invalid; retains incoming value if not present.
 * @param out_is_ssl Output parameter indicating if scheme is HTTPS. Retains incoming value if no scheme is matched.
 * @param override_default_port If true and scheme is present, overrides the port with 80/443 if no explicit port is found.
 */
void parse_target_url(std::string_view input_host, std::string& out_clean_host, int& out_port, bool& out_is_ssl, bool override_default_port = true);

/**
 * @brief Encloses raw IPv6 address strings in brackets ([::1]) for use in URLs.
 *
 * @param host The hostname or IP address to check.
 * @return A bracketed version if it contains colons and is not already bracketed, otherwise the original string.
 */
std::string bracket_host_if_ipv6(std::string_view host);

} // namespace lemon::utils
