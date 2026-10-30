#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "lemon/backends/fastflowlm/fastflowlm_models.h"

using lemon::backends::fastflowlm::flm_validation_error;
using json = nlohmann::json;

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

std::string dev04_error(const std::string& corelib_error) {
    return flm_validation_error(json{{"npu_platform", "dev04"},
                                     {"backend", "core"},
                                     {"corelib_npu_ok", false},
                                     {"corelib_error", corelib_error},
                                     {"ready", false}});
}

bool exposes_internals(const std::string& message) {
    for (const char* internal : {"corelib", "XRT", "NPU_STACK_TOO_OLD", "selftest", "Unsupported:"}) {
        if (message.find(internal) != std::string::npos) return true;
    }
    return false;
}

void expect_described(const std::string& corelib_error, const std::string& expected,
                      const std::string& label) {
    const std::string message = dev04_error(corelib_error);
    expect(message == expected, label + " (got: " + message + ")");
    expect(!exposes_internals(message), label + " exposes no internals");
}

}  // namespace

int main() {
    expect(flm_validation_error(json{{"npu_platform", "aie2p"}, {"ready", true}}).empty(),
           "a ready aie2p stack has no error");
    expect(flm_validation_error(json{{"npu_platform", "dev04"},
                                     {"corelib_npu_ok", true},
                                     {"ready", true}})
               .empty(),
           "a ready dev04 stack has no error");

    expect(flm_validation_error(json{{"npu_platform", "aie2p"},
                                     {"amd_device_found", true},
                                     {"npu_driver_ok", false},
                                     {"ready", false}}) == "NPU driver version is too old.",
           "aie2p reports an old driver");

    expect_described(
        "ryzenai_corelib_selftest_dependencies failed: Unsupported: the NPU driver "
        "32.0.20221.4000 is older than 32.0.20221.4401 (NPU_STACK_TOO_OLD): update the AMD NPU driver",
        "NPU driver version is too old (installed 32.0.20221.4000, requires 32.0.20221.4401 or newer).",
        "dev04 old driver");
    expect_described(
        "ryzenai_corelib_selftest_dependencies failed: Unsupported: XRT 2.25.0 is older than "
        "2.26.0 (NPU_STACK_TOO_OLD): update the AMD NPU driver",
        "NPU driver version is too old.", "dev04 old XRT");
    expect_described(
        "ryzenai_corelib_selftest_dependencies failed: Unsupported: the NPU driver's version "
        "could not be read (NPU_STACK_TOO_OLD): update the AMD NPU driver",
        "NPU driver version could not be verified; update the NPU driver.",
        "dev04 unreadable driver version");
    expect_described(
        "ryzenai_corelib_selftest_dependencies failed: Generic Failure: XRT is not usable",
        "NPU runtime is not working; reinstall the NPU driver.", "dev04 unusable XRT");
    expect_described("corelib has no usable NPU", "No usable NPU was found.", "dev04 no NPU");
    expect_described(
        "failed to load corelib library 'C:\\flm\\ryzenai_corelib.dll': error 126",
        "FastFlowLM installation is incomplete; reinstall the flm backend.",
        "dev04 broken install");
    expect_described("something corelib has never said before", "NPU validation failed.",
                     "dev04 unknown reason");
    expect_described("", "NPU validation failed.", "dev04 without a reason");

    const std::string both = flm_validation_error(
        json{{"corelib_npu_ok", false},
             {"corelib_error", "corelib has no usable NPU"},
             {"memlock_ok", false},
             {"ready", false}});
    expect(both == "Memlock limits are too low. No usable NPU was found.",
           "every failed check is reported (got: " + both + ")");

    expect(flm_validation_error(json{{"ready", false}}) == "NPU validation failed.",
           "an unexplained failure has a generic error");

    if (failures == 0) {
        std::cout << "All FLM validation-error tests passed" << std::endl;
        return 0;
    }
    std::cout << failures << " test(s) failed" << std::endl;
    return 1;
}
