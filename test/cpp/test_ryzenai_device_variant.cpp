// Proves the ryzenai-llm device reporting follows the model variant. The backend
// descriptor declares DEVICE_NPU for the whole recipe, so without this mapping a
// -CPU model and a -Hybrid model both report the same device as a -NPU one.

#include "lemon/backends/ryzenai/ryzenai.h"
#include "lemon/model_types.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(const std::string& what, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) {
        ++failures;
    }
}

// Builds the device string the models route would print for this model name, so
// the expectation is the user-visible value and not an internal bitmask.
void expect(const std::string& model_name, const std::string& expected) {
    const lemon::DeviceType device =
        lemon::backends::ryzenai::device_for_variant(model_name);
    const std::string got = lemon::device_type_to_string(device);

    check(model_name + " -> " + expected + " (got " + got + ")", got == expected);
}

} // namespace

int main() {
    // Hybrid uses the NPU with the iGPU alongside it.
    expect("Qwen3-4B-Hybrid", "gpu|npu");
    expect("Llama-3.2-1B-Instruct-Hybrid", "gpu|npu");

    // CPU variants run on the CPU only.
    expect("Llama-3.2-1B-Instruct-CPU", "cpu");

    // NPU variants, and names with no variant suffix, stay on the NPU.
    expect("Llama-3.2-1B-Instruct-NPU", "npu");
    expect("Qwen3-0.6B-GGUF", "npu");

    // Only a suffix counts: the same word in the middle of a name does not.
    expect("Something-Hybrid-GGUF", "npu");

    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("all checks passed\n");
    return 0;
}
