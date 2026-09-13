// Proves every image-backed recipe's committed pins line up with its descriptor.
// A support row with no pin resolves to "publishes no toolbox image for <arch>"
// at load time; a pin with no support row is dead weight the refresh workflow
// would keep bumping.
//
// Build with: cmake --build --preset default --target test_toolbox_image_pins
// Run with: ctest --test-dir build -R '^ToolboxImagePinsTest$' --output-on-failure

#include "lemon/backends/backend_descriptor_registry.h"
#include "lemon/backends/container_image_pins.h"

#include <iostream>
#include <map>
#include <set>
#include <string>

using lemon::backends::all_descriptors;
using lemon::backends::all_image_pins;
using lemon::backends::image_pin;

namespace {

int failures = 0;

void expect(bool condition, const std::string& label) {
    if (condition) {
        std::cout << "PASS: " << label << std::endl;
    } else {
        std::cout << "FAIL: " << label << std::endl;
        ++failures;
    }
}

bool is_sha256(const std::string& digest) {
    if (digest.rfind("sha256:", 0) != 0) return false;
    const std::string hex = digest.substr(7);
    if (hex.size() != 64) return false;
    return hex.find_first_not_of("0123456789abcdef") == std::string::npos;
}

}  // namespace

int main() {
    const auto pins = all_image_pins();
    expect(!pins.empty(), "at least one image pin is committed");

    std::set<std::string> pinned;  // "<recipe>|<variant>|<arch>"
    std::set<std::string> image_backed_recipes;

    for (const auto& pin : pins) {
        const std::string label = pin.recipe + ":" + pin.variant + "/" + pin.arch;
        expect(!pin.image.repository.empty(), label + " names a repository");
        expect(!pin.image.tag.empty(), label + " records the tag it was resolved from");
        expect(is_sha256(pin.image.digest), label + " pins a sha256 digest");
        expect(pin.image.channel == "stable" || pin.image.channel == "experimental",
               label + " declares a known channel");
        expect(pin.image.pinned_ref() == pin.image.repository + "@" + pin.image.digest,
               label + " resolves to a digest reference, never a tag");
        expect(pinned.insert(pin.recipe + "|" + pin.variant + "|" + pin.arch).second,
               label + " is pinned exactly once");
        image_backed_recipes.insert(pin.recipe);
    }

    for (const auto* descriptor : all_descriptors()) {
        if (!descriptor->image_backed) {
            expect(image_backed_recipes.count(descriptor->recipe) == 0,
                   descriptor->recipe + " declares no pins unless it is image-backed");
            continue;
        }

        expect(!descriptor->support.empty(),
               descriptor->recipe + " declares at least one support row");

        for (const auto& row : descriptor->support) {
            auto amd = row.devices.find("amd_gpu");
            if (amd == row.devices.end()) continue;
            for (const auto& arch : amd->second) {
                const auto ref = image_pin(descriptor->recipe, row.backend, arch);
                expect(ref.valid(), descriptor->recipe + ":" + row.backend + " has a pin for " +
                                        arch + " as its support row claims");
                pinned.erase(descriptor->recipe + "|" + row.backend + "|" + arch);
            }
        }
    }

    for (const auto& leftover : pinned) {
        expect(false, "pin " + leftover + " has no matching descriptor support row");
    }

    if (failures == 0) {
        std::cout << "All toolbox image pin tests passed" << std::endl;
        return 0;
    }
    std::cout << failures << " toolbox image pin test(s) failed" << std::endl;
    return 1;
}
