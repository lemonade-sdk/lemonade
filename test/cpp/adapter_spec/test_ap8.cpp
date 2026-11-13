// AP8 (R5): ready states. Run: bash test/cpp/adapter_spec/run.sh ap8
#include "adapter_spec.h"
#include "check.h"

using namespace lemon::backends::adapter::specs;

static bool st(const Readiness& r, ReadyState s, double p) { return r.state == s && r.progress == p; }

static int body() {
    const json spec = {{"json_field", "status"}, {"ready_values", {"ok"}}, {"warm_values", {"warm"}},
                       {"loading_values", {"loading", "warming"}}, {"error_values", {"failed"}},
                       {"progress_field", "progress"}};

    // no json_field: any object is ready
    CHECK(st(ready_state(json{{"anything", 1}}, json::object()), ReadyState::Ready, -1));
    CHECK(st(ready_state(json::array(), json::object()), ReadyState::Starting, -1));
    CHECK(st(ready_state(json("up"), json::object()), ReadyState::Starting, -1));

    CHECK(st(ready_state(json{{"status", "ok"}}, spec), ReadyState::Ready, -1));
    CHECK(st(ready_state(json{{"status", "warm"}}, spec), ReadyState::Warm, -1));
    CHECK(st(ready_state(json{{"status", "failed"}}, spec), ReadyState::Error, -1));
    CHECK(st(ready_state(json{{"status", "booting"}}, spec), ReadyState::Starting, -1));
    CHECK(st(ready_state(json::object(), spec), ReadyState::Starting, -1));
    CHECK(st(ready_state(json{{"status", 3}}, spec), ReadyState::Starting, -1));

    // loading with progress: 0..1 as is, (1..100] as percent, anything else -1
    CHECK(st(ready_state(json{{"status", "loading"}, {"progress", 0.25}}, spec), ReadyState::Loading, 0.25));
    CHECK(st(ready_state(json{{"status", "warming"}, {"progress", 40}}, spec), ReadyState::Loading, 0.4));
    CHECK(st(ready_state(json{{"status", "loading"}, {"progress", 1}}, spec), ReadyState::Loading, 1.0));
    CHECK(st(ready_state(json{{"status", "loading"}}, spec), ReadyState::Loading, -1));
    CHECK(st(ready_state(json{{"status", "loading"}, {"progress", 250}}, spec), ReadyState::Loading, -1));
    CHECK(st(ready_state(json{{"status", "loading"}, {"progress", -0.1}}, spec), ReadyState::Loading, -1));
    CHECK(st(ready_state(json{{"status", "loading"}, {"progress", "half"}}, spec), ReadyState::Loading, -1));
    // progress is read only while loading
    CHECK(st(ready_state(json{{"status", "ok"}, {"progress", 0.5}}, spec), ReadyState::Ready, -1));
    return report("ap8");
}

int main() {
    try {
        return body();
    } catch (const std::exception& e) {
        std::cerr << "ap8: uncaught exception (a check above did not run): " << e.what() << "\n";
        report("ap8");
        return 2;
    }
}
