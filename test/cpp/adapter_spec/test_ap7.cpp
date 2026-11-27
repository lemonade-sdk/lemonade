// AP7 (R10): budget verb. Run: bash test/cpp/adapter_spec/run.sh ap7
#include "adapter_spec.h"
#include "check.h"

using namespace lemon::backends::adapter::specs;

static int body() {
    CHECK(budget_request("GPU-a", 4096) == (json{{"device", "GPU-a"}, {"mib", 4096}}));
    CHECK(budget_request("GPU-a", 0) == (json{{"device", "GPU-a"}, {"mib", 0}}));
    THROWS(budget_request("", 10), "device");
    THROWS(budget_request("GPU-a", -1), "mib");

    auto ok = parse_budget_reply(json{{"accepted", true}, {"now_bytes", 4294967296LL}});
    CHECK(ok.accepted && ok.now_bytes == 4294967296ULL && ok.reason.empty());
    auto no = parse_budget_reply(json{{"accepted", false}, {"reason", "experts pinned"}});
    CHECK(!no.accepted && no.reason == "experts pinned");
    auto no2 = parse_budget_reply(json{{"accepted", false}});
    CHECK(!no2.accepted && no2.reason.empty());

    THROWS(parse_budget_reply(json::array()), "");
    THROWS(parse_budget_reply(json::object()), "accepted");
    THROWS(parse_budget_reply(json{{"accepted", "yes"}}), "accepted");
    THROWS(parse_budget_reply(json{{"accepted", true}}), "now_bytes");
    THROWS(parse_budget_reply(json{{"accepted", true}, {"now_bytes", -3}}), "now_bytes");
    THROWS(parse_budget_reply(json{{"accepted", true}, {"now_bytes", 2.5}}), "now_bytes");
    return report("ap7");
}

int main() {
    try {
        return body();
    } catch (const std::exception& e) {
        std::cerr << "ap7: uncaught exception (a check above did not run): " << e.what() << "\n";
        report("ap7");
        return 2;
    }
}
