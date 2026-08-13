#pragma once

#include <string>

namespace lemon::utils {

bool is_tcp_listener_active(int family, const std::string& host_ip, int port);

} // namespace lemon::utils
