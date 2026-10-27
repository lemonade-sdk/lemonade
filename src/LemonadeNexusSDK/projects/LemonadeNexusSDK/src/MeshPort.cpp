#include <LemonadeNexusSDK/MeshPort.hpp>
#include <LemonadeNexusSDK/CertRenewal.hpp>
#include <LemonadeNexusSDK/LemonadeNexusClient.hpp>

#include <httplib.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <sodium.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <random>

#ifndef _WIN32
#include <cerrno>
#endif

namespace lnsdk {

namespace {

using json = nlohmann::json;

/// Header that carries a device's API key for a request to another device's name.
/// The mesh port sends it to that device as Authorization and never forwards the
/// caller's own Authorization to a peer.
constexpr const char* kPeerAuthorizationHeader = "X-Lemonade-Peer-Authorization";
constexpr size_t kPayloadLimit = 256u * 1024u * 1024u;
constexpr size_t kPipeLimit = 1u * 1024u * 1024u;

std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string strip_prefix_length(std::string address) {
    if (const auto slash = address.find('/'); slash != std::string::npos) address.resize(slash);
    return address;
}

const MeshPortMember* find_member(const std::vector<MeshPortMember>& members, std::string_view node_id) {
    for (const auto& member : members) {
        if (member.node_id == node_id) return &member;
    }
    return nullptr;
}

bool names_member(const MeshPortMember& member, std::string_view name) {
    return !name.empty() && (member.node_id == name || member.device_name == name);
}

bool reachable_peer(const MeshPortMember& member, std::string_view self_node_id) {
    return member.node_id != self_node_id && member.is_inference && member.is_online && member.can_connect;
}

/// The federation accepts either base64 spelling of a key, with or without the
/// "ed25519:" prefix.
bool same_key(const std::string& a, const std::string& b) {
    const auto canonical = [](std::string key) {
        if (key.rfind("ed25519:", 0) == 0) key.erase(0, 8);
        std::replace(key.begin(), key.end(), '-', '+');
        std::replace(key.begin(), key.end(), '_', '/');
        while (!key.empty() && key.back() == '=') key.pop_back();
        return key;
    };
    const auto left = canonical(a);
    return !left.empty() && left == canonical(b);
}

bool grants_connect(const std::vector<Assignment>& assignments, const std::string& self_key) {
    for (const auto& entry : assignments) {
        if (!same_key(entry.management_pubkey, self_key)) continue;
        for (const auto& permission : entry.permissions) {
            if (permission == "connect_private" || permission == "connect_shared" || permission == "admin") return true;
        }
    }
    return false;
}

void reply_error(httplib::Response& res, int status, const std::string& message) {
    res.set_header("Cache-Control", "no-store");
    res.status = status;
    res.set_content(json{{"error", message}}.dump(), "application/json");
}

bool is_models_path(const std::string& path) {
    return path == "/api/v1/models" || path == "/api/v0/models" || path == "/v1/models" || path == "/v0/models";
}

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

/// httplib parses a multipart body into its form parts and leaves req.body empty, so a
/// forwarded multipart request is encoded again with a fresh boundary.
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

/// Binds without SO_REUSEPORT, so a second process cannot share the port. Unix keeps
/// SO_REUSEADDR so the HTTP to TLS switch can rebind past TIME_WAIT connections; it
/// does not allow two listeners on one address. Windows uses SO_EXCLUSIVEADDRUSE.
void exclusive_socket_options(socket_t sock) {
    int opt = 1;
#ifdef _WIN32
    setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const void*>(&opt), sizeof(opt));
#endif
}

/// True when binding 127.0.0.1:`port` fails because another socket owns it.
bool port_in_use(uint16_t port) {
    const auto sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return false;
    exclusive_socket_options(sock);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool bound = ::bind(sock, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
#ifdef _WIN32
    const int code = bound ? 0 : WSAGetLastError();
    const bool in_use = code == WSAEADDRINUSE || code == WSAEACCES;
#else
    const bool in_use = !bound && errno == EADDRINUSE;
#endif
    httplib::detail::close_socket(sock);
    return in_use;
}

/// One forwarded request. A worker thread reads the upstream response into a bounded
/// queue; the listener streams the queue to the caller with the upstream status and
/// Content-Type.
struct Pipe {
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
    ~Pipe() {
        if (worker.joinable()) {
            cancel();
            worker.join();
        }
    }
};

}  // namespace

/// The open pipes, so stop() can abort streams that would otherwise hold the
/// listener open.
struct MeshPortProxies {
    std::mutex mutex;
    std::vector<std::weak_ptr<Pipe>> pipes;

