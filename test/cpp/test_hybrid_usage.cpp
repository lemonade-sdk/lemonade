#include "lemon/server/hybrid_usage.h"

#include <iostream>
#include <string>

using lemon::hybrid_usage::Sample;
using lemon::hybrid_usage::Totals;
using lemon::hybrid_usage::extract;
using lemon::hybrid_usage::json;
using lemon::hybrid_usage::request_diagnostics;
using lemon::hybrid_usage::request_usage;

namespace {
int errors = 0;
void expect(bool ok, const std::string& label) {
    if (!ok) { std::cerr << "FAIL: " << label << '\n'; ++errors; }
    else { std::cout << "ok: " << label << '\n'; }
}
}

int main() {
    const json routing = {
        {"usage", {{"prompt_tokens", 100}, {"completion_tokens", 11},
                   {"prompt_tokens_details", {{"cached_tokens", 80}}}}},
        {"timings", {{"prompt_n", 20}, {"prompt_ms", 6.5}}}
    };
    const json reply = {
        {"usage", {{"prompt_tokens", 90}, {"completion_tokens", 17}}},
        {"timings", {{"cache_n", 70}, {"prompt_n", 20}, {"prompt_ms", 8.0}}}
    };
    Totals total;
    const Totals before = total;
    total.add(extract(routing), true, 35.0);
    total.add(extract(reply), false, 48.0, false);
    const json usage = request_usage(190, 28, total, before);
    expect(usage["prompt_tokens"] == 190 && usage["completion_tokens"] == 28,
           "all internal prompt and output tokens remain accounted for");
    expect(usage["prompt_tokens_details"]["cached_tokens"] == 150,
           "cache reads from routing and visible replies are reported");
    expect(usage["prompt_tokens"].get<int>() -
           usage["prompt_tokens_details"]["cached_tokens"].get<int>() == 40,
           "Pi's uncached input accounting is consistent with cached reads");
    const json details = request_diagnostics(total, before);
    expect(details["routing_calls"] == 1 && details["reply_calls"] == 1,
           "routing and visible model calls are distinguished");
    expect(details["evaluated_prompt_tokens_observed"] == 40 &&
           details["prefill_ms_observed"] == 14.5,
           "prefill evaluations and measured milliseconds are captured");
    expect(details["routing_wall_ms"] == 35.0 && details["reply_wall_ms"] == 48.0,
           "wall time is split by internal model call kind");

    expect(details["orchestrator"]["calls"] == 1 && details["worker"]["calls"] == 1,
           "each role reports its own inference count");
    expect(details["orchestrator"]["cached_tokens_observed"] == 80 &&
           details["worker"]["cached_tokens_observed"] == 70,
           "cache observations remain attributable to each KV slot");
    expect(details["worker"]["wall_ms"] == 48.0 &&
           details["orchestrator"]["prefill_ms_observed"] == 6.5,
           "role-level wall and prefill timings are preserved");

    // An unreported cache count is unknown, NOT a measured zero.
    const json unknown = {{"usage", {{"prompt_tokens", 120}, {"completion_tokens", 8}}}};
    total.add(extract(unknown), true, 30.0);
    expect(!request_usage(310, 36, total, before).contains("prompt_tokens_details"),
           "partial cache reporting does not silently pretend there were zero hits");
    expect(request_diagnostics(total, before)["cache_samples"] == 2 &&
           request_diagnostics(total, before)["model_calls"] == 3,
           "unknown cache samples stay visible in diagnostic coverage");

    const json impossible = {{"usage", {{"prompt_tokens", 2},
                          {"prompt_tokens_details", {{"cached_tokens", 20}}}}}};
    expect(!extract(impossible).cached.has_value(),
           "impossible cache counts are rejected");
    const json bad_type = {{"usage", {{"prompt_tokens", 10},
                           {"prompt_tokens_details", {{"cached_tokens", "10"}}}}}};
    expect(!extract(bad_type).cached.has_value(),
           "non-numeric cached token counts are not accepted");
    const json no_timings = {{"usage", {{"prompt_tokens", 60},
                              {"prompt_tokens_details", {{"cached_tokens", 45}}}}}};
    expect(extract(no_timings).evaluated.value_or(-1) == 15,
           "evaluated tokens can be inferred when explicit cache count is present");

    return errors ? 1 : 0;
}
