#pragma once

/// HTTP forwarding shared by the mesh port (MeshPort.cpp) and the service
/// ingress front end (ServiceIngress.cpp). Internal to the SDK: the
/// listener that receives a request forwards it to a loopback service and
/// streams the answer back, chunk by chunk, with the upstream status and
/// Content-Type.

#include <httplib.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace lnsdk {

struct MeshProxyPipe;

/// Largest request body a proxying listener accepts.
inline constexpr size_t kProxyPayloadLimit = 256u * 1024u * 1024u;

/// The open requests of a listener: the handlers that are running and the pipes
/// that still stream a response after their handler returned. cancel_all()
/// aborts the pipes; the mesh port's switch from plain HTTP to TLS waits until
/// nothing is open, because stopping an httplib server cuts every chunked
/// response it is still writing.
struct MeshPortProxies {
    std::mutex mutex;
    std::vector<std::weak_ptr<MeshProxyPipe>> pipes;
    size_t handlers{0};
    bool switching{false};       // refuse new requests while the listener is replaced
    bool wake_when_idle{false};  // a switch is waiting for the open requests to finish
    std::function<void()> on_idle;

    /// Registers a handler. False while the listener is being replaced.
    bool enter() {
        std::lock_guard lock(mutex);
        if (switching) return false;
        ++handlers;
        return true;
    }
    void leave() {
        std::function<void()> wake;
        {
            std::lock_guard lock(mutex);
            --handlers;
            wake = idle_wake_locked();
        }
        if (wake) wake();
    }
    void add(const std::shared_ptr<MeshProxyPipe>& pipe) {
        std::lock_guard lock(mutex);
        prune_locked();
        pipes.push_back(pipe);
    }
    /// Called by a pipe's destructor, once its response is written or aborted.
    void released() {
        std::function<void()> wake;
        {
            std::lock_guard lock(mutex);
            wake = idle_wake_locked();
        }
        if (wake) wake();
    }
    /// Starts refusing new requests when nothing is open. Otherwise arranges for
    /// on_idle to run when the last open request finishes and returns false.
    bool begin_switch() {
        std::lock_guard lock(mutex);
        if (!idle_locked()) {
            wake_when_idle = true;
            return false;
        }
        wake_when_idle = false;
        switching = true;
        return true;
    }
    void end_switch() {
        std::lock_guard lock(mutex);
        switching = false;
    }
    void forget_switch() {
        std::lock_guard lock(mutex);
        switching = false;
        wake_when_idle = false;
    }
    void cancel_all();

private:
    void prune_locked() {
        pipes.erase(std::remove_if(pipes.begin(), pipes.end(), [](const auto& p) { return p.expired(); }), pipes.end());
    }
    bool idle_locked() {
        prune_locked();
        return handlers == 0 && pipes.empty();
    }
    std::function<void()> idle_wake_locked() {
        if (!wake_when_idle || !idle_locked()) return {};
        wake_when_idle = false;
        return on_idle;
    }
};

/// Answers `status` with {"error": message} as JSON and Cache-Control: no-store.
void reply_error(httplib::Response& res, int status, const std::string& message);

/// Encodes a parsed multipart request again with a fresh boundary. httplib parses
/// a multipart body into its form parts and leaves req.body empty, so a forwarded
/// multipart request needs this. Returns {Content-Type, body}.
std::pair<std::string, std::string> multipart_body(const httplib::Request& req);

/// Forwards `req` as `target` to the HTTP service at host:port with `headers` and
/// `body`, and streams the upstream response into `res` with its status and
/// Content-Type. A service that does not answer gets 502. The response streams
/// chunk by chunk, so server-sent events pass through as they arrive.
void proxy_request(const std::shared_ptr<MeshPortProxies>& proxies, const std::string& host, uint16_t port,
                   const httplib::Request& req, const std::string& target, httplib::Headers headers,
                   const std::string& body, httplib::Response& res);

}  // namespace lnsdk