    void add(const std::shared_ptr<Pipe>& pipe) {
        std::lock_guard lock(mutex);
        pipes.erase(std::remove_if(pipes.begin(), pipes.end(), [](const auto& p) { return p.expired(); }), pipes.end());
        pipes.push_back(pipe);
    }
    void cancel_all() {
        std::vector<std::shared_ptr<Pipe>> open;
        {
            std::lock_guard lock(mutex);
            for (const auto& weak : pipes) {
                if (auto pipe = weak.lock()) open.push_back(std::move(pipe));
            }
            pipes.clear();
        }
        for (const auto& pipe : open) pipe->cancel();
    }
};

namespace {

void proxy(MeshPortProxies& proxies, const std::string& host, uint16_t port, const httplib::Request& req,
           const std::string& target, httplib::Headers headers, const std::string& body, httplib::Response& res) {
    auto pipe = std::make_shared<Pipe>();
    pipe->client = std::make_unique<httplib::Client>(host, port);
    pipe->client->set_connection_timeout(10, 0);
    pipe->client->set_read_timeout(300, 0);
    pipe->client->set_write_timeout(300, 0);
    pipe->client->set_keep_alive(false);
    proxies.add(pipe);

    httplib::Request upstream;
    upstream.method = req.method;
    upstream.path = target;
    upstream.headers = std::move(headers);
    upstream.body = body;
    Pipe* raw = pipe.get();
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

httplib::Headers forwarded_headers(const httplib::Request& req, const std::string& authorization,
                                   const std::string& content_type) {
    httplib::Headers headers;
    if (!authorization.empty()) headers.emplace("Authorization", authorization);
    for (const char* name : {"Accept", "X-Client-Session-Id"}) {
        if (req.has_header(name)) headers.emplace(name, req.get_header_value(name));
    }
    if (!content_type.empty()) headers.emplace("Content-Type", content_type);
    return headers;
}

std::optional<json> get_json(const std::string& host, uint16_t port, const std::string& path,
                             const std::string& authorization) {
    httplib::Client client(host, port);
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(5, 0);
    httplib::Headers headers;
    if (!authorization.empty()) headers.emplace("Authorization", authorization);
    auto reply = client.Get(path, headers);
    if (!reply || reply->status != 200) return std::nullopt;
    auto parsed = json::parse(reply->body, nullptr, false);
    if (parsed.is_discarded()) return std::nullopt;
    return parsed;
}

std::vector<std::string> model_ids(const json& listing) {
    std::vector<std::string> ids;
    if (!listing.is_object() || !listing.contains("data") || !listing["data"].is_array()) return ids;
    for (const auto& model : listing["data"]) {
        if (model.is_object() && model.contains("id") && model["id"].is_string()) ids.push_back(model["id"].get<std::string>());
    }
    return ids;
}

int64_t epoch_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

// ---------------------------------------------------------------------------
// Routing rules
// ---------------------------------------------------------------------------

namespace mesh_port {

std::string device_label(std::string_view device_name) {
    std::string label;
    for (const unsigned char c : device_name) {
        if (std::isalnum(c)) label += static_cast<char>(std::tolower(c));
        else if (!label.empty() && label.back() != '-') label += '-';
    }
    while (!label.empty() && label.back() == '-') label.pop_back();
    if (label.size() > 63) label.resize(63);
    while (!label.empty() && label.back() == '-') label.pop_back();
    return label;
}

std::string local_domain_from_certificate(std::string_view domain) {
    const auto name = lower(domain);
    if (name.rfind("*.", 0) != 0) return "";
    const auto rest = name.substr(1);  // ".<group>.lo.<base>"
    const auto lo = rest.find(".lo.");
    if (lo == std::string::npos || lo == 0) return "";
    const auto group = rest.substr(1, lo - 1);
    const auto base = rest.substr(lo + 4);
    if (group.empty() || group.find('.') != std::string::npos || base.empty() || base.back() == '.') return "";
    for (const unsigned char c : rest) {
        if (!std::isalnum(c) && c != '-' && c != '.') return "";
    }
    if (rest.find("..") != std::string::npos) return "";
    return rest;
}

std::pair<int, std::string> host_target(std::string_view host, std::string_view sni, std::string_view local_domain,
                                        const std::vector<MeshPortMember>& members, std::string_view self_node_id) {
    auto name = lower(host);
    if (!name.empty() && name.front() == '[') {
        name = name.substr(0, name.find(']') + 1);
    } else if (const auto colon = name.rfind(':'); colon != std::string::npos && name.find(':') == colon) {
        name.resize(colon);
    }
    if (!sni.empty() && lower(sni) != name) return {421, "The TLS server name and the Host header name different hosts"};
    if (name == "127.0.0.1" || name == "localhost" || name == "[::1]") return {0, "local"};
    const auto domain = lower(local_domain);
    if (domain.empty() || name.size() <= domain.size() ||
        name.compare(name.size() - domain.size(), domain.size(), domain) != 0) {
        return {421, "This port serves only names under the group's local domain"};
    }
    const auto label = name.substr(0, name.size() - domain.size());
    if (label.find('.') != std::string::npos) return {404, "No device in your group is named " + name};
    for (const auto& member : members) {
        if (label != lower(member.node_id) && label != device_label(member.device_name)) continue;
        return {0, member.node_id == self_node_id ? std::string("local") : member.node_id};
    }
    return {404, "No device in your group is named " + name};
}

std::pair<int, std::string> forward_target(const std::vector<MeshPortMember>& members, std::string_view self_node_id,
                                           std::string_view node_id) {
    if (node_id == self_node_id) return {409, "Use this device's own service instead of forwarding to it"};
    const auto* target = find_member(members, node_id);
    if (!target) return {404, "That device is not in your group"};
    if (!target->is_inference) return {409, "That device is not an inference server"};
    if (!target->can_connect) return {403, "This device has no connect permission on that device"};
    if (!target->is_online) return {409, "That device is offline or not reachable from this device"};
    auto address = strip_prefix_length(target->tunnel_ip);
    if (address.empty()) return {409, "That device has no mesh address"};
    return {0, address};
}

MeshPortAutoRoute auto_route(const std::string& model, const std::vector<MeshPortMember>& members,
                             std::string_view self_node_id, const std::vector<std::string>& local_models,
                             const std::vector<MeshPortPeerModels>& peers) {
    if (model.empty()) return {"local", model};
    std::vector<const MeshPortMember*> candidates;
    const MeshPortMember* self = nullptr;
    for (const auto& member : members) {
        if (member.node_id == self_node_id) { self = &member; continue; }
        if (reachable_peer(member, self_node_id)) candidates.push_back(&member);
    }
    if (const auto slash = model.find('/'); slash != std::string::npos) {
        const auto prefix = model.substr(0, slash);
        const auto rest = model.substr(slash + 1);
        if (self && names_member(*self, prefix)) return {"local", rest};
        for (const auto* member : candidates) {
            if (names_member(*member, prefix)) return {member->node_id, rest};
        }
        for (const auto& member : members) {
            if (names_member(member, prefix)) {
                return {"", model, 409, "That device is offline, not an inference server, or not reachable from this device"};
            }
        }
    }
    if (std::find(local_models.begin(), local_models.end(), model) != local_models.end()) return {"local", model};
    const MeshPortPeerModels* best = nullptr;
    int best_rank = 3;  // 0 loaded and idle, 1 loaded and busy, 2 listed only
    for (const auto& peer : peers) {
        const bool reachable = std::any_of(candidates.begin(), candidates.end(),
            [&](const MeshPortMember* member) { return member->node_id == peer.node_id; });
        if (!reachable) continue;
        int rank = 3;
        if (peer.loaded.is_array()) {
            for (const auto& entry : peer.loaded) {
                if (entry.is_object() && entry.value("model_name", "") == model) rank = entry.value("is_busy", false) ? 1 : 0;
            }
        }
        if (rank == 3 && std::find(peer.available.begin(), peer.available.end(), model) != peer.available.end()) rank = 2;
        if (rank < best_rank || (rank == best_rank && rank < 3 && peer.latency_ms < best->latency_ms)) {
            best = &peer;
            best_rank = rank;
        }
    }
    if (best) return {best->node_id, model};
    return {"", model, 404, "No reachable device serves the model " + model};
}

json merge_models(const json& local, const std::vector<MeshPortPeerModels>& peers) {
    json result = local.is_object() ? local : json{{"object", "list"}, {"data", json::array()}};
    if (!result.contains("data") || !result["data"].is_array()) result["data"] = json::array();
    for (const auto& peer : peers) {
        const auto owner = peer.device_name.empty() ? peer.node_id : peer.device_name;
        for (const auto& id : peer.available) {
            result["data"].push_back({{"id", owner + "/" + id}, {"object", "model"},
                {"owned_by", owner}, {"nexus_node_id", peer.node_id}});
        }
    }
    return result;
}

json device_urls(const std::vector<MeshPortMember>& members, std::string_view self_node_id,
                 std::string_view local_domain, int port, bool tls) {
    json urls = json::array();
    if (port <= 0) return urls;
    if (local_domain.empty()) {
        urls.push_back({{"node_id", std::string(self_node_id)}, {"device_name", ""},
            {"url", "http://127.0.0.1:" + std::to_string(port)}, {"self", true}});
        return urls;
    }
    const auto suffix = std::string(local_domain) + ":" + std::to_string(port);
    const std::string scheme = tls ? "https://" : "http://";
    for (const auto& member : members) {
        const bool self = member.node_id == self_node_id;
        if (!self && !reachable_peer(member, self_node_id)) continue;
        const auto label = member.device_name.empty() ? lower(member.node_id) : device_label(member.device_name);
        if (label.empty()) continue;
        urls.push_back({{"node_id", member.node_id}, {"device_name", member.device_name},
            {"url", scheme + label + suffix}, {"self", self}});
    }
    return urls;
}

std::optional<std::pair<std::string, uint16_t>> parse_local_service_url(std::string_view url) {
    constexpr std::string_view scheme = "http://";
    if (url.substr(0, scheme.size()) != scheme) return std::nullopt;
    auto rest = url.substr(scheme.size());
    if (!rest.empty() && rest.back() == '/') rest.remove_suffix(1);
    std::string host;
    std::string_view port_text;
    for (const std::string_view candidate : {"127.0.0.1:", "localhost:", "[::1]:"}) {
        if (rest.substr(0, candidate.size()) == candidate) {
            host = std::string(candidate.substr(0, candidate.size() - 1));
            port_text = rest.substr(candidate.size());
            break;
        }
    }
    if (host.empty() || port_text.empty() || port_text.size() > 5) return std::nullopt;
    uint32_t port = 0;
    for (const char c : port_text) {
        if (c < '0' || c > '9') return std::nullopt;
        port = port * 10 + static_cast<uint32_t>(c - '0');
    }
    if (port == 0 || port > 65535) return std::nullopt;
    if (host == "[::1]") host = "::1";
    return std::make_pair(host, static_cast<uint16_t>(port));
}

}  // namespace mesh_port

// ---------------------------------------------------------------------------
// Client backend
// ---------------------------------------------------------------------------

MeshPortBackend make_mesh_port_backend(LemonadeNexusClient& client) {
    MeshPortBackend backend;
    backend.self_node_id = [&client] { return client.node_id(); };
    backend.open_egress = [&client](const std::string& ip, uint16_t port) { return client.open_egress(ip, port); };
    backend.close_egress = [&client](const std::string& ip, uint16_t port) { client.close_egress(ip, port); };
    backend.load_members = [&client]() -> std::optional<std::vector<MeshPortMember>> {
        const auto self = client.node_id();
        if (self.empty()) return std::nullopt;
        const auto own = client.get_tree_node(self);
        if (!own.ok || own.value.parent_id.empty()) return std::nullopt;
        const auto children = client.get_children(own.value.parent_id);
        if (!children.ok) return std::nullopt;
        const auto peers = client.get_mesh_peers();
        const auto self_key = client.identity().pubkey_string();
        std::vector<MeshPortMember> members;
        for (const auto& child : children.value) {
            if (child.type != NodeType::Endpoint || child.id.empty()) continue;
            MeshPortMember member;
            member.node_id = child.id;
            member.device_name = child.hostname;
            member.tunnel_ip = child.tunnel_ip;
            member.is_inference = child.is_inference;
            const bool is_self = child.id == self;
            member.can_connect = is_self || grants_connect(child.assignments, self_key);
            member.is_online = is_self;
            for (const auto& peer : peers) {
                if (peer.node_id != child.id) continue;
                member.is_online = is_self || peer.is_online;
                member.latency_ms = peer.latency_ms > 0 ? static_cast<double>(peer.latency_ms) : 0.0;
                break;
            }
            members.push_back(std::move(member));
        }
        return members;
    };
    backend.ensure_certificate = [&client](const std::string& cert_dir) -> std::optional<MeshPortCertificate> {
        const auto result = client.renew_certificate_if_needed(std::string(kGroupLoopbackCertTarget), cert_dir);
        if (result.status == CertRenewalStatus::Failed) {
            spdlog::info("[MeshPort] group loopback certificate not available: {}", result.error);
            return std::nullopt;
        }
        return read_certificate_files(result.domain, result.fullchain_path, result.privkey_path);
    };
    return backend;
}

std::optional<MeshPortCertificate> read_certificate_files(const std::string& domain,
                                                          const std::string& fullchain_path,
                                                          const std::string& privkey_path) {
    constexpr std::uintmax_t kFileLimit = 1u * 1024u * 1024u;
    const auto read = [&](const std::string& path) -> std::optional<std::string> {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size > kFileLimit) return std::nullopt;
        std::ifstream input(path, std::ios::binary);
        if (!input) return std::nullopt;
        std::string text(static_cast<size_t>(size), '\0');
        if (!input.read(text.data(), static_cast<std::streamsize>(text.size()))) return std::nullopt;
        return text;
    };
    auto fullchain = read(fullchain_path);
    auto key = read(privkey_path);
    if (!fullchain || !key) {
        if (key) sodium_memzero(key->data(), key->size());
        return std::nullopt;
    }
    return MeshPortCertificate{domain, std::move(*fullchain), std::move(*key), 0};
}

// ---------------------------------------------------------------------------
// TLS material
// ---------------------------------------------------------------------------

struct MeshPort::Tls {
    struct Material {
        X509* leaf{nullptr};
        STACK_OF(X509)* chain{nullptr};
        EVP_PKEY* key{nullptr};
        int64_t not_after{0};  // Unix seconds
        Material() = default;
        Material(const Material&) = delete;
        Material& operator=(const Material&) = delete;
        ~Material() {
            if (leaf) X509_free(leaf);
            if (chain) sk_X509_pop_free(chain, X509_free);
            if (key) EVP_PKEY_free(key);
        }
    };

