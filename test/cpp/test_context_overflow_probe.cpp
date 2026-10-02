// Unit tests for ContextOverflowProbeSink (#2959).
//
// Pins the three properties the streaming backstop rests on: a rejection as the
// first data event is withheld and never forwarded (even when the producer
// ignores the false write return), keep-alive comments stay live without
// settling the decision, and a rejection after real content passes through.
//
// Compile (standalone):
//   g++ -std=c++17 -I src/cpp/include -I build/_deps/json-src/include \
//       -I build/_deps/httplib-src test/cpp/test_context_overflow_probe.cpp \
//       src/cpp/server/routing_capacity.cpp -o test_context_overflow_probe

#include "lemon/context_overflow_probe_sink.h"

#include <cstdio>
#include <cstring>
#include <string>

using lemon::ContextOverflowProbeSink;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

namespace {

// Records everything a probe forwards, plus whether the response was closed.
// Shaped for lemon::bind_sink, since httplib::DataSink cannot be returned by
// value (it is neither copyable nor movable).
struct CapturingSink {
    std::string written;
    bool done_called = false;
    bool writable = true;

    bool write(const char* data, std::size_t len) {
        written.append(data, len);
        return true;
    }
    void done() { done_called = true; }
    void done_with_trailer(const httplib::Headers&) { done_called = true; }
    bool is_writable() const { return writable; }
};

const char* kOverflowEvent =
    "data: {\"error\":{\"message\":\"request (9000 tokens) exceeds the available "
    "context size (512 tokens), try increasing it\",\"status_code\":400}}\n\n";
const char* kContentEvent =
    "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n";

} // namespace

static void test_overflow_first_event_is_withheld() {
    CapturingSink client;
    httplib::DataSink inner;
    lemon::bind_sink(inner, client);
    ContextOverflowProbeSink probe(inner);

    const bool ok = probe.write(kOverflowEvent, std::strlen(kOverflowEvent));

    check("an overflow first event stops the relay", !ok);
    check("an overflow first event is flagged", probe.overflow_detected());
    check("nothing reaches the client", client.written.empty());
    check("the rejection is retained for the caller",
          probe.withheld_event().find("exceeds the available context size") !=
              std::string::npos);

    // A withheld attempt must leave the response open for the re-route.
    probe.done();
    check("a withheld attempt does not close the response", !client.done_called);
}

// The producer is not obliged to honor a false return. If it keeps writing, the
// withheld rejection must still never reach the client.
static void test_writes_after_overflow_keep_being_swallowed() {
    CapturingSink client;
    httplib::DataSink inner;
    lemon::bind_sink(inner, client);
    ContextOverflowProbeSink probe(inner);

    probe.write(kOverflowEvent, std::strlen(kOverflowEvent));
    const bool second = probe.write(kOverflowEvent, std::strlen(kOverflowEvent));
    const bool third = probe.write(kContentEvent, std::strlen(kContentEvent));
    probe.done_with_trailer({});

    check("a post-overflow write is refused", !second);
    check("a post-overflow content write is refused too", !third);
    check("no post-overflow byte reaches the client", client.written.empty());
    check("a withheld attempt does not close the response via trailer",
          !client.done_called);
}

static void test_content_passes_through() {
    CapturingSink client;
    httplib::DataSink inner;
    lemon::bind_sink(inner, client);
    ContextOverflowProbeSink probe(inner);

    check("a content first event is forwarded",
          probe.write(kContentEvent, std::strlen(kContentEvent)));
    check("no overflow is flagged", !probe.overflow_detected());
    check("the content reached the client", client.written == kContentEvent);

    // Once decided, later chunks stream straight through unbuffered.
    check("later chunks pass through", probe.write("data: x\n\n", 9));
    check("later chunks reach the client",
          client.written == std::string(kContentEvent) + "data: x\n\n");

    probe.done();
    check("a passed-through attempt closes the response", client.done_called);
}

