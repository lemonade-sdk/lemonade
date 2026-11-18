#include "lemon/live_throughput.h"

#include <chrono>
#include <iostream>
#include <stdexcept>

void require(bool condition) {
    if (!condition) throw std::runtime_error("live throughput assertion failed");
}

int main() {
    using namespace std::chrono_literals;
    lemon::LiveThroughput meter;
    const auto t0 = lemon::LiveThroughput::Clock::now();
    require(meter.snapshot(t0).active_requests == 0);

    {
        auto request = meter.start();
        // Pre-fill, SSE heartbeats, metadata and usage do not call observe().
        // Merely opening a streaming request must not raise active_requests.
        require(meter.snapshot(t0 + 1s).active_requests == 0);
        require(meter.snapshot(t0 + 1s).estimated_tokens_per_second == 0);

        request.observe("abcd", t0 + 1s);
        request.observe("abcdefgh", t0 + 2s);
        const auto live = meter.snapshot(t0 + 2s);
        require(live.active_requests == 1);
        require(live.estimated_tokens_per_second > 2.9 && live.estimated_tokens_per_second < 3.1);

        {
            auto second = meter.start();
            require(meter.snapshot(t0 + 2s).active_requests == 1);
            second.observe("abcd", t0 + 2s);
            second.observe("abcd", t0 + 3s);
            const auto concurrent = meter.snapshot(t0 + 3s);
            require(concurrent.active_requests == 2);
            require(concurrent.estimated_tokens_per_second > 1);
        }
        require(meter.snapshot(t0 + 3s).active_requests == 1);
        require(meter.snapshot(t0 + 6s).active_requests == 0); // silent stream
        require(meter.snapshot(t0 + 6s).estimated_tokens_per_second == 0);
    }
    require(meter.snapshot(t0 + 3s).active_requests == 0); // request finished
    require(meter.snapshot(t0 + 3s).estimated_tokens_per_second == 0);

    {
        auto request = meter.start();
        request.observe("\xc3\xa4", t0);
        request.observe("abcd", t0 + 1s);
        const auto sample = meter.snapshot(t0 + 1s);
        require(sample.estimated_tokens_per_second > 1.9 && sample.estimated_tokens_per_second < 2.1);
    }
    std::cout << "Live throughput tests passed\n";
}
