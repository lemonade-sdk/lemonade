#include <iostream>
#include <stdexcept>
#include <string>

#include "lemon/backends/ds4/ds4.h"
#include "lemon/backends/ds4/ds4_server.h"
#include "lemon/system_info.h"
#include "lemon/utils/custom_args.h"

using lemon::SystemInfo;
using lemon::backends::Ds4Server;

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

std::string install_filename(const std::string& arch, const std::string& version) {
    SystemInfo::set_rocm_arch_override(arch);
    const auto params = Ds4Server::get_install_params("rocm", version);
    SystemInfo::set_rocm_arch_override("");
    return params.filename;
}

bool throws_for_backend(const std::string& backend) {
    try {
        Ds4Server::get_install_params(backend, "b0001");
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

bool throws_for_arch(const std::string& arch) {
    SystemInfo::set_rocm_arch_override(arch);
    bool threw = false;
    try {
        Ds4Server::get_install_params("rocm", "b0001");
    } catch (const std::exception&) {
        threw = true;
    }
    SystemInfo::set_rocm_arch_override("");
    return threw;
}

}  // namespace

int main() {
    // get_install_params() rejects the host OS before it looks at the
    // architecture, so asset resolution can only be asserted where ds4 actually
    // publishes. Everywhere else the only correct outcome is a refusal.
    const bool host_publishes =
        lemon::backends::ds4::publishes_for_os(lemon::get_current_os());

    if (host_publishes) {
        SystemInfo::set_rocm_arch_override("gfx1151");
        const auto params = Ds4Server::get_install_params("rocm", "b0001");
        SystemInfo::set_rocm_arch_override("");

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

    // The no-GPU case (get_rocm_arch() == "") is guarded in get_install_params
    // but cannot be asserted here: an empty override clears the override rather
    // than setting an empty architecture, so the probe falls back to hardware.

    expect(lemon::backends::ds4::descriptor.experimental,
           "ds4 is marked experimental");

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
