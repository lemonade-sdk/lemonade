// AP6 (R9): memory report. Run: bash test/cpp/adapter_spec/run.sh ap6
#include "adapter_spec.h"
#include "check.h"

using namespace lemon::backends::adapter::specs;

static int body() {
    const json body = {{"devices", {{{"uuid", "GPU-a"}, {"bytes", 1000}, {"evictable", true}},
                                     {{"uuid", "GPU-b"}, {"bytes", 250}}}},
                       {"pinned_host", 4000}, {"remote", 16000000000LL}, {"disk", 70000000000LL}};
    auto r = parse_memory_report(body);
    CHECK(r.devices.size() == 2);
    CHECK(r.devices[0].uuid == "GPU-a" && r.devices[0].bytes == 1000 && r.devices[0].evictable);
    CHECK(r.devices[1].uuid == "GPU-b" && r.devices[1].bytes == 250 && !r.devices[1].evictable);
    CHECK(r.pinned_host == 4000 && r.remote == 16000000000ULL && r.disk == 70000000000ULL);
    CHECK(local_bytes(r) == 1000 + 250 + 4000);  // remote and disk excluded
    CHECK(device_bytes(r, "GPU-b") == 250);
    CHECK(device_bytes(r, "gpu-A") == 1000);
    CHECK(device_bytes(r, "GPU-z") == 0);

    auto empty = parse_memory_report(json::object());
    CHECK(empty.devices.empty() && empty.pinned_host == 0 && empty.remote == 0 && empty.disk == 0 && local_bytes(empty) == 0);

    THROWS(parse_memory_report(json::array()), "");
    THROWS(parse_memory_report(json{{"pinned_host", -1}}), "pinned_host");
    THROWS(parse_memory_report(json{{"disk", 1.5}}), "disk");
    THROWS(parse_memory_report(json{{"remote", "big"}}), "remote");
    THROWS(parse_memory_report(json{{"devices", {{{"bytes", 5}}}}}), "uuid");
    THROWS(parse_memory_report(json{{"devices", {{{"uuid", ""}, {"bytes", 5}}}}}), "uuid");
    THROWS(parse_memory_report(json{{"devices", {{{"uuid", "GPU-a"}, {"bytes", -5}}}}}), "bytes");
    THROWS(parse_memory_report(json{{"devices", {{{"uuid", "GPU-a"}, {"bytes", 1}}, {{"uuid", "gpu-a"}, {"bytes", 2}}}}}), "uuid");
    THROWS(parse_memory_report(json{{"devices", "GPU-a"}}), "devices");
    return report("ap6");
}

int main() {
    try {
        return body();
    } catch (const std::exception& e) {
        std::cerr << "ap6: uncaught exception (a check above did not run): " << e.what() << "\n";
        report("ap6");
        return 2;
    }
}
