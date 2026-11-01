#pragma once

#include "backend_definition_exports_internal.h"

#include <set>
#include <string>

namespace lemon::backends {

inline std::set<std::string> expected_container_source_ids() {
    static_assert(LEMON_C8_SOURCE_COUNT == 1 || LEMON_C8_SOURCE_COUNT == 2);
    std::set<std::string> ids{"gufo", "hrx", "llamacpp", "rocmfpx"};
#if LEMON_C8_SOURCE_COUNT == 2
    ids.insert("halogen");
#endif
    return ids;
}

inline std::set<std::string> current_container_source_ids() {
    std::set<std::string> ids;
    for (const auto& source : embedded_v2_sources()) ids.insert(source.key.id);
    return ids;
}

}