    std::mutex mutex;
    std::shared_ptr<const Material> material;

    /// Parses the PEM pair and checks that the key matches, the certificate is
    /// currently valid, and it covers names under `local_domain`.
    static std::shared_ptr<const Material> parse(const MeshPortCertificate& certificate,
                                                 const std::string& local_domain, std::string& error) {
        constexpr size_t kPemLimit = 1u * 1024u * 1024u;
        if (certificate.fullchain_pem.size() > kPemLimit || certificate.privkey_pem.size() > kPemLimit) {
            error = "the certificate PEM is too large";
            return nullptr;
        }
        auto material = std::make_shared<Material>();
        std::unique_ptr<BIO, decltype(&BIO_free)> chain_bio(
            BIO_new_mem_buf(certificate.fullchain_pem.data(), static_cast<int>(certificate.fullchain_pem.size())), BIO_free);
        if (!chain_bio) { error = "out of memory"; return nullptr; }
        material->leaf = PEM_read_bio_X509(chain_bio.get(), nullptr, nullptr, nullptr);
        if (!material->leaf) { error = "the certificate PEM is invalid"; return nullptr; }
        material->chain = sk_X509_new_null();
        if (!material->chain) { error = "out of memory"; return nullptr; }
        while (X509* extra = PEM_read_bio_X509(chain_bio.get(), nullptr, nullptr, nullptr)) {
            if (!sk_X509_push(material->chain, extra)) { X509_free(extra); error = "out of memory"; return nullptr; }
        }
        ERR_clear_error();
        std::unique_ptr<BIO, decltype(&BIO_free)> key_bio(
            BIO_new_mem_buf(certificate.privkey_pem.data(), static_cast<int>(certificate.privkey_pem.size())), BIO_free);
        if (!key_bio) { error = "out of memory"; return nullptr; }
        material->key = PEM_read_bio_PrivateKey(key_bio.get(), nullptr, nullptr, nullptr);
        if (!material->key) { ERR_clear_error(); error = "the private key PEM is invalid"; return nullptr; }
        if (X509_check_private_key(material->leaf, material->key) != 1) {
            ERR_clear_error();
            error = "the private key does not match the certificate";
            return nullptr;
        }
        if (X509_cmp_current_time(X509_get0_notBefore(material->leaf)) >= 0 ||
            X509_cmp_current_time(X509_get0_notAfter(material->leaf)) <= 0) {
            error = "the certificate is not currently valid";
            return nullptr;
        }
        int days = 0;
        int seconds = 0;
        if (ASN1_TIME_diff(&days, &seconds, nullptr, X509_get0_notAfter(material->leaf)) != 1) {
            error = "the certificate expiry cannot be read";
            return nullptr;
        }
        material->not_after = epoch_seconds() + static_cast<int64_t>(days) * 86400 + seconds;
        const auto probe = "mesh-port-check" + local_domain;
        if (X509_check_host(material->leaf, probe.c_str(), probe.size(), 0, nullptr) != 1) {
            error = "the certificate does not cover the group's local names";
            return nullptr;
        }
        return material;
    }

