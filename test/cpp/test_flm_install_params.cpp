#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/fastflowlm/fastflowlm.h"
#include "lemon/backends/fastflowlm/fastflowlm_server.h"
#include "lemon/system_info.h"

using lemon::SystemInfo;
using lemon::backends::BackendUtils;
using lemon::backends::FastFlowLMServer;
using lemon::backends::InstallParams;

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

void set_flm_bin(const std::string& value) {
#ifdef _WIN32
    _putenv_s("LEMONADE_FLM_NPU_BIN", value.c_str());
#else
    if (value.empty()) {
        unsetenv("LEMONADE_FLM_NPU_BIN");
    } else {
        setenv("LEMONADE_FLM_NPU_BIN", value.c_str(), 1);
    }
#endif
}

std::string install_error(const std::string& family, const std::string& version) {
    SystemInfo::set_npu_family_override(family);
    std::string error;
    try {
        FastFlowLMServer::get_install_params("npu", version);
    } catch (const std::exception& e) {
        error = e.what();
    }
    SystemInfo::set_npu_family_override("");
    return error;
}

}  // namespace

int main(int, char** argv) {
    set_flm_bin("");
    const std::string default_pin = BackendUtils::get_backend_version("flm", "npu");

    SystemInfo::set_npu_family_override("XDNA2");
    const InstallParams xdna2 = FastFlowLMServer::get_install_params("npu", default_pin);
    SystemInfo::set_npu_family_override("");
    expect(xdna2.repo == "ROCm/FastFlowLM", "XDNA2 installs the upstream release");
    expect(xdna2.version_override.empty(), "XDNA2 uses the pinned tag as-is");
#ifdef _WIN32
    expect(xdna2.filename == "fastflowlm_" + default_pin.substr(1) + "_windows_amd64.zip",
           "XDNA2 asset is the upstream Windows zip");
#else
    expect(xdna2.filename == "fastflowlm_" + default_pin.substr(1) + "_linux.tar.gz",
           "XDNA2 asset is the upstream Linux tarball");
#endif

    const std::string refusal = install_error("XDNA3", default_pin);
    expect(!refusal.empty(), "XDNA3 refuses to download the upstream release");
    expect(refusal.find("flm.npu_bin") != std::string::npos,
           "XDNA3 refusal says how to supply FastFlowLM");

    const auto* ops = lemon::backends::fastflowlm::ops();
    SystemInfo::set_npu_family_override("XDNA3");
    const auto xdna3_state = ops->classify_unavailable("npu", "", "lemonade backends install flm:npu");
    SystemInfo::set_npu_family_override("XDNA2");
    const auto xdna2_state = ops->classify_unavailable("npu", "", "lemonade backends install flm:npu");
    SystemInfo::set_npu_family_override("");
    expect(xdna3_state && xdna3_state->state == "action_required",
           "XDNA3 without FastFlowLM is action_required, not installable");
    expect(xdna3_state && xdna3_state->action.find("lemonade config set flm.npu_bin=") == 0,
           "XDNA3 action is the config command");
    expect(!xdna2_state, "XDNA2 keeps the generic installable state");

    // A configured flm.npu_bin is a user-supplied FastFlowLM: nothing to refuse.
    set_flm_bin(argv[0]);
    expect(install_error("XDNA3", default_pin).empty(),
           "XDNA3 with flm.npu_bin set does not refuse");
    SystemInfo::set_npu_family_override("XDNA3");
    expect(!ops->classify_unavailable("npu", "", ""),
           "XDNA3 with flm.npu_bin set is not action_required");
    SystemInfo::set_npu_family_override("");
    set_flm_bin("");

    const auto& support = lemon::backends::fastflowlm::descriptor.support;
    expect(support.size() == 1 && support[0].backend == "npu", "single npu support row");
    if (!support.empty()) {
        const auto npu = support[0].devices.find("amd_npu");
        expect(npu != support[0].devices.end() && npu->second.count("XDNA2") == 1,
               "XDNA2 is supported");
        expect(npu != support[0].devices.end() && npu->second.count("XDNA3") == 1,
               "XDNA3 is supported");
        const auto gate = support[0].arch_gates.find("XDNA3");
        expect(gate != support[0].arch_gates.end() &&
                   gate->second.os == std::set<std::string>{"windows"},
               "XDNA3 is gated to Windows");
        expect(support[0].arch_gates.count("XDNA2") == 0, "XDNA2 is not gated");
    }

    if (failures == 0) {
        std::cout << "All FLM install-param tests passed" << std::endl;
        return 0;
    }
    std::cout << failures << " test(s) failed" << std::endl;
    return 1;
}
