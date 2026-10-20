#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lemon::backends {

struct HookAbiDescriptor {
    std::string id;
    std::string stage;
    std::uint32_t revision;
    std::uint32_t implementation_revision;
    std::vector<std::string> operations;
};

const std::vector<HookAbiDescriptor>& installed_hook_abis();
const HookAbiDescriptor* find_hook_abi(std::string_view id) noexcept;
const std::string& installed_hook_abi_digest();

}