    void set(std::shared_ptr<const Material> next) {
        std::lock_guard lock(mutex);
        material = std::move(next);
    }

    /// Runs for each handshake, so a replaced certificate applies to new connections.
    static int certificate_callback(SSL* ssl, void* arg) {
        auto* tls = static_cast<Tls*>(arg);
        std::shared_ptr<const Material> current;
        {
            std::lock_guard lock(tls->mutex);
            current = tls->material;
        }
        if (!current) return 0;
        if (SSL_use_certificate(ssl, current->leaf) != 1 || SSL_use_PrivateKey(ssl, current->key) != 1 ||
            SSL_set1_chain(ssl, current->chain) != 1) {
            return 0;
        }
        return 1;
    }

    bool configure(SSL_CTX& context) {
        SSL_CTX_set_min_proto_version(&context, TLS1_2_VERSION);
        SSL_CTX_set_options(&context, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
        SSL_CTX_set_cert_cb(&context, &Tls::certificate_callback, this);
        return true;
    }
};

struct MeshPort::Listener {
    std::unique_ptr<httplib::Server> server;
    std::thread thread;
    bool tls{false};
};

// ---------------------------------------------------------------------------
// MeshPort
// ---------------------------------------------------------------------------

MeshPort::MeshPort(MeshPortBackend backend) : MeshPort(std::move(backend), Options{}) {}

MeshPort::MeshPort(MeshPortBackend backend, Options options)
    : backend_(std::move(backend)), options_(options), tls_(std::make_unique<Tls>()),
      proxies_(std::make_shared<MeshPortProxies>()) {}

MeshPort::~MeshPort() { stop(); }

MeshPortStartResult MeshPort::bind_listener(uint16_t port, bool tls) {
    auto listener = std::make_unique<Listener>();
    listener->tls = tls;
    if (tls) {
        auto* state = tls_.get();
        listener->server = std::make_unique<httplib::SSLServer>(
            [state](SSL_CTX& context) { return state->configure(context); });
        if (!listener->server->is_valid()) return MeshPortStartResult::BindFailed;
    } else {
        listener->server = std::make_unique<httplib::Server>();
    }
    auto& server = *listener->server;
    const auto serve = [this, tls](const httplib::Request& req, httplib::Response& res) { handle(req, res, tls); };
    server.Get(".*", serve);
    server.Post(".*", serve);
    server.Put(".*", serve);
    server.Patch(".*", serve);
    server.Delete(".*", serve);
    server.Options(".*", serve);
    server.set_socket_options(exclusive_socket_options);
    server.set_read_timeout(30, 0);
    server.set_write_timeout(300, 0);
    server.set_keep_alive_timeout(2);
    server.set_payload_max_length(kPayloadLimit);
    if (!server.bind_to_port("127.0.0.1", port)) {
        return port_in_use(port) ? MeshPortStartResult::PortInUse : MeshPortStartResult::BindFailed;
    }
    auto* raw = listener->server.get();
    listener->thread = std::thread([raw] { raw->listen_after_bind(); });
    raw->wait_until_ready();
    listener_ = std::move(listener);
    return MeshPortStartResult::Ok;
}

void MeshPort::stop_listener() {
    if (!listener_) return;
    listener_->server->stop();
    proxies_->cancel_all();
    if (listener_->thread.joinable()) listener_->thread.join();
    listener_.reset();
}

MeshPortStartResult MeshPort::start(uint16_t port, const std::string& local_service_url) {
    if (port == 0) port = kMeshPortDefaultPort;
    std::optional<std::pair<std::string, uint16_t>> local;
    if (!local_service_url.empty()) {
        local = mesh_port::parse_local_service_url(local_service_url);
        if (!local) return MeshPortStartResult::InvalidArgument;
    }
    std::lock_guard control(control_mutex_);
    bool running = false;
    {
        std::lock_guard lock(state_mutex_);
        running = running_;
    }
    // A refresh thread remains after a failed switch to TLS. Join it before taking
    // the lifecycle lock: it may be waiting for that lock.
    if (!running && refresh_thread_.joinable()) {
        {
            std::lock_guard lock(wake_mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        refresh_thread_.join();
    }
    std::lock_guard lifecycle(lifecycle_mutex_);
    {
        std::lock_guard lock(state_mutex_);
        if (running_) {
            if (port != port_) return MeshPortStartResult::AlreadyRunning;
            local_host_ = local ? local->first : std::string();
            local_port_ = local ? local->second : 0;
            return MeshPortStartResult::Ok;
        }
    }
    tls_->set(nullptr);
    const auto bound = bind_listener(port, false);
    if (bound != MeshPortStartResult::Ok) return bound;
    {
        std::lock_guard lock(state_mutex_);
        running_ = true;
        port_ = port;
        local_host_ = local ? local->first : std::string();
        local_port_ = local ? local->second : 0;
        local_domain_.clear();
        tls_state_ = "waiting";
        tls_domain_.clear();
        tls_error_.clear();
        certificate_expires_at_ = 0;
        certificate_pending_ = !cert_dir_.empty();
        error_.clear();
        selection_target_.clear();
        selection_key_.clear();
    }
    {
        std::lock_guard lock(wake_mutex_);
        stopping_ = false;
    }
    refresh_thread_ = std::thread(&MeshPort::refresh_loop, this);
    return MeshPortStartResult::Ok;
}

void MeshPort::stop() {
    std::lock_guard control(control_mutex_);
    std::thread refresher;
    std::map<std::string, std::string> egress;
    {
        std::lock_guard lifecycle(lifecycle_mutex_);
        {
            std::lock_guard lock(wake_mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        refresher = std::move(refresh_thread_);
        stop_listener();
        tls_->set(nullptr);
        std::lock_guard lock(state_mutex_);
        running_ = false;
        port_ = 0;
        selection_target_.clear();
        selection_key_.clear();
        members_.clear();
        local_domain_.clear();
        tls_state_ = "off";
        tls_domain_.clear();
        egress.swap(egress_ips_);
    }
    // Joined without lifecycle_mutex_: the refresh thread takes it to install a certificate.
    if (refresher.joinable()) refresher.join();
    {
        std::lock_guard lock(cache_mutex_);
        peer_cache_.clear();
        local_models_.clear();
        local_models_authorization_.clear();
        local_models_fetched_ = {};
    }
    if (backend_.close_egress) {
        for (const auto& [node, address] : egress) backend_.close_egress(address, kMeshPortPeerServicePort);
    }
}

void MeshPort::refresh_loop() {
    while (true) {
        refresh_now();
        std::unique_lock lock(wake_mutex_);
        wake_.wait_for(lock, options_.refresh_interval, [this] { return stopping_ || refresh_requested_; });
        if (stopping_) return;
        refresh_requested_ = false;
    }
}

void MeshPort::refresh_now() {
    std::lock_guard refresh(refresh_mutex_);
    try {
        refresh_members();
        check_certificate();
    } catch (const std::exception& e) {
        spdlog::warn("[MeshPort] refresh failed: {}", e.what());
    }
}

void MeshPort::refresh_members() {
    if (!backend_.load_members) return;
    auto loaded = backend_.load_members();
    std::vector<std::pair<std::string, std::string>> closed;
    {
        std::lock_guard lock(state_mutex_);
        if (!running_) return;
        if (!loaded) {
            // Keep the names, but refuse to forward until the list can be read again.
            const auto self = backend_.self_node_id ? backend_.self_node_id() : std::string();
            for (auto& member : members_) {
                if (member.node_id != self) member.is_online = false;
            }
            error_ = "Cannot read the group devices";
            return;
        }
        error_.clear();
        for (auto it = egress_ips_.begin(); it != egress_ips_.end();) {
            const auto* member = find_member(*loaded, it->first);
            if (!member || strip_prefix_length(member->tunnel_ip) != it->second) {
                closed.emplace_back(it->first, it->second);
                it = egress_ips_.erase(it);
            } else {
                ++it;
            }
        }
        members_ = std::move(*loaded);
    }
    if (backend_.close_egress) {
        for (const auto& [node, address] : closed) backend_.close_egress(address, kMeshPortPeerServicePort);
    }
}

// Runs once per start or directory change. The SDK certificate renewer owns the
// schedule after that: a failed first attempt is retried in the background, and
// each renewal reaches certificate_renewed().
void MeshPort::check_certificate() {
    std::string dir;
    {
        std::lock_guard lock(state_mutex_);
        if (!running_ || !certificate_pending_) return;
        certificate_pending_ = false;
        dir = cert_dir_;
    }
    if (dir.empty() || !backend_.ensure_certificate) return;
    auto certificate = backend_.ensure_certificate(dir);
    if (!certificate) {
        std::lock_guard lock(state_mutex_);
        tls_error_ = "The group loopback certificate is not available yet";
        return;
    }
    install_certificate(*certificate);
    sodium_memzero(certificate->privkey_pem.data(), certificate->privkey_pem.size());
}

void MeshPort::set_certificate_dir(const std::string& cert_dir) {
    {
        std::lock_guard lock(state_mutex_);
        if (cert_dir == cert_dir_) return;
        cert_dir_ = cert_dir;
        certificate_pending_ = !cert_dir.empty();
    }
    {
        std::lock_guard lock(wake_mutex_);
        refresh_requested_ = true;
    }
    wake_.notify_all();
}

void MeshPort::certificate_renewed(const std::string& target, const std::string& domain,
                                   const std::string& fullchain_path, const std::string& privkey_path) {
    if (target != kGroupLoopbackCertTarget) return;
    {
        std::lock_guard lock(state_mutex_);
        if (!running_ || cert_dir_.empty()) return;
        std::error_code ignored;
        const auto dir = std::filesystem::weakly_canonical(cert_dir_, ignored);
        if (std::filesystem::weakly_canonical(std::filesystem::path(fullchain_path).parent_path(), ignored) != dir) return;
    }
    auto certificate = read_certificate_files(domain, fullchain_path, privkey_path);
    if (!certificate) {
        std::lock_guard lock(state_mutex_);
        tls_error_ = "The renewed certificate files cannot be read";
        return;
    }
    install_certificate(*certificate);
    sodium_memzero(certificate->privkey_pem.data(), certificate->privkey_pem.size());
}

bool MeshPort::install_certificate(const MeshPortCertificate& certificate) {
    const auto domain = mesh_port::local_domain_from_certificate(certificate.domain);
    std::string error;
    std::shared_ptr<const Tls::Material> material;
    if (domain.empty()) {
        error = "The certificate " + certificate.domain + " is not a group wildcard (*.<group>.lo.<base>)";
    } else {
        material = Tls::parse(certificate, domain, error);
    }
    std::lock_guard lifecycle(lifecycle_mutex_);
    {
        std::lock_guard lock(state_mutex_);
        if (!running_) return false;
        if (!material) {
            tls_error_ = error;
            if (tls_state_ != "active") tls_state_ = "unsupported_certificate";
            return false;
        }
    }
    tls_->set(material);
    if (!listener_ || !listener_->tls) {
        uint16_t port = 0;
        {
            std::lock_guard lock(state_mutex_);
            port = port_;
        }
        stop_listener();
        const auto bound = bind_listener(port, true);
        if (bound != MeshPortStartResult::Ok) {
            tls_->set(nullptr);
            std::lock_guard lock(state_mutex_);
            running_ = false;
            tls_state_ = "error";
            tls_error_ = "Cannot bind the TLS listener";
            error_ = bound == MeshPortStartResult::PortInUse
                ? "Another process took the port during the switch to TLS" : "Cannot bind the TLS listener";
            {
                std::lock_guard wake(wake_mutex_);
                stopping_ = true;
            }
            wake_.notify_all();
            return false;
        }
    }
    std::lock_guard lock(state_mutex_);
    local_domain_ = domain;
    tls_state_ = "active";
    tls_domain_ = lower(certificate.domain);
    tls_error_.clear();
    // Report the certificate's own expiry; a declared expiry can only shorten it.
    auto expires = static_cast<uint64_t>(std::max<int64_t>(material->not_after, 1));
    if (certificate.expires_at != 0) expires = std::min(expires, certificate.expires_at);
    certificate_expires_at_ = expires;
    return true;
}

int MeshPort::set_selection(const std::string& target, const std::string& peer_key) {
    auto selected = target;
    if (!selected.empty() && selected != "local" && selected != "auto") {
        bool known = false;
        {
            std::lock_guard lock(state_mutex_);
            known = find_member(members_, selected) != nullptr;
        }
        if (!known) refresh_now();
    }
    std::lock_guard lock(state_mutex_);
    if (!running_) return 409;
    const auto self = backend_.self_node_id ? backend_.self_node_id() : std::string();
    if (!selected.empty() && selected == self) selected = "local";
    if (selected == "local" && local_host_.empty()) return 409;
    if (!selected.empty() && selected != "local" && selected != "auto") {
        const auto* member = find_member(members_, selected);
        if (!member) return 404;
        if (!member->is_inference) return 409;
        if (!member->can_connect) return 403;
    }
    selection_target_ = selected;
    selection_key_ = selected.empty() ? std::string() : peer_key;
    return 0;
}

nlohmann::json MeshPort::status() const {
    std::lock_guard lock(state_mutex_);
    const auto self = backend_.self_node_id ? backend_.self_node_id() : std::string();
    const bool tls = tls_state_ == "active";
    json devices = json::array();
    for (const auto& member : members_) {
        if (member.node_id == self || !member.is_inference || !member.can_connect) continue;
        devices.push_back({{"node_id", member.node_id}, {"device_name", member.device_name}, {"is_online", member.is_online}});
    }
    return {
        {"running", running_},
        {"port", running_ ? port_ : 0},
        {"scheme", tls ? "https" : "http"},
        {"local_domain", local_domain_},
        {"local_service", !local_host_.empty()},
        {"error", error_},
        {"selection", {{"target", selection_target_.empty() ? json(nullptr) : json(selection_target_)},
                       {"peer_key_set", !selection_key_.empty()}}},
        {"tls", {{"state", running_ ? tls_state_ : std::string("off")}, {"domain", tls_domain_},
                 {"expires_at", certificate_expires_at_},
                 {"error", tls_error_.empty() && cert_dir_.empty() && running_
                               ? std::string("No certificate directory is set") : tls_error_}}},
        {"urls", running_ ? mesh_port::device_urls(members_, self, local_domain_, port_, tls) : json::array()},
        {"devices", devices},
    };
}

void MeshPort::handle(const httplib::Request& req, httplib::Response& res, bool tls) {
    try {
        if (req.target.empty() || req.target.front() != '/') {
            reply_error(res, 400, "Send an origin-form request target");
            return;
        }
        const auto host = req.get_header_value("Host");
        // Browsers send Origin on cross-site requests. Only a page served by this
        // same name may call the port, so another site cannot use this device's
        // mesh access.
        if (req.has_header("Origin")) {
            const auto expected = std::string(tls ? "https://" : "http://") + lower(host);
            if (lower(req.get_header_value("Origin")) != expected) {
                reply_error(res, 403, "Cross-origin requests are not accepted on this port");
                return;
            }
        }
        std::string sni;
        if (tls && req.ssl) {
            // req.ssl is `const SSL*` in cpp-httplib before 0.28 and an opaque
            // `tls::const_session_t` (const void*) from 0.28 on; both cast.
            const auto* session = static_cast<const SSL*>(req.ssl);
            if (const char* name = SSL_get_servername(session, TLSEXT_NAMETYPE_host_name)) {
                sni = name;
            }
        }
        std::vector<MeshPortMember> members;
        std::string domain;
        std::string target;
        std::string key;
        {
            std::lock_guard lock(state_mutex_);
            members = members_;
            domain = local_domain_;
            target = selection_target_;
            key = selection_key_;
        }
        const auto self = backend_.self_node_id ? backend_.self_node_id() : std::string();
        const auto named = mesh_port::host_target(host, sni, domain, members, self);
        if (named.first != 0) {
            reply_error(res, named.first, named.second);
            return;
        }
        if (named.second != "local") {
            const auto supplied = req.get_header_value(kPeerAuthorizationHeader);
            const auto stored = named.second == target && !key.empty() ? "Bearer " + key : std::string();
            forward_to_node(named.second, req.target, req, res, supplied.empty() ? stored : supplied, req.body);
            return;
        }
        serve_selection(req, res, req.target);
    } catch (const std::exception& e) {
        spdlog::warn("[MeshPort] request failed: {}", e.what());
        reply_error(res, 500, "The mesh port could not serve this request");
    }
}

void MeshPort::serve_selection(const httplib::Request& req, httplib::Response& res, const std::string& path) {
    std::string target;
    std::string key;
    {
        std::lock_guard lock(state_mutex_);
        target = selection_target_;
        key = selection_key_;
    }
    const auto peer_authorization = key.empty() ? std::string() : "Bearer " + key;
    if (target.empty()) {
        reply_error(res, 409, "No device is selected. Set a selection for this port first.");
        return;
    }
    if (target == "auto") {
        serve_auto(req, res, path, peer_authorization);
        return;
    }
    if (target == "local") {
        forward_local(path, req, res, req.body);
        return;
    }
    forward_to_node(target, path, req, res, peer_authorization, req.body);
}

void MeshPort::serve_auto(const httplib::Request& req, httplib::Response& res, const std::string& path,
                          const std::string& peer_authorization) {
    std::vector<MeshPortMember> members;
    {
        std::lock_guard lock(state_mutex_);
        members = members_;
    }
    const auto self = backend_.self_node_id ? backend_.self_node_id() : std::string();
    const auto caller_authorization = req.get_header_value("Authorization");
    if (req.method == "GET" && is_models_path(req.path)) {
        const auto merged = mesh_port::merge_models(local_models(caller_authorization),
                                                    peer_models(members, self, peer_authorization));
        res.set_header("Cache-Control", "no-store");
        res.set_content(merged.dump(), "application/json");
        return;
    }
    auto body = req.is_multipart_form_data() ? json() : json::parse(req.body, nullptr, false);
    const auto model = body.is_object() && body.contains("model") && body["model"].is_string()
        ? body["model"].get<std::string>() : std::string();
    const auto local_ids = local_model_ids(caller_authorization);
    auto route = mesh_port::auto_route(model, members, self, local_ids, {});
    if (route.target.empty() && route.status == 404) {
        route = mesh_port::auto_route(model, members, self, local_ids, peer_models(members, self, peer_authorization));
    }
    if (route.target.empty()) {
        reply_error(res, route.status, route.error);
        return;
    }
    auto forwarded = req.body;
    if (route.model != model) {
        body["model"] = route.model;
        forwarded = body.dump();
    }
    if (route.target != "local") {
        forward_to_node(route.target, path, req, res, peer_authorization, forwarded);
        return;
    }
    forward_local(path, req, res, forwarded);
}

void MeshPort::forward_to_node(const std::string& node_id, const std::string& path, const httplib::Request& req,
                               httplib::Response& res, const std::string& peer_authorization, const std::string& body) {
    std::vector<MeshPortMember> members;
    {
        std::lock_guard lock(state_mutex_);
        members = members_;
    }
    const auto self = backend_.self_node_id ? backend_.self_node_id() : std::string();
    const auto target = mesh_port::forward_target(members, self, node_id);
    if (target.first != 0) {
        reply_error(res, target.first, target.second);
        return;
    }
    const uint16_t port = backend_.open_egress ? backend_.open_egress(target.second, kMeshPortPeerServicePort) : 0;
    if (port == 0) {
        reply_error(res, 502, "Cannot open a mesh connection to that device");
        return;
    }
    {
        std::lock_guard lock(state_mutex_);
        egress_ips_[node_id] = target.second;
    }
    if (req.is_multipart_form_data()) {
        const auto [type, encoded] = multipart_body(req);
        proxy(*proxies_, "127.0.0.1", port, req, path, forwarded_headers(req, peer_authorization, type), encoded, res);
        return;
    }
    const auto type = req.has_header("Content-Type") ? req.get_header_value("Content-Type") : std::string();
    proxy(*proxies_, "127.0.0.1", port, req, path, forwarded_headers(req, peer_authorization, type), body, res);
}

void MeshPort::forward_local(const std::string& path, const httplib::Request& req, httplib::Response& res,
                             const std::string& body) {
    std::string host;
    uint16_t port = 0;
    {
        std::lock_guard lock(state_mutex_);
        host = local_host_;
        port = local_port_;
    }
    if (host.empty()) {
        reply_error(res, 409, "This device has no local service for this port");
        return;
    }
    // The local service applies its own API key policy to the caller's credentials.
    const auto authorization = req.get_header_value("Authorization");
    if (req.is_multipart_form_data()) {
        const auto [type, encoded] = multipart_body(req);
        proxy(*proxies_, host, port, req, path, forwarded_headers(req, authorization, type), encoded, res);
        return;
    }
    const auto type = req.has_header("Content-Type") ? req.get_header_value("Content-Type") : std::string();
    proxy(*proxies_, host, port, req, path, forwarded_headers(req, authorization, type), body, res);
}

nlohmann::json MeshPort::local_models(const std::string& authorization) {
    std::string host;
    uint16_t port = 0;
    {
        std::lock_guard lock(state_mutex_);
        host = local_host_;
        port = local_port_;
    }
    if (!host.empty()) {
        if (auto listing = get_json(host, port, "/api/v1/models", authorization); listing && listing->is_object()) {
            return *listing;
        }
    }
    return json{{"object", "list"}, {"data", json::array()}};
}

std::vector<std::string> MeshPort::local_model_ids(const std::string& authorization) {
    {
        std::lock_guard lock(cache_mutex_);
        if (local_models_authorization_ == authorization &&
            std::chrono::steady_clock::now() - local_models_fetched_ < options_.models_ttl) {
            return local_models_;
        }
    }
    auto ids = model_ids(local_models(authorization));
    std::lock_guard lock(cache_mutex_);
    local_models_ = ids;
    local_models_authorization_ = authorization;
    local_models_fetched_ = std::chrono::steady_clock::now();
    return ids;
}

std::vector<MeshPortPeerModels> MeshPort::peer_models(const std::vector<MeshPortMember>& members,
                                                      const std::string& self,
                                                      const std::string& peer_authorization) {
    std::vector<MeshPortPeerModels> result;
    for (const auto& member : members) {
        if (!reachable_peer(member, self)) continue;
        const auto cache_key = std::make_pair(member.node_id, peer_authorization);
        {
            std::lock_guard lock(cache_mutex_);
            const auto cached = peer_cache_.find(cache_key);
            if (cached != peer_cache_.end() &&
                std::chrono::steady_clock::now() - cached->second.fetched < options_.models_ttl) {
                result.push_back(cached->second.models);
                continue;
            }
        }
        const auto address = strip_prefix_length(member.tunnel_ip);
        const uint16_t port = backend_.open_egress && !address.empty()
            ? backend_.open_egress(address, kMeshPortPeerServicePort) : 0;
        if (port == 0) continue;
        {
            std::lock_guard lock(state_mutex_);
            egress_ips_[member.node_id] = address;
        }
        MeshPortPeerModels models;
        models.node_id = member.node_id;
        models.device_name = member.device_name;
        models.latency_ms = member.latency_ms;
        const auto health = get_json("127.0.0.1", port, "/api/v1/health", peer_authorization);
        if (health && health->is_object() && health->contains("all_models_loaded")) {
            models.loaded = (*health)["all_models_loaded"];
        }
        if (const auto listing = get_json("127.0.0.1", port, "/api/v1/models", peer_authorization)) {
            models.available = model_ids(*listing);
        }
        std::lock_guard lock(cache_mutex_);
        peer_cache_[cache_key] = {std::chrono::steady_clock::now(), models};
        result.push_back(std::move(models));
    }
    return result;
}

}  // namespace lnsdk
