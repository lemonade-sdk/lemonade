#pragma once

#include "backend_definition_exports_internal.h"

#include <set>
#include <string>

#ifndef LEMON_C9_SOURCE_COUNT
#define LEMON_C9_SOURCE_COUNT 0
#endif

namespace lemon::backends {

inline std::set<std::string> expected_container_source_ids() {
    static_assert(LEMON_C8_SOURCE_COUNT == 1 || LEMON_C8_SOURCE_COUNT == 2);
    std::set<std::string> ids{"gufo", "hrx", "llamacpp", "rocmfpx"};
#if LEMON_C8_SOURCE_COUNT == 2
    ids.insert("halogen");
#endif
    static_assert(LEMON_C9_SOURCE_COUNT >= 0 && LEMON_C9_SOURCE_COUNT <= 3);
#if LEMON_C9_SOURCE_COUNT >= 1
    ids.insert("halobox");
#endif
#if LEMON_C9_SOURCE_COUNT >= 2
    ids.insert("nathanw");
#endif
#if LEMON_C9_SOURCE_COUNT >= 3
    ids.insert("ds4_rocm");
#endif
    return ids;
}

inline std::set<std::string> current_container_source_ids() {
    std::set<std::string> ids;
    for (const auto& source : embedded_v2_sources()) ids.insert(source.key.id);
    return ids;
}

}
