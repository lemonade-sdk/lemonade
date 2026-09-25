// Proves every committed image pin is a valid entry and lines up with a
// descriptor support row. A support row with no pin resolves to "publishes no
// image for <arch>" at load time. Running in a container is per backend, not
// per recipe: llamacpp ships release binaries with one container backend among
// them.
//
// Build with: cmake --build --preset default --target test_container_image_pins
// Run with: ctest --test-dir build -R '^ContainerImagePinsTest$' --output-on-failure

#include "lemon/backends/backend_descriptor_registry.h"
#include "lemon/backends/container_image_pins.h"

#include <iostream>
#include <nlohmann/json.hpp>
#include <map>
#include <set>
#include <string>

using lemon::backends::all_descriptors;
using lemon::backends::all_image_pins;
using lemon::backends::backend_is_image_backed;
using lemon::backends::image_from_entry;
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

    std::set<std::string> pinned;  // "<recipe>|<backend>|<arch>"

    for (const auto& pin : pins) {
        const std::string label = pin.recipe + ":" + pin.backend + "/" + pin.arch;
        expect(!pin.image.repository.empty(), label + " names a repository");
        expect(!pin.image.tag.empty(), label + " records the tag it was resolved from");
        expect(is_sha256(pin.image.digest), label + " pins a sha256 digest");
        expect(pin.image.repository.rfind("docker.io/kyuz0/", 0) == 0 ||
                   pin.image.repository.rfind("ghcr.io/peonist-ai/", 0) == 0,
               label + " is published from an allowed repository");
        expect(pin.image.pinned_ref() == pin.image.repository + "@" + pin.image.digest,
               label + " resolves to a digest reference");
        expect(!pin.image.devices.empty(), label + " lists the devices its container opens");
        expect(pinned.insert(pin.recipe + "|" + pin.backend + "|" + pin.arch).second,
               label + " is pinned exactly once");
        expect(backend_is_image_backed(pin.recipe, pin.backend),
               label + " is pinned only because that backend runs from an image");
    }

    for (const auto* descriptor : all_descriptors()) {
        for (const auto& row : descriptor->support) {
            if (!backend_is_image_backed(descriptor->recipe, row.backend)) continue;
            auto amd = row.devices.find("amd_gpu");
            if (amd == row.devices.end()) continue;
            for (const auto& arch : amd->second) {
                const auto image = image_pin(descriptor->recipe, row.backend, arch);
                expect(image.valid(), descriptor->recipe + ":" + row.backend + " has a pin for " +
                                        arch + " as its support row claims");
                pinned.erase(descriptor->recipe + "|" + row.backend + "|" + arch);
            }
        }
    }

    const auto halogen = image_pin("halogen", "rocm", "gfx1151");
    const decltype(halogen.env) halogen_env = {{"HALOGEN_CTX", "262144"},
                                               {"HALOGEN_KV_POOL_POSITIONS", "524288"},
                                               {"HALOGEN_KV_SLOTS", "4"},
                                               {"HALOGEN_PROMPT_CACHE", "2"}};
    expect(halogen.env == halogen_env, "halogen:rocm carries Cockpit's engine settings");
    expect(halogen.ipc_host && halogen.memlock_unlimited, "halogen:rocm shares IPC and memlock");

    const nlohmann::json good = {{"repository", "docker.io/kyuz0/amd-strix-halo-toolboxes"},
                                 {"tag", "rocm-10.0"},
                                 {"digest", "sha256:abc"},
                                 {"devices", {"/dev/dri"}}};
    expect(image_from_entry(good).valid(), "a complete entry from an allowed repository parses");
    for (const char* field : {"repository", "tag", "digest", "devices"}) {
        nlohmann::json missing = good;
        missing.erase(field);
        expect(!image_from_entry(missing).valid(),
               std::string("an entry without '") + field + "' is rejected");
    }
    nlohmann::json elsewhere = good;
    elsewhere["repository"] = "docker.io/someone-else/llama";
    expect(!image_from_entry(elsewhere).valid(), "an entry outside the allowed repositories is rejected");

    for (const auto& leftover : pinned) {
        expect(false, "pin " + leftover + " has no matching descriptor support row");
    }

    if (failures == 0) {
        std::cout << "All container image pin tests passed" << std::endl;
        return 0;
    }
    std::cout << failures << " container image pin test(s) failed" << std::endl;
    return 1;
}
