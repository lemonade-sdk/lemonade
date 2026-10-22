#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

#include "lemon/backends/backend_utils.h"
#include "lemon/backends/configured/backend_definition.h"
#include "lemon/backends/ds4/ds4_server.h"
#include "lemon/system_info.h"
#include "lemon/utils/custom_args.h"

using lemon::SystemInfo;

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

const lemon::json& definition_records() {
    return lemon::backends::ds4::spec()->definition->records;
}

bool definition_publishes_for_os(const std::string& os) {
    for (const auto& row : definition_records().at("metadata").at("support")) {
        if (row.at("backend") == "rocm") {
            const auto& supported = row.at("supported_os");
            return std::find(supported.begin(), supported.end(), os) != supported.end();
        }
    }
    return false;
}

std::set<std::string> definition_reserved_flags() {
    return definition_records().at("launch").at("custom_args").at("reserved").get<std::set<std::string>>();
}

lemon::backends::InstallParams install_params(const std::string& backend, const std::string& version,
                                              const std::string& os, const std::string& arch) {
    const auto* spec = lemon::backends::ds4::spec();
    auto platform = lemon::backends::native_platform();
    platform.os = os;
    platform.rocm_arch = arch;
    return spec->install_params_fn({*spec, spec->definition, "ds4", spec->pin_recipe, backend, "",
                                    version, version, platform, lemon::backends::InstallPhase::Asset});
}

std::string install_filename(const std::string& arch, const std::string& version) {
    return install_params("rocm", version, "linux", arch).filename;
}

std::string install_error(const std::string& backend, const std::string& os, const std::string& arch) {
    try {
        install_params(backend, "b0001", os, arch);
    } catch (const std::exception& error) {
        return error.what();
    }
    return "";
}

bool throws_for_backend(const std::string& backend) {
    return !install_error(backend, "linux", "").empty();
}

bool throws_for_arch(const std::string& arch) {
    return !install_error("rocm", "linux", arch).empty();
}

}  // namespace

int main() {
    const auto params = install_params("rocm", "b0001", "linux", "gfx1151");

    expect(params.repo == "lemonade-sdk/ds4-rocm",
           "rocm resolves to the ds4-rocm release repo");

    // The asset name has to be derivable from the release tag alone: ds4-rocm
    // deliberately does not put the upstream ds4 commit in the filename, because
    // lemonade only knows the tag it is pinned to.
    expect(params.filename == "ds4-b0001-linux-rocm-gfx1151-x64.tar.gz",
           "asset filename is built from tag + detected arch");

    expect(install_filename("gfx1151", "b0002") == "ds4-b0002-linux-rocm-gfx1151-x64.tar.gz",
           "filename tracks the pinned version");

    // ds4-rocm publishes raw ISA names, not family names, so the arch must be
    // passed through verbatim rather than collapsed to a family.
    expect(install_filename("gfx1151", "b0001").find("gfx1151") != std::string::npos,
           "arch is used verbatim, not collapsed to a family target");

    // Install must refuse an architecture we publish nothing for, rather than
    // composing a plausible-looking asset name that 404s. Model filtering
    // normally keeps such hosts away, but the install path cannot assume it ran.
    expect(throws_for_arch("gfx1150"),
           "install refuses an unsupported architecture");
    expect(throws_for_arch("gfx942"),
           "install refuses a CDNA architecture");
    expect(!throws_for_arch("gfx1151"),
           "install still resolves for the supported architecture");
    expect(throws_for_arch(""),
           "install refuses a host with no ROCm architecture");

    expect(throws_for_backend("system"),
           "the removed system variant is rejected");
    expect(throws_for_backend("vulkan"),
           "unsupported backend throws rather than resolving a bogus asset");

    // Only gfx1151 is validated upstream; the descriptor must not advertise
    // architectures ds4-rocm does not publish, or install would resolve an
    // asset that does not exist.
    expect(SystemInfo::backend_supports_arch("ds4", "rocm", "gfx1151"),
           "rocm is published for gfx1151");
    expect(!SystemInfo::backend_supports_arch("ds4", "rocm", "gfx1150"),
           "rocm is not advertised for unvalidated architectures");

    // The support row is Linux-only, and the direct backend-install endpoint
    // does not go through model filtering, so a Windows gfx1151 host would
    // otherwise pass the arch check and resolve the Linux asset.
    expect(definition_publishes_for_os("linux"),
           "definition support row publishes for linux");
    expect(!definition_publishes_for_os("windows"),
           "definition support row does not publish for windows");
    expect(!definition_publishes_for_os("macos"),
           "definition support row does not publish for macos");
    for (const char* os : {"windows", "macos"}) {
        expect(install_error("rocm", os, "gfx1151") ==
                   std::string("ds4 backend 'rocm' publishes no build for ") + os + "; Linux only",
               std::string("install refuses a gfx1151 host on ") + os);
    }

    // ds4-server parses left-to-right, so anything appended after Lemonade's
    // own flags wins. Every argument Lemonade manages must be refused.
    const auto reserved = definition_reserved_flags();
    for (const char* flag : {"-m", "--model", "--host", "--port", "-c", "--ctx"}) {
        expect(!lemon::utils::validate_custom_args(std::string(flag) + " x", reserved).empty(),
               std::string("rejects managed argument ") + flag);
    }

    // ds4-server also selects its compute device with these, which would run
    // the child somewhere other than the backend Lemonade is tracking.
    for (const char* flag : {"--backend", "--cpu", "--metal", "--rocm", "--cuda"}) {
        expect(!lemon::utils::validate_custom_args(std::string(flag) + " x", reserved).empty(),
               std::string("rejects backend-selection argument ") + flag);
    }

    expect(lemon::utils::validate_custom_args("--power 100", reserved).empty(),
           "allows arguments Lemonade does not manage");
    expect(lemon::utils::validate_custom_args("", reserved).empty(),
           "allows empty custom arguments");

    if (failures == 0) {
        std::cout << "All ds4 install-param tests passed" << std::endl;
        return 0;
    }
    std::cout << failures << " test(s) failed" << std::endl;
    return 1;
}