// StreamingProxy emits `: ping` keep-alives while waiting on a slow prefill —
// and does so before the backend's HTTP status is known, so one can precede a
// rejection. A comment must not spend the re-route opportunity.
static void test_comment_does_not_settle_the_decision() {
    CapturingSink client;
    httplib::DataSink inner;
    lemon::bind_sink(inner, client);
    ContextOverflowProbeSink probe(inner);

    check("a keep-alive comment is forwarded immediately",
          probe.write(": ping\n\n", 8));
    check("the keep-alive reached the client", client.written == ": ping\n\n");
    check("the keep-alive settled nothing", !probe.overflow_detected());

    const bool ok = probe.write(kOverflowEvent, std::strlen(kOverflowEvent));
    check("a rejection after a keep-alive is still withheld", !ok);
    check("a rejection after a keep-alive is still flagged",
          probe.overflow_detected());
    check("only the keep-alive ever reached the client",
          client.written == ": ping\n\n");
}

static void test_multiple_comments_then_content() {
    CapturingSink client;
    httplib::DataSink inner;
    lemon::bind_sink(inner, client);
    ContextOverflowProbeSink probe(inner);

    const std::string burst = std::string(": ping\n\n") + ": ping\n\n" + kContentEvent;
    check("a comment burst followed by content is forwarded",
          probe.write(burst.c_str(), burst.size()));
    check("all of it reached the client in order", client.written == burst);
    check("content after comments settles the decision",
          !probe.overflow_detected());
}

// A rejection mid-stream cannot be undone — the client already has content — so
// it must be forwarded rather than swallowed.
static void test_overflow_after_content_passes_through() {
    CapturingSink client;
    httplib::DataSink inner;
    lemon::bind_sink(inner, client);
    ContextOverflowProbeSink probe(inner);

    probe.write(kContentEvent, std::strlen(kContentEvent));
    const bool ok = probe.write(kOverflowEvent, std::strlen(kOverflowEvent));

    check("a mid-stream rejection is forwarded", ok);
    check("no re-route is attempted mid-stream", !probe.overflow_detected());
    check("the client sees the mid-stream rejection",
          client.written.find("exceeds the available context size") !=
              std::string::npos);
}

// A rejection can be split across chunk boundaries; the probe must not decide
// on a partial event.
static void test_split_event_is_reassembled() {
    CapturingSink client;
    httplib::DataSink inner;
    lemon::bind_sink(inner, client);
    ContextOverflowProbeSink probe(inner);

    const std::string whole(kOverflowEvent);
    const std::size_t split = whole.size() / 2;
    check("a partial event is buffered, not decided",
          probe.write(whole.c_str(), split));
    check("nothing decided on a partial event", !probe.overflow_detected());
    check("nothing forwarded on a partial event", client.written.empty());

    const bool ok = probe.write(whole.c_str() + split, whole.size() - split);
    check("the reassembled rejection is withheld", !ok);
    check("the reassembled rejection is flagged", probe.overflow_detected());
    check("the reassembled rejection never reaches the client",
          client.written.empty());
}

// A stream that ends without ever completing an event must still deliver what
// it had, rather than silently dropping it.
static void test_incomplete_stream_is_flushed_on_done() {
    CapturingSink client;
    httplib::DataSink inner;
    lemon::bind_sink(inner, client);
    ContextOverflowProbeSink probe(inner);

    probe.write("data: partial", 13);
    check("an incomplete event is buffered", client.written.empty());
    probe.done();
    check("an incomplete event is flushed on done", client.written == "data: partial");
    check("done still closes the response", client.done_called);
}

static void test_crlf_framing() {
    CapturingSink client;
    httplib::DataSink inner;
    lemon::bind_sink(inner, client);
    ContextOverflowProbeSink probe(inner);

    std::string crlf_overflow(kOverflowEvent);
    crlf_overflow.replace(crlf_overflow.size() - 2, 2, "\r\n\r\n");
    const bool ok = probe.write(crlf_overflow.c_str(), crlf_overflow.size());

    check("a CRLF-framed rejection is withheld", !ok);
    check("a CRLF-framed rejection is flagged", probe.overflow_detected());
    check("a CRLF-framed rejection never reaches the client",
          client.written.empty());
}

int main() {
    test_overflow_first_event_is_withheld();
    test_writes_after_overflow_keep_being_swallowed();
    test_content_passes_through();
    test_comment_does_not_settle_the_decision();
    test_multiple_comments_then_content();
    test_overflow_after_content_passes_through();
    test_split_event_is_reassembled();
    test_incomplete_stream_is_flushed_on_done();
    test_crlf_framing();

    if (g_failures > 0) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("All checks passed\n");
    return 0;
}
