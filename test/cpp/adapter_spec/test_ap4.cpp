// AP4 (R11): choose_device. Run: bash test/cpp/adapter_spec/run.sh ap4
#include "adapter_spec.h"
#include "check.h"

using namespace lemon::backends::adapter::specs;

static bool is(const DeviceChoice& d, int index, const std::string& uuid) { return d.index == index && d.uuid == uuid; }

static int body() {
    const std::vector<Gpu> inv = {{0, "GPU-eff8bb7c-8194"}, {1, "GPU-52c983ba-a85d"}, {2, "amd:0x67c7:0x1002:0x0b0c"}};

    CHECK(is(choose_device("1", "", inv), 1, "GPU-52c983ba-a85d"));
    CHECK(is(choose_device("", "2", inv), 2, "amd:0x67c7:0x1002:0x0b0c"));   // spec default
    CHECK(is(choose_device("", "", inv), 0, "GPU-eff8bb7c-8194"));           // else 0
    CHECK(is(choose_device("0", "2", inv), 0, "GPU-eff8bb7c-8194"));         // requested wins
    CHECK(is(choose_device("GPU-52c983ba-a85d", "0", inv), 1, "GPU-52c983ba-a85d"));
    CHECK(is(choose_device("gpu-52C983BA-a85d", "", inv), 1, "GPU-52c983ba-a85d"));  // case-insensitive, inventory spelling out
    CHECK(is(choose_device("  1 ", "", inv), 1, "GPU-52c983ba-a85d"));
    CHECK(is(choose_device("amd:0x67c7:0x1002:0x0b0c", "", inv), 2, "amd:0x67c7:0x1002:0x0b0c"));

    THROWS(choose_device("3", "", inv), "3");
    THROWS(choose_device("GPU-ffffffff", "", inv), "GPU-ffffffff");
    THROWS(choose_device("-1", "", inv), "-1");
    THROWS(choose_device("1.5", "", inv), "1.5");
    THROWS(choose_device("GPU-52c983ba", "", inv), "GPU-52c983ba");  // a prefix is not a match
    THROWS(choose_device("0", "", {}), "0");                          // no cards

    // an index names Gpu::index, not a position: sparse and unordered inventories
    const std::vector<Gpu> sparse = {{1, "GPU-one"}, {3, "GPU-three"}};
    CHECK(is(choose_device("3", "", sparse), 3, "GPU-three"));
    CHECK(is(choose_device("1", "", sparse), 1, "GPU-one"));
    THROWS(choose_device("2", "", sparse), "2");
    THROWS(choose_device("", "", sparse), "0");                       // the default 0 is not there
    const std::vector<Gpu> unordered = {{2, "GPU-x"}, {0, "GPU-y"}};
    CHECK(is(choose_device("0", "", unordered), 0, "GPU-y"));
    CHECK(is(choose_device("2", "", unordered), 2, "GPU-x"));
    // a number too large for int is a SpecError, not std::out_of_range
    THROWS(choose_device("99999999999999999999", "", inv), "99999999999999999999");
    return report("ap4");
}

int main() {
    try {
        return body();
    } catch (const std::exception& e) {
        std::cerr << "ap4: uncaught exception (a check above did not run): " << e.what() << "\n";
        report("ap4");
        return 2;
    }
}
