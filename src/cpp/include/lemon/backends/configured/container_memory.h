#pragma once

#include <cstdint>
#include <optional>

namespace lemon::backends {

struct IntegratedMemoryPool {
    std::optional<bool> available;
    std::optional<double> vram_gib;
    std::optional<double> virtual_gib;
};
struct ArtifactByteFact {
    bool known;
    std::uint64_t bytes;
};
struct FreeMemoryFact {
    std::optional<bool> known;
    std::optional<double> gib;
};

}
