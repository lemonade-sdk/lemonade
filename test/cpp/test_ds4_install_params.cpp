#include <iostream>
#include <stdexcept>
#include <string>

#include "lemon/backends/ds4/ds4.h"
#include "lemon/backends/ds4/ds4_server.h"
#include "lemon/system_info.h"

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

}  // namespace

int main() {
    SystemInfo::set_rocm_arch_override("gfx1151");
    const auto params = Ds4Server::get_install_params("rocm", "b0001");
    SystemInfo::set_rocm_arch_override("");

    expect(params.repo == "lemonade-sdk/ds4-rocm", "rocm resolves to the ds4-rocm release repo");

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

    expect(lemon::backends::ds4::descriptor.experimental,
           "ds4 is marked experimental");

    if (failures == 0) {
        std::cout << "All ds4 install-param tests passed" << std::endl;
        return 0;
    }
    std::cout << failures << " test(s) failed" << std::endl;
    return 1;
}
