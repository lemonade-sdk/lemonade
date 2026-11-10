// AP10 (R9): memory_telemetry. Run: bash test/cpp/adapter_spec/run.sh ap10
#include "adapter_spec.h"
#include "check.h"

using namespace lemon::backends::adapter::specs;

static int body() {
    const json answer = {{"devices", {{{"uuid", "GPU-a"}, {"bytes", 1000}, {"evictable", true}}, {{"uuid", "GPU-b"}, {"bytes", 250}}}},
                         {"pinned_host", 4000}, {"remote", 16000}, {"disk", 70000}};
    const json t = memory_telemetry(answer);
    CHECK(t.is_object() && !t.contains("error"));
    CHECK(t.value("local_bytes", 0) == 1000 + 250 + 4000);
    CHECK(t.value("pinned_host", 0) == 4000 && t.value("remote", 0) == 16000 && t.value("disk", 0) == 70000);
    CHECK(t.contains("devices") && t["devices"].is_array() && t["devices"].size() == 2);
    CHECK(t["devices"][0] == (json{{"uuid", "GPU-a"}, {"bytes", 1000}, {"evictable", true}}));
    CHECK(t["devices"][1] == (json{{"uuid", "GPU-b"}, {"bytes", 250}, {"evictable", false}}));

    const json e = memory_telemetry(json::object());
    CHECK(e == (json{{"devices", json::array()}, {"pinned_host", 0}, {"remote", 0}, {"disk", 0}, {"local_bytes", 0}}));

    // a bad answer is reported, never thrown
    const json bad = memory_telemetry(json{{"pinned_host", -1}});
    CHECK(bad.is_object() && bad.size() == 1 && bad.contains("error") && bad["error"].get<std::string>().find("pinned_host") != std::string::npos);
    const json bad2 = memory_telemetry(json::array());
    CHECK(bad2.is_object() && bad2.contains("error"));
    const json bad3 = memory_telemetry(json("text"));
    CHECK(bad3.is_object() && bad3.contains("error"));
    return report("ap10");
}

int main() {
    try {
        return body();
    } catch (const std::exception& e) {
        std::cerr << "ap10: uncaught exception (a check above did not run): " << e.what() << "\n";
        report("ap10");
        return 2;
    }
}
