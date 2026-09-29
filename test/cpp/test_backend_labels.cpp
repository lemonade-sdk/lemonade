// Proves every backend a descriptor lists in `support` carries a tier and a
// format, and that no label names a backend the descriptor does not list, so
// /system-info, the CLI and the docs never show an unlabeled backend.

#include "lemon/backends/backend_descriptor_registry.h"

#include <cstdio>
#include <set>
#include <string>

namespace {

int failures = 0;

void check(const std::string& what, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

}  // namespace

int main() {
    for (const auto* desc : lemon::backends::all_descriptors()) {
        std::set<std::string> supported;
        for (const auto& row : desc->support) {
            supported.insert(row.backend);
        }
        for (const auto& backend : supported) {
            check(desc->recipe + ":" + backend + " has a tier and a format",
                  desc->labels_for(backend) != nullptr);
        }
        for (const auto& [backend, labels] : desc->labels) {
            (void)labels;
            check(desc->recipe + ":" + backend + " is labeled and listed in support",
                  supported.count(backend) > 0);
        }
    }

    if (failures == 0) {
        std::printf("\nAll backend label checks passed.\n");
        return 0;
    }
    std::printf("\n%d backend label check(s) failed.\n", failures);
    return 1;
}
