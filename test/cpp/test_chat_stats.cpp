#include <lemon_cli/chat_stats.h>

#include <cmath>
#include <cstdio>
#include <string>

using lemon_cli::TurnStats;
using nlohmann::json;

static int failed = 0;

static void check(bool ok, const char* message) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failed;
}

static bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

static bool contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

int main() {
    {
        // Streamed turn with no server speed data: decode speed must exclude prefill.
        TurnStats s;
        s.completion_tokens = 101;
        s.total_seconds = 10.5;
        lemon_cli::finalize_turn_stats(s, 0.5);
        check(near(s.ttft_seconds, 0.5), "client TTFT is kept");
        check(near(s.decode_tps, 10.0), "decode speed counts tokens after the first over decode time");
        check(!s.decode_is_overall, "streamed speed is not marked overall");
    }
    {
        // Non-streamed turn with no server speed data: only the overall average exists.
        TurnStats s;
        s.completion_tokens = 50;
        s.total_seconds = 5.0;
        lemon_cli::finalize_turn_stats(s, -1.0);
        check(s.ttft_seconds < 0.0, "TTFT stays unknown without streaming");
        check(near(s.decode_tps, 10.0) && s.decode_is_overall, "overall average is marked");
        check(contains(lemon_cli::format_turn_stats(s, false), "(overall)"),
              "overall average is labelled in the output");
    }
    {
        // llama.cpp timings win for speed; the client's TTFT wins for the wait.
        TurnStats s;
        s.completion_tokens = 40;
        s.total_seconds = 9.0;
        lemon_cli::apply_server_timings(
            json{{"prompt_ms", 200.0}, {"prompt_per_second", 800.0},
                 {"predicted_per_second", 55.5}, {"predicted_n", 40}},
            s);
        lemon_cli::finalize_turn_stats(s, 0.75);
        check(near(s.decode_tps, 55.5), "server decode speed is preferred");
        check(near(s.prefill_tps, 800.0), "server prefill speed is read");
        check(near(s.ttft_seconds, 0.75), "client TTFT overrides the server's prefill time");
    }
    {
        // Non-streamed llama.cpp reply: the server's prefill time is the TTFT.
        TurnStats s;
        lemon_cli::apply_server_timings(
            json{{"prompt_ms", 123.0}, {"predicted_per_second", 30.0}, {"predicted_n", 12}}, s);
        s.total_seconds = 2.0;
        lemon_cli::finalize_turn_stats(s, -1.0);
        check(near(s.ttft_seconds, 0.123), "server prefill time becomes TTFT");
        check(s.completion_tokens == 12, "token count falls back to predicted_n");
    }
    {
        // FastFlowLM and RyzenAI report speed inside usage.
        TurnStats s;
        lemon_cli::apply_usage(
            json{{"prompt_tokens", 20}, {"completion_tokens", 30},
                 {"prefill_duration_ttft", 0.4}, {"decoding_speed_tps", 22.0}},
            s);
        s.total_seconds = 3.0;
        lemon_cli::finalize_turn_stats(s, -1.0);
        check(s.prompt_tokens == 20 && s.completion_tokens == 30, "usage token counts are read");
        check(near(s.ttft_seconds, 0.4) && near(s.decode_tps, 22.0), "usage speed fields are read");
        check(s.prefill_tps == 0.0, "prefill speed stays unknown when the server gives none");
    }
    {
        // A single token has no decode interval, so no per-token speed is invented.
        TurnStats s;
        s.completion_tokens = 1;
        s.total_seconds = 1.0;
        lemon_cli::finalize_turn_stats(s, 0.9);
        check(s.decode_is_overall, "one token falls back to the overall average");
    }
    {
        // Non-numeric and missing fields are ignored instead of throwing.
        TurnStats s;
        lemon_cli::apply_usage(json{{"prompt_tokens", "x"}}, s);
        lemon_cli::apply_server_timings(json("nope"), s);
        check(s.prompt_tokens == 0 && s.decode_tps == 0.0, "malformed stats are ignored");
    }
    {
        TurnStats s;
        s.prompt_tokens = 87;
        s.completion_tokens = 128;
        s.decode_tps = 45.3;
        s.ttft_seconds = 0.34;
        s.prefill_tps = 512.0;
        const std::string line = lemon_cli::format_turn_stats(s, false);
        check(line == "128 tokens | 45.3 tok/s | TTFT 340 ms | prefill 512.0 tok/s | 87 prompt",
              "full line format");
        s.ttft_seconds = 1.25;
        s.prefill_tps = 0.0;
        check(lemon_cli::format_turn_stats(s, false) ==
                  "128 tokens | 45.3 tok/s | TTFT 1.2 s | 87 prompt",
              "slow TTFT is shown in seconds and missing prefill is omitted");
    }

    return failed == 0 ? 0 : 1;
}
