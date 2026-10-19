#include <iostream>
#include <stdexcept>
#include <string>

#include "lemon/backends/backend_utils.h"
#include "lemon/backends/ds4/ds4.h"
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

lemon::backends::InstallParams install_params(const std::string& backend, const std::string& version,
                                              const std::string& arch) {
    const auto* spec = lemon::backends::ds4::spec();
    auto platform = lemon::backends::native_platform();
    platform.rocm_arch = arch;
    return spec->install_params_fn({*spec, spec->definition, "ds4", spec->recipe, spec->pin_recipe, backend, "",
                                    version, version, platform, lemon::backends::InstallPhase::Asset});
}

std::string install_filename(const std::string& arch, const std::string& version) {
    return install_params("rocm", version, arch).filename;
}

bool throws_for_backend(const std::string& backend) {
    try {
        install_params(backend, "b0001", "");
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

bool throws_for_arch(const std::string& arch) {
    try {
        install_params("rocm", "b0001", arch);
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

}  // namespace

int main() {
    const bool host_publishes =
        lemon::backends::ds4::publishes_for_os(lemon::get_current_os());

    if (host_publishes) {
        const auto params = install_params("rocm", "b0001", "gfx1151");

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
    } else {
        expect(throws_for_arch("gfx1151"),
               "install refuses a host OS ds4 publishes no build for");
    }

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
    expect(lemon::backends::ds4::publishes_for_os("linux"),
           "publishes for linux");
    expect(!lemon::backends::ds4::publishes_for_os("windows"),
           "does not publish for windows");
    expect(!lemon::backends::ds4::publishes_for_os("macos"),
           "does not publish for macos");

    // ds4-server parses left-to-right, so anything appended after Lemonade's
    // own flags wins. Every argument Lemonade manages must be refused.
    const auto& reserved = lemon::backends::ds4::reserved_custom_arg_flags();
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
