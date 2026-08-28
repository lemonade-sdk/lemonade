#include <lemon/streaming_proxy.h>

#include <cstdio>
#include <string>
#include <vector>

struct TestResult {
    int passed = 0;
    int failed = 0;

    void ok(const std::string& name) {
        printf("[PASS] %s\n", name.c_str());
        ++passed;
    }

    void fail(const std::string& name) {
        printf("[FAIL] %s\n", name.c_str());
        ++failed;
    }
};

int main() {
    TestResult result;

    // Chunks split across reads: reassembled into complete SSE lines.
    {
        std::string buffer;
        std::vector<std::string> lines;
        auto collect = [&](const std::string& line) { lines.push_back(line); };

        buffer.append("data: {\"model\":\"upstream\"}");
        lemon::StreamingProxy::process_sse_lines(buffer, collect);
        buffer.append("\ndata: [DO");
        lemon::StreamingProxy::process_sse_lines(buffer, collect);
        buffer.append("NE]\n");
        lemon::StreamingProxy::process_sse_lines(buffer, collect);

        if (lines.size() == 2 && lines[0] == "data: {\"model\":\"upstream\"}" &&
            lines[1] == "data: [DONE]" && buffer.empty()) {
            result.ok("split chunks across reads");
        } else {
            result.fail("split chunks across reads");
        }
    }

    // Final frame without trailing newline stays buffered until flush.
    {
        std::string buffer;
        std::vector<std::string> lines;
        auto collect = [&](const std::string& line) { lines.push_back(line); };

        buffer.append("data: {\"choices\":[]}\n");
        lemon::StreamingProxy::process_sse_lines(buffer, collect);
        buffer.append("data: [DONE]");  // no trailing '\n'
        lemon::StreamingProxy::process_sse_lines(buffer, collect);

        if (lines.size() == 1 && lines[0] == "data: {\"choices\":[]}" &&
            buffer == "data: [DONE]") {
            // Flush remaining partial line (cloud streaming path).
            lemon::StreamingProxy::flush_sse_line_buffer(buffer, collect);
            if (lines.size() == 2 && lines[1] == "data: [DONE]" && buffer.empty()) {
                result.ok("flush final frame without trailing newline");
            } else {
                result.fail("flush final frame without trailing newline");
            }
        } else {
            result.fail("flush final frame without trailing newline");
        }
    }

    // Empty flush is a no-op.
    {
        std::string buffer;
        int calls = 0;
        lemon::StreamingProxy::flush_sse_line_buffer(buffer, [&](const std::string&) { ++calls; });
        if (calls == 0 && buffer.empty()) {
            result.ok("empty flush is no-op");
        } else {
            result.fail("empty flush is no-op");
        }
    }

    printf("\n%d passed, %d failed\n", result.passed, result.failed);
    return result.failed == 0 ? 0 : 1;
}
