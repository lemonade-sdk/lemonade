#pragma once

#include <cstddef>
#include <string>
#include <utility>

#include <httplib.h>

#include "lemon/routing_capacity.h"

namespace lemon {

// Offset just past the first complete SSE event in `buffer`, or npos. Events
// are terminated by a blank line; both LF and CRLF framings are accepted.
inline std::size_t sse_event_end(const std::string& buffer) {
    const std::size_t lf = buffer.find("\n\n");
    const std::size_t crlf = buffer.find("\r\n\r\n");
    if (crlf != std::string::npos && (lf == std::string::npos || crlf < lf)) {
        return crlf + 4;
    }
    return lf == std::string::npos ? std::string::npos : lf + 2;
}

// Withholds a streaming attempt's bytes until its first SSE *data* event is
// known (#2959). A backend length rejection is delivered as a data event with
// no data event before it, so it can be swallowed and the request re-routed
// while the client has still seen nothing that matters.
//
// Three things this must get right:
//  - SSE comments (`: ping` keep-alives, which StreamingProxy can emit before
//    the backend's HTTP status is even known, since it assumes 200 until told
//    otherwise) carry no payload. They are forwarded immediately to preserve
//    liveness but do NOT settle the decision — buffering them instead would let
//    a client or proxy time out during a long prefill.
//  - Once a rejection has been withheld, every later byte is swallowed too. The
//    relays do honor a false write return and stop, but a source that ignored
//    it must not be able to push the withheld rejection through afterwards.
//  - A withheld attempt must not call done() on the inner sink: the caller is
//    about to stream a different candidate into that same sink.
//
// A rejection that arrives *after* real content was released cannot be undone,
// so it is passed through unchanged — the pre-existing behavior.
class ContextOverflowProbeSink {
public:
    explicit ContextOverflowProbeSink(httplib::DataSink& inner) : inner_(inner) {}

    bool overflow_detected() const { return overflow_; }
    const std::string& withheld_event() const { return withheld_; }

    bool write(const char* data, std::size_t length) {
        if (overflow_) {
            return false;
        }
        if (decided_) {
            return inner_.write(data, length);
        }

        buffer_.append(data, length);
        while (true) {
            const std::size_t end = sse_event_end(buffer_);
            if (end == std::string::npos) {
                return true;
            }
            std::string event = buffer_.substr(0, end);

            if (is_sse_comment(event)) {
                buffer_.erase(0, end);
                if (!inner_.write(event.c_str(), event.size())) {
                    return false;
                }
                continue;
            }

            if (routing_capacity::sse_event_is_context_overflow(event)) {
                overflow_ = true;
                decided_ = true;
                withheld_ = std::move(event);
                buffer_.clear();
                return false;
            }

            decided_ = true;
            return release();
        }
    }

    void done() {
        if (overflow_) {
            return;
        }
        settle();
        if (inner_.done) {
            inner_.done();
        }
    }

    void done_with_trailer(const httplib::Headers& trailer) {
        if (overflow_) {
            return;
        }
        settle();
        if (inner_.done_with_trailer) {
            inner_.done_with_trailer(trailer);
        } else if (inner_.done) {
            inner_.done();
        }
    }

    bool is_writable() const {
        return !inner_.is_writable || inner_.is_writable();
    }

private:
    // An SSE comment line — a keep-alive, never a payload.
    static bool is_sse_comment(const std::string& event) {
        const std::size_t first = event.find_first_not_of("\r\n");
        return first != std::string::npos && event[first] == ':';
    }

    void settle() {
        if (!decided_) {
            decided_ = true;
            release();
        }
    }

    bool release() {
        if (buffer_.empty()) {
            return true;
        }
        const bool ok = inner_.write(buffer_.c_str(), buffer_.size());
        buffer_.clear();
        return ok;
    }

    httplib::DataSink& inner_;
    std::string buffer_;
    std::string withheld_;
    bool decided_ = false;
    bool overflow_ = false;
};

// Point a DataSink's std::function members at `wrapper`. DataSink is neither
// copyable nor movable, so it is filled in place rather than returned.
template <typename Wrapper>
void bind_sink(httplib::DataSink& sink, Wrapper& wrapper) {
    sink.write = [&wrapper](const char* data, std::size_t len) {
        return wrapper.write(data, len);
    };
    sink.done = [&wrapper]() { wrapper.done(); };
    sink.done_with_trailer = [&wrapper](const httplib::Headers& trailer) {
        wrapper.done_with_trailer(trailer);
    };
    sink.is_writable = [&wrapper]() { return wrapper.is_writable(); };
}

} // namespace lemon
