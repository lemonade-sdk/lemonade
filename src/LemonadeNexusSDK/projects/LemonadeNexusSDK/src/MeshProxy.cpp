#include "MeshProxy.hpp"

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <deque>
#include <random>
#include <thread>

namespace lnsdk {

namespace {

using json = nlohmann::json;

/// Response bytes buffered between the upstream reader and the listener.
constexpr size_t kPipeLimit = 1u * 1024u * 1024u;

struct FormPart {
    std::string name;
    std::string filename;
    std::string content_type;
    std::string content;
};

/// cpp-httplib 0.23 replaced Request::files with Request::form (fields and files);
/// distributions ship both generations, so read whichever the header provides.
template <typename Request>
auto form_parts(const Request& req, int) -> decltype(req.form.files, std::vector<FormPart>()) {
    std::vector<FormPart> parts;
    for (const auto& [name, field] : req.form.fields) parts.push_back({field.name, "", "", field.content});
    for (const auto& [name, file] : req.form.files) parts.push_back({file.name, file.filename, file.content_type, file.content});
    return parts;
}
template <typename Request>
std::vector<FormPart> form_parts(const Request& req, long) {
    std::vector<FormPart> parts;
    for (const auto& [name, part] : req.files) parts.push_back({part.name, part.filename, part.content_type, part.content});
    return parts;
}

}  // namespace

/// One forwarded request. A worker thread reads the upstream response into a bounded
/// queue; the listener streams the queue to the caller with the upstream status and
/// Content-Type.
struct MeshProxyPipe {
    std::mutex mutex;
    std::condition_variable changed;
    bool headers{false};
    bool finished{false};
    bool cancelled{false};
    int status{0};
    std::string content_type;
    std::deque<std::string> chunks;
    size_t buffered{0};
    std::unique_ptr<httplib::Client> client;
    std::thread worker;
    std::shared_ptr<MeshPortProxies> owner;

    void cancel() {
        {
            std::lock_guard lock(mutex);
            cancelled = true;
        }
        changed.notify_all();
        if (client) client->stop();
    }
    void join() {
        if (worker.joinable()) worker.join();
    }
    ~MeshProxyPipe() {
        if (worker.joinable()) {
            cancel();
            worker.join();
        }
        if (owner) owner->released();
    }
};

void MeshPortProxies::cancel_all() {
    std::vector<std::shared_ptr<MeshProxyPipe>> open;
    {
        std::lock_guard lock(mutex);
        for (const auto& weak : pipes) {
            if (auto pipe = weak.lock()) open.push_back(std::move(pipe));
        }
        pipes.clear();
    }
    for (const auto& pipe : open) pipe->cancel();
}

void reply_error(httplib::Response& res, int status, const std::string& message) {
    res.set_header("Cache-Control", "no-store");
    res.status = status;
    res.set_content(json{{"error", message}}.dump(), "application/json");
}

std::pair<std::string, std::string> multipart_body(const httplib::Request& req) {
    const auto parts = form_parts(req, 0);
    std::random_device random;
    std::string boundary;
    std::string body;
    for (int attempt = 0; attempt < 4; ++attempt) {
        boundary = "lemonade-mesh-port-" + std::to_string(random()) + std::to_string(random()) + std::to_string(random());
        body.clear();
        for (const auto& part : parts) {
            body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + part.name + "\"";
            if (!part.filename.empty()) {
                body += "; filename=\"" + part.filename + "\"\r\nContent-Type: " +
                    (part.content_type.empty() ? std::string("application/octet-stream") : part.content_type);
            }
            body += "\r\n\r\n" + part.content + "\r\n";
        }
        body += "--" + boundary + "--\r\n";
        bool collides = false;
        for (const auto& part : parts) {
            if (part.content.find(boundary) != std::string::npos) collides = true;
        }
        if (!collides) break;
    }
    return {"multipart/form-data; boundary=" + boundary, body};
}

void proxy_request(const std::shared_ptr<MeshPortProxies>& proxies, const std::string& host, uint16_t port,
                   const httplib::Request& req, const std::string& target, httplib::Headers headers,
                   const std::string& body, httplib::Response& res) {
    auto pipe = std::make_shared<MeshProxyPipe>();
    pipe->owner = proxies;
    pipe->client = std::make_unique<httplib::Client>(host, port);
    pipe->client->set_connection_timeout(10, 0);
    pipe->client->set_read_timeout(300, 0);
    pipe->client->set_write_timeout(300, 0);
    pipe->client->set_keep_alive(false);
    proxies->add(pipe);

    httplib::Request upstream;
    upstream.method = req.method;
    upstream.path = target;
    upstream.headers = std::move(headers);
    upstream.body = body;
    MeshProxyPipe* raw = pipe.get();
    upstream.response_handler = [raw](const httplib::Response& reply) {
        std::lock_guard lock(raw->mutex);
        raw->status = reply.status;
        raw->content_type = reply.get_header_value("Content-Type");
        raw->headers = true;
        raw->changed.notify_all();
        return !raw->cancelled;
    };
    upstream.content_receiver = [raw](const char* data, size_t size, uint64_t, uint64_t) {
        std::unique_lock lock(raw->mutex);
        raw->changed.wait(lock, [raw] { return raw->cancelled || raw->buffered < kPipeLimit; });
        if (raw->cancelled) return false;
        raw->chunks.emplace_back(data, size);
        raw->buffered += size;
        raw->changed.notify_all();
        return true;
    };
    // The worker holds a raw pointer: every path below joins it before the pipe is freed.
    raw->worker = std::thread([raw, upstream = std::move(upstream)] {
        auto result = raw->client->send(upstream);
        (void)result;
        std::lock_guard lock(raw->mutex);
        raw->finished = true;
        raw->changed.notify_all();
    });

    {
        std::unique_lock lock(pipe->mutex);
        pipe->changed.wait(lock, [&] { return pipe->headers || pipe->finished || pipe->cancelled; });
    }
    res.set_header("Cache-Control", "no-store");
    bool ready = false;
    int status = 0;
    std::string type;
    {
        std::lock_guard lock(pipe->mutex);
        ready = pipe->headers && !pipe->cancelled;
        status = pipe->status;
        type = pipe->content_type;
    }
    if (!ready) {
        pipe->cancel();
        pipe->join();
        reply_error(res, 502, "Cannot reach that service");
        return;
    }
    res.status = status;
    if (status == 204 || status == 304) {
        pipe->join();
        return;
    }
    res.set_chunked_content_provider(
        type.empty() ? "application/octet-stream" : type,
        [pipe](size_t, httplib::DataSink& sink) {
            std::string chunk;
            {
                std::unique_lock lock(pipe->mutex);
                pipe->changed.wait(lock, [&] { return !pipe->chunks.empty() || pipe->finished || pipe->cancelled; });
                if (pipe->chunks.empty()) {
                    lock.unlock();
                    sink.done();
                    return true;
                }
                chunk = std::move(pipe->chunks.front());
                pipe->chunks.pop_front();
                pipe->buffered -= chunk.size();
            }
            pipe->changed.notify_all();
            return sink.write(chunk.data(), chunk.size());
        },
        [pipe](bool) {
            pipe->cancel();
            pipe->join();
        });
}

}  // namespace lnsdk
