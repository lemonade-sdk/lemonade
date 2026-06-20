#pragma once

#include <string>

namespace lemon {

// Collapse a specific RDNA dGPU gfx target to the ROCm "family" download target
// that the backend support set and install filenames expect:
//   gfx1030-gfx1036 -> gfx103X   (RDNA2, Radeon RX 6000)
//   gfx1100-gfx1103 -> gfx110X   (RDNA3, Radeon RX 7000)
//   gfx1200/gfx1201 -> gfx120X   (RDNA4, Radeon RX 9000)
// iGPU targets (gfx1150/gfx1151/gfx1152) ship as exact binaries and pass through
// unchanged, as do CDNA / exact-package targets (e.g. gfx90a). Anything else
// passes through unchanged.
//
// The RDNA2 set mirrors backend_versions.json `url_mapping`, which maps every
// gfx1030-gfx1036 to the published `gfx103X-all` archive.
//
// This restores the specific->family normalization that #2093 (commit 2a7aa18c)
// dropped when it removed ROCM_ARCH_MAPPING. Without it, identify_rocm_arch_from_name
// returns the specific arch (e.g. gfx1100, gfx1201) from the gfx-regex and KFD
// numeric-ISA detection paths, which no longer matches the gfx103X/gfx110X/gfx120X
// families in the support set -> ROCm reported "unsupported" for every RDNA2/3/4
// dGPU on those paths (#2319).
inline std::string normalize_rocm_family(const std::string& arch) {
    // RDNA2 / RDNA3 / RDNA4 dGPU families collapse to one ROCm download target.
    if (arch == "gfx1030" || arch == "gfx1031" || arch == "gfx1032" ||
        arch == "gfx1033" || arch == "gfx1034" || arch == "gfx1035" || arch == "gfx1036") {
        return "gfx103X";
    }
    if (arch == "gfx1100" || arch == "gfx1101" || arch == "gfx1102" || arch == "gfx1103") {
        return "gfx110X";
    }
    if (arch == "gfx1200" || arch == "gfx1201") {
        return "gfx120X";
    }
    // iGPU exact targets (gfx1150/gfx1151/gfx1152), CDNA (gfx90a, ...), and any
    // other target pass through unchanged. (Family normalization assumes a
    // single hex minor/step digit, which holds for all RDNA families today.)
    return arch;
}

}  // namespace lemon
