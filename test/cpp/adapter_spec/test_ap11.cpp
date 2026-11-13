// AP11 (R11): device_values. Run: bash test/cpp/adapter_spec/run.sh ap11
#include "adapter_spec.h"
#include "check.h"

using namespace lemon::backends::adapter::specs;

static bool is(const DeviceValues& v, const std::string& d, const std::string& u) { return v.device == d && v.gpu_uuids == u; }

static int body() {
    // no card list: the old rule, unchecked
    CHECK(is(device_values("", "", {}), "0", "0"));
    CHECK(is(device_values("", "2", {}), "2", "2"));
    CHECK(is(device_values("1", "2", {}), "1", "1"));
    CHECK(is(device_values("GPU-x", "", {}), "GPU-x", "GPU-x"));
    // a card list: choose_device decides
    const std::vector<Gpu> inv = {{0, "GPU-eff8bb7c"}, {1, "GPU-52c983ba"}, {3, ""}};
    CHECK(is(device_values("1", "", inv), "1", "GPU-52c983ba"));
    CHECK(is(device_values("gpu-52c983ba", "", inv), "1", "GPU-52c983ba"));
    CHECK(is(device_values("", "", inv), "0", "GPU-eff8bb7c"));
    CHECK(is(device_values("", "3", inv), "3", "3"));     // a card without a uuid
    THROWS(device_values("2", "", inv), "2");
    THROWS(device_values("GPU-nope", "", inv), "GPU-nope");
    return report("ap11");
}

int main() {
    try {
        return body();
    } catch (const std::exception& e) {
        std::cerr << "ap11: uncaught exception (a check above did not run): " << e.what() << "\n";
        report("ap11");
        return 2;
    }
}
