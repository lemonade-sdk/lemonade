#include "lemon/server/hybrid_context_policy.h"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace lemon::hybrid_context_policy;

namespace {
int failed = 0;
void check(bool ok, const char* description) {
    std::cout << (ok ? "PASS " : "FAIL ") << description << '\n';
    failed += !ok;
}
template<class Fn> bool rejects(Fn fn) {
    try { fn(); } catch (const std::runtime_error&) { return true; }
    return false;
}
}

int main() {
    Observation o{600, 1200, 6};
    check(!needs_checkpoint(1000, o, 1260, 6), "healthy role continues using its context");
    check(needs_checkpoint(1000, o, 1301, 6), "observed prompt plus incoming bytes triggers checkpoint");
    check(needs_checkpoint(1000, o, 1200, 8), "new tool results add chat-template overhead");
    check(!needs_checkpoint(1000, Observation{}, 999999, 100, 990), "no unreliable initial prompt estimate");
    check(needs_checkpoint(1000, o, 1200, 6, 720), "actual occupied KV can trigger rollover");
    check(!needs_checkpoint(1000, o, 1200, 6, 690), "occupied KV below the guard does not trigger rollover");
    check(!needs_checkpoint(2000, o, 1301, 6), "capacity follows actual backend slot, not a fixed token count");
    check(needs_checkpoint(1000, Observation{830, 1200, 6}, 1200, 6), "previous measured prompt already near limit");
    check(needs_checkpoint(1000, o, static_cast<size_t>(-1), 6), "huge unexpected tool output cannot overflow estimate");
    check(!needs_checkpoint(0, o, 1301, 6), "unknown capacity is never treated as sufficient");
    check(!needs_checkpoint(1000, Observation{300, 1500, 10}, 200, 1), "compaction does not invent positive growth");
    // Each slot has independent observations; normal Worker recycling must
    // clear its old observation while leaving Orchestrator pressure intact.
    Observation orchestrator{900, 1000, 5};
    Observation worker{680, 1000, 5};
    check(needs_checkpoint(1000, orchestrator, 1000, 5), "orchestrator can independently require rollover");
    check(!needs_checkpoint(1000, worker, 1000, 5), "healthy worker is not reset with orchestrator");
    worker = {};
    check(!needs_checkpoint(1000, worker, 1000, 5, 990), "recycled worker cannot inherit stale KV observations");

    validate_checkpoint(false, true, false, "Validated task facts and remaining work", 8192);
    check(rejects([]{ validate_checkpoint(true, true, false, "ok", 8192); }), "pending tool call forbids checkpoint");
    check(rejects([]{ validate_checkpoint(false, false, false, "partial", 8192); }), "length-limited summary refused");
    check(rejects([]{ validate_checkpoint(false, true, true, "result", 8192); }), "summary cannot contain new tool calls");
    check(rejects([]{ validate_checkpoint(false, true, false, "  \n", 8192); }), "empty checkpoint refused");
    check(rejects([]{ validate_checkpoint(false, true, false, "123456", 5); }), "oversized checkpoint refused");
    check(saturating_add(700, std::numeric_limits<uint64_t>::max()) ==
              std::numeric_limits<int64_t>::max(), "saturating growth estimate avoids integer wrap");
    return failed ? 1 : 0;
}
