#include <LemonadeNexusSDK/MeshPort.hpp>
#include <LemonadeNexusSDK/CertRenewal.hpp>
#include <LemonadeNexusSDK/LemonadeNexusClient.hpp>
#include <LemonadeNexusSDK/ServiceIngress.hpp>
#include "MeshProxy.hpp"

#include <LemonadeNexus/Tree/DeviceLabel.hpp>

#include <httplib.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <sodium.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

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

bool is_models_path(const std::string& path) {
    return path == "/api/v1/models" || path == "/api/v0/models" || path == "/v1/models" || path == "/v0/models";
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

/// One GET of a JSON document. `status` is 0 when the service did not answer;
/// `body` is set only for a 200 with a JSON body.
struct JsonReply {
    int status{0};
    std::optional<json> body;
};

JsonReply get_json(httplib::Client& client, const std::string& path, const std::string& authorization) {
    httplib::Headers headers;
    if (!authorization.empty()) headers.emplace("Authorization", authorization);
    auto reply = client.Get(path, headers);
    if (!reply) return {};
    JsonReply result{reply->status, std::nullopt};
    if (reply->status != 200) return result;
    auto parsed = json::parse(reply->body, nullptr, false);
    if (!parsed.is_discarded()) result.body = std::move(parsed);
    return result;
}

void set_timeouts(httplib::Client& client, std::chrono::milliseconds connect, std::chrono::milliseconds read) {
    const auto split = [](std::chrono::milliseconds value) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(value);
        return std::make_pair(static_cast<time_t>(seconds.count()),
                              static_cast<time_t>(std::chrono::duration_cast<std::chrono::microseconds>(value - seconds).count()));
    };
    const auto [connect_sec, connect_usec] = split(connect);
    const auto [read_sec, read_usec] = split(read);
    client.set_connection_timeout(connect_sec, connect_usec);
    client.set_read_timeout(read_sec, read_usec);
    client.set_write_timeout(read_sec, read_usec);
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

/// One GET of a device's /.nexus/catalog. The probe needs no credentials: the
/// device's SDK answers it for any attributed peer.
MeshPortPeerCatalog fetch_catalog(httplib::Client& client) {
    MeshPortPeerCatalog catalog;
    const auto reply = get_json(client, std::string(service_ingress::kCatalogPath), "");
    catalog.status = reply.status;
    if (!reply.body || !reply.body->is_object()) return catalog;
    const auto& body = *reply.body;
    if (body.contains("version") && body["version"].is_number_integer() && body["version"].get<int64_t>() >= 0) {
        catalog.version = body["version"].get<uint64_t>();
    }
    if (body.contains("app") && body["app"].is_object()) {
        catalog.app_name = body["app"].value("name", "");
        catalog.app_version = body["app"].value("version", "");
    }
    if (body.contains("families") && body["families"].is_array()) {
        for (const auto& family : body["families"]) {
            if (family.is_object() && family.contains("name") && family["name"].is_string()) {
                catalog.families.push_back(family["name"].get<std::string>());
            }
        }
    }
    return catalog;
}

/// Runs `work(i)` for every i in [0, count) on at most eight threads at once and
/// waits until all finish or `deadline` passes. Then `cancel(i)` runs for each i
/// that has not finished, and every thread is joined: `work` must return soon
/// after `cancel`, and must skip its request when cancelled before it started.
void run_probes(size_t count, std::chrono::steady_clock::time_point deadline,
                const std::function<void(size_t)>& work, const std::function<void(size_t)>& cancel) {
    if (count == 0) return;
    constexpr size_t kProbeThreads = 8;
    std::mutex done_mutex;
    std::condition_variable done;
    std::vector<bool> finished(count, false);
    size_t finished_count = 0;
    std::atomic<size_t> next{0};
    std::vector<std::thread> workers;
    for (size_t i = 0; i < std::min(kProbeThreads, count); ++i) {
        workers.emplace_back([&] {
            for (size_t index = next++; index < count; index = next++) {
                work(index);
                std::lock_guard lock(done_mutex);
                finished[index] = true;
                ++finished_count;
                done.notify_all();
            }
        });
    }
    {
        std::unique_lock lock(done_mutex);
        done.wait_until(lock, deadline, [&] { return finished_count == count; });
    }
    std::vector<size_t> unfinished;
    {
        std::lock_guard lock(done_mutex);
        for (size_t i = 0; i < count; ++i) {
            if (!finished[i]) unfinished.push_back(i);
        }
    }
    for (const auto index : unfinished) cancel(index);
    for (auto& worker : workers) worker.join();
}

json catalog_json(const MeshPortPeerCatalog& catalog) {
    return json{{"name", catalog.app_name}, {"version", catalog.app_version}};
}

}  // namespace

// ---------------------------------------------------------------------------
// Routing rules
// ---------------------------------------------------------------------------

namespace mesh_port {

std::string device_label(std::string_view device_name) {
    // The server validates device names with the same function, so a name the
    // tree accepted routes here and two names that route the same are refused
    // there.
    return nexus::tree::device_label(device_name);
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
    const auto target = [&](const MeshPortMember& member) {
        return member.node_id == self_node_id ? std::string("local") : member.node_id;
    };
    // A node id is unique, so it always names exactly one device.
    for (const auto& member : members) {
        if (label == lower(member.node_id)) return {0, target(member)};
    }
    // Device names fold to DNS labels ("Work PC" and "work-pc" collide), so a
    // label that fits several devices routes to none of them: the caller must
    // use a node id. Picking the first match would send traffic to a device the
    // caller did not name.
    const MeshPortMember* found = nullptr;
    std::string others;
    for (const auto& member : members) {
        if (label != device_label(member.device_name)) continue;
        if (!found) { found = &member; continue; }
        others += (others.empty() ? "" : ", ") + member.device_name + " (" + member.node_id + ")";
    }
    if (!found) return {404, "No device in your group is named " + name};
    if (!others.empty()) {
        return {409, "The name " + name + " fits several devices: " + found->device_name + " (" + found->node_id +
                         "), " + others + ". Use the device id as the host name."};
    }
    return {0, target(*found)};
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
    // An exact local ID wins before "<device>/<model>" is read, so a local model whose
    // ID contains '/' is not sent to a device of that name.
    if (std::find(local_models.begin(), local_models.end(), model) != local_models.end()) return {"local", model};
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
    const MeshPortPeerModels* best = nullptr;
    int best_rank = 3;  // 0 loaded and idle, 1 loaded and busy, 2 listed only
    std::vector<std::string> refused;
    for (const auto& peer : peers) {
        const bool reachable = std::any_of(candidates.begin(), candidates.end(),
            [&](const MeshPortMember* member) { return member->node_id == peer.node_id; });
        if (!reachable) continue;
        if (peer.models_status == 401 || peer.models_status == 403) {
            // The stored peer key does not authenticate against this device, so
            // nothing it serves can be reached with it.
            refused.push_back(peer.device_name.empty() ? peer.node_id : peer.device_name);
            continue;
        }
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
    MeshPortAutoRoute route;
    route.model = model;
    route.refused = std::move(refused);
    if (best) {
        route.target = best->node_id;
        return route;
    }
    route.status = 404;
    route.error = "No reachable device serves the model " + model;
    if (!route.refused.empty()) {
        route.error += ". The peer key was refused by";
        for (size_t i = 0; i < route.refused.size(); ++i) route.error += (i == 0 ? " " : ", ") + route.refused[i];
    }
    return route;
}

std::set<std::string> loaded_model_names(const MeshPortPeerModels& peer) {
    std::set<std::string> names;
    if (!peer.loaded.is_array()) return names;
    for (const auto& entry : peer.loaded) {
        if (!entry.is_object()) continue;
        const auto name = entry.find("model_name");
        if (name != entry.end() && name->is_string() && !name->get_ref<const std::string&>().empty()) {
            names.insert(name->get<std::string>());
        }
    }
    return names;
}

json merge_models(const json& local, const std::vector<MeshPortPeerModels>& peers) {
    json result = local.is_object() ? local : json{{"object", "list"}, {"data", json::array()}};
    if (!result.contains("data") || !result["data"].is_array()) result["data"] = json::array();
    for (const auto& peer : peers) {
        const auto owner = peer.device_name.empty() ? peer.node_id : peer.device_name;
        const auto loaded = loaded_model_names(peer);
        for (const auto& id : peer.available) {
            result["data"].push_back({{"id", owner + "/" + id}, {"object", "model"},
                {"owned_by", owner}, {"nexus_node_id", peer.node_id}, {"loaded", loaded.contains(id)}});
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
    // Count every member's label, not only the listed ones: host_target() matches
    // against the whole group, so a name that collides anywhere is ambiguous.
    std::map<std::string, int> label_count;
    for (const auto& member : members) ++label_count[device_label(member.device_name)];
    for (const auto& member : members) {
        const bool self = member.node_id == self_node_id;
        if (!self && !reachable_peer(member, self_node_id)) continue;
        auto label = device_label(member.device_name);
        // An ambiguous or empty label gets the node id, which always routes.
        if (label.empty() || label_count[label] > 1) label = lower(member.node_id);
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
        // Every network of this identity: its own group, the devices that
        // joined it from their own networks, and the groups it joined.
        const auto devices = client.list_network_devices();
        if (!devices.ok) return std::nullopt;
        const auto peers = client.get_mesh_peers();
        std::vector<MeshPortMember> members;
        for (const auto& device : devices.value) {
            if (device.node_id.empty()) continue;
            MeshPortMember member;
            member.node_id = device.node_id;
            member.device_name = device.device_name;
            member.tunnel_ip = device.tunnel_ip;
            member.is_inference = device.is_inference;
            const bool is_self = device.node_id == self;
            member.can_connect = is_self || device.can_connect;
            member.is_online = is_self;
            for (const auto& peer : peers) {
                if (peer.node_id != device.node_id) continue;
                member.is_online = is_self || peer.is_online;
                member.latency_ms = peer.latency_ms > 0 ? static_cast<double>(peer.latency_ms) : 0.0;
                member.relay_endpoint = peer.relay_endpoint;
                member.path = peer.path;
                break;
            }
            members.push_back(std::move(member));
        }
        return members;
    };
    backend.local_catalog = [&client] { return client.service_catalog(kMeshPortPeerServicePort); };
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
      proxies_(std::make_shared<MeshPortProxies>()) {
    // The last open plain request hands the pending switch to TLS to the refresh
    // thread; a request handler must not replace the listener that runs it.
    proxies_->on_idle = [this] {
        {
            std::lock_guard lock(wake_mutex_);
            refresh_requested_ = true;
        }
        wake_.notify_all();
    };
}

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
    server.set_payload_max_length(kProxyPayloadLimit);
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
        tls_pending_ = false;
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
        proxies_->forget_switch();
        tls_->set(nullptr);
        std::lock_guard lock(state_mutex_);
        running_ = false;
        port_ = 0;
        tls_pending_ = false;
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
        catalog_cache_.clear();
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
        activate_pending_tls();
        refresh_members();
        refresh_catalogs();
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
        // A bridge carries virtual TCP connections over the mesh path that was
        // current when they opened. When the device's address or its relay
        // changes, close the bridge so the next request opens a fresh one;
        // connections left on the old relay allocation answer nothing.
        for (auto it = egress_ips_.begin(); it != egress_ips_.end();) {
            const auto* member = find_member(*loaded, it->first);
            const auto* before = find_member(members_, it->first);
            if (!member || strip_prefix_length(member->tunnel_ip) != it->second ||
                (before && before->relay_endpoint != member->relay_endpoint)) {
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
            if (tls_state_ != "active" && tls_state_ != "activating") tls_state_ = "unsupported_certificate";
            return false;
        }
    }
    // New handshakes read the material through the certificate callback, so a TLS
    // listener needs no restart and keeps its connections.
    tls_->set(material);
    const bool tls_listener = listener_ && listener_->tls;
    {
        std::lock_guard lock(state_mutex_);
        tls_state_ = tls_listener ? "active" : "activating";
        tls_pending_ = !tls_listener;
        tls_domain_ = lower(certificate.domain);
        tls_error_.clear();
        // Report the certificate's own expiry; a declared expiry can only shorten it.
        auto expires = static_cast<uint64_t>(std::max<int64_t>(material->not_after, 1));
        if (certificate.expires_at != 0) expires = std::min(expires, certificate.expires_at);
        certificate_expires_at_ = expires;
        if (tls_listener) {
            local_domain_ = domain;
            return true;
        }
    }
    return activate_tls_locked();
}

void MeshPort::activate_pending_tls() {
    std::lock_guard lifecycle(lifecycle_mutex_);
    activate_tls_locked();
}

// Replaces the plain listener with a TLS listener on the same port. Runs with
// lifecycle_mutex_ held. Stopping the plain server would cut every response it is
// still streaming, so the switch starts only when no request is open; otherwise it
// is retried from refresh_now() once the last open request finishes. Returns false
// only when the TLS listener cannot be bound.
bool MeshPort::activate_tls_locked() {
    uint16_t port = 0;
    std::string domain;
    {
        std::lock_guard lock(state_mutex_);
        if (!running_ || !tls_pending_) return true;
        port = port_;
        domain = mesh_port::local_domain_from_certificate(tls_domain_);
    }
    if (!listener_ || listener_->tls) return true;
    if (!proxies_->begin_switch()) return true;
    stop_listener();
    const auto bound = bind_listener(port, true);
    proxies_->end_switch();
    if (bound != MeshPortStartResult::Ok) {
        tls_->set(nullptr);
        std::lock_guard lock(state_mutex_);
        running_ = false;
        tls_pending_ = false;
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
    std::lock_guard lock(state_mutex_);
    tls_pending_ = false;
    tls_state_ = "active";
    local_domain_ = domain;
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
    const auto self = backend_.self_node_id ? backend_.self_node_id() : std::string();
    const auto local_catalog = backend_.local_catalog ? backend_.local_catalog() : json::object();
    std::map<std::string, CachedCatalog> catalogs;
    // The newest model probe of each device, whatever peer key it used.
    std::map<std::string, CachedModels> models;
    {
        std::lock_guard lock(cache_mutex_);
        catalogs = catalog_cache_;
        for (const auto& [key, cached] : peer_cache_) {
            auto& newest = models[key.first];
            if (cached.fetched >= newest.fetched) newest = cached;
        }
    }
    std::lock_guard lock(state_mutex_);
    const bool tls = tls_state_ == "active";
    json devices = json::array();
    for (const auto& member : members_) {
        if (member.node_id == self || !member.is_inference || !member.can_connect) continue;
        MeshPortPeerCatalog catalog;
        if (const auto cached = catalogs.find(member.node_id); cached != catalogs.end()) catalog = cached->second.catalog;
        size_t models_count = 0;
        size_t loaded_count = 0;
        if (const auto cached = models.find(member.node_id); cached != models.end()) {
            models_count = cached->second.models.available.size();
            loaded_count = mesh_port::loaded_model_names(cached->second.models).size();
        }
        devices.push_back({{"node_id", member.node_id}, {"device_name", member.device_name}, {"is_online", member.is_online},
            {"path", member.path},
            {"app", catalog_json(catalog)}, {"families", catalog.families}, {"catalog_status", catalog.status},
            {"models_count", models_count}, {"loaded_count", loaded_count}});
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
        {"catalog", local_catalog.is_object() ? local_catalog : json::object()},
    };
}

void MeshPort::handle(const httplib::Request& req, httplib::Response& res, bool tls) {
    if (!proxies_->enter()) {
        reply_error(res, 503, "The mesh port is switching to TLS. Retry the request.");
        res.set_header("Retry-After", "1");
        return;
    }
    struct Leave {
        MeshPortProxies& proxies;
        ~Leave() { proxies.leave(); }
    } leave{*proxies_};
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
    if (!route.refused.empty()) {
        std::string names;
        for (const auto& name : route.refused) names += (names.empty() ? "" : ", ") + name;
        spdlog::info("[MeshPort] auto route for {} skipped devices that refused the peer key: {}", model, names);
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
    // A bridge whose virtual connections fail at once (no response byte) is
    // replaced once before the request is answered: its mesh path changed
    // under it, for example to a new relay allocation.
    const auto address = target.second;
    const auto reopen = [this, &address, &node_id]() -> uint16_t {
        if (!backend_.close_egress || !backend_.open_egress) return 0;
        spdlog::info("[MeshPort] the bridge to {} answered nothing; opening a new one", node_id);
        backend_.close_egress(address, kMeshPortPeerServicePort);
        return backend_.open_egress(address, kMeshPortPeerServicePort);
    };
    if (req.is_multipart_form_data()) {
        const auto [type, encoded] = multipart_body(req);
        proxy_request(proxies_, "127.0.0.1", port, req, path, forwarded_headers(req, peer_authorization, type), encoded,
                      res, reopen);
        return;
    }
    const auto type = req.has_header("Content-Type") ? req.get_header_value("Content-Type") : std::string();
    proxy_request(proxies_, "127.0.0.1", port, req, path, forwarded_headers(req, peer_authorization, type), body, res,
                  reopen);
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
        proxy_request(proxies_, host, port, req, path, forwarded_headers(req, authorization, type), encoded, res);
        return;
    }
    const auto type = req.has_header("Content-Type") ? req.get_header_value("Content-Type") : std::string();
    proxy_request(proxies_, host, port, req, path, forwarded_headers(req, authorization, type), body, res);
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
        httplib::Client client(host, port);
        set_timeouts(client, std::chrono::seconds(5), std::chrono::seconds(5));
        if (auto listing = get_json(client, "/api/v1/models", authorization); listing.body && listing.body->is_object()) {
            return std::move(*listing.body);
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

std::optional<MeshPortPeerCatalog> MeshPort::cached_catalog(const std::string& node_id) {
    std::lock_guard lock(cache_mutex_);
    const auto cached = catalog_cache_.find(node_id);
    if (cached == catalog_cache_.end() ||
        std::chrono::steady_clock::now() - cached->second.fetched >= options_.catalog_ttl) {
        return std::nullopt;
    }
    return cached->second.catalog;
}

void MeshPort::store_catalog(const std::string& node_id, const MeshPortPeerCatalog& catalog) {
    std::lock_guard lock(cache_mutex_);
    catalog_cache_[node_id] = {std::chrono::steady_clock::now(), catalog};
}

namespace {

/// One peer probe: an egress and an HTTP client, cancelled by the coordinator
/// at the deadline through `cancelled` and the client.
struct MeshPortProbe {
    const MeshPortMember* member{nullptr};
    std::mutex mutex;
    std::shared_ptr<httplib::Client> client;
    bool cancelled{false};
    bool done{false};

    /// Opens the egress and the client; null when cancelled or unreachable.
    std::shared_ptr<httplib::Client> open(const MeshPortBackend& backend, const MeshPort::Options& options,
                                          std::mutex& state_mutex, std::map<std::string, std::string>& egress_ips) {
        const auto address = strip_prefix_length(member->tunnel_ip);
        const uint16_t egress = backend.open_egress && !address.empty()
            ? backend.open_egress(address, kMeshPortPeerServicePort) : 0;
        if (egress == 0) return nullptr;
        {
            std::lock_guard lock(state_mutex);
            egress_ips[member->node_id] = address;
        }
        std::lock_guard lock(mutex);
        if (cancelled) return nullptr;
        client = std::make_shared<httplib::Client>("127.0.0.1", egress);
        set_timeouts(*client, options.peer_probe_connect_timeout, options.peer_probe_deadline);
        return client;
    }
    bool is_cancelled() {
        std::lock_guard lock(mutex);
        return cancelled;
    }
    void finish() {
        std::lock_guard lock(mutex);
        done = true;
        client.reset();
    }
    /// Shuts the socket down under the probe thread, so its read fails now.
    void cancel() {
        std::shared_ptr<httplib::Client> open_client;
        {
            std::lock_guard lock(mutex);
            if (done) return;
            cancelled = true;
            open_client = client;
        }
        if (open_client) open_client->stop();
    }
};

}  // namespace

// Runs on the refresh thread: reads the catalog of every reachable peer whose
// cached answer is stale, so the status shows each device's app without a
// request having probed it.
void MeshPort::refresh_catalogs() {
    std::vector<MeshPortMember> members;
    {
        std::lock_guard lock(state_mutex_);
        if (!running_) return;
        members = members_;
    }
    const auto self = backend_.self_node_id ? backend_.self_node_id() : std::string();
    std::vector<std::unique_ptr<MeshPortProbe>> probes;
    for (const auto& member : members) {
        if (!reachable_peer(member, self) || cached_catalog(member.node_id)) continue;
        auto probe = std::make_unique<MeshPortProbe>();
        probe->member = &member;
        probes.push_back(std::move(probe));
    }
    run_probes(probes.size(), std::chrono::steady_clock::now() + options_.peer_probe_deadline,
        [&](size_t index) {
            auto& probe = *probes[index];
            MeshPortPeerCatalog catalog;
            if (const auto client = probe.open(backend_, options_, state_mutex_, egress_ips_)) {
                catalog = fetch_catalog(*client);
            }
            probe.finish();
            // A peer that did not answer is cached as unknown until the next interval.
            store_catalog(probe.member->node_id, catalog);
        },
        [&](size_t index) { probes[index]->cancel(); });
}

std::vector<MeshPortPeerModels> MeshPort::peer_models(const std::vector<MeshPortMember>& members,
                                                      const std::string& self,
                                                      const std::string& peer_authorization) {
    struct Probe {
        MeshPortProbe probe;
        std::optional<MeshPortPeerModels> models;  // set for a cached or finished probe
    };
    std::vector<std::unique_ptr<Probe>> probes;
    std::vector<Probe*> pending;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& member : members) {
        if (!reachable_peer(member, self)) continue;
        auto probe = std::make_unique<Probe>();
        probe->probe.member = &member;
        {
            std::lock_guard lock(cache_mutex_);
            const auto cached = peer_cache_.find(std::make_pair(member.node_id, peer_authorization));
            if (cached != peer_cache_.end() && now - cached->second.fetched < options_.models_ttl) {
                probe->models = cached->second.models;
            }
        }
        if (!probe->models) pending.push_back(probe.get());
        probes.push_back(std::move(probe));
    }

    // A bounded set of threads probes the peers at the same time, so the listing
    // waits for the slowest peer or the deadline, not for the sum.
    run_probes(pending.size(), now + options_.peer_probe_deadline,
        [&](size_t index) {
            auto& entry = *pending[index];
            const auto& member = *entry.probe.member;
            MeshPortPeerModels models;
            models.node_id = member.node_id;
            models.device_name = member.device_name;
            models.latency_ms = member.latency_ms;
            if (const auto client = entry.probe.open(backend_, options_, state_mutex_, egress_ips_)) {
                const auto health = get_json(*client, "/api/v1/health", peer_authorization);
                if (health.body && health.body->is_object() && health.body->contains("all_models_loaded")) {
                    models.loaded = (*health.body)["all_models_loaded"];
                }
                if (!entry.probe.is_cancelled()) {
                    const auto listing = get_json(*client, "/api/v1/models", peer_authorization);
                    models.models_status = listing.status;
                    if (listing.body) models.available = model_ids(*listing.body);
                }
                if (const auto cached = cached_catalog(member.node_id)) {
                    models.catalog = *cached;
                } else if (!entry.probe.is_cancelled()) {
                    models.catalog = fetch_catalog(*client);
                    store_catalog(member.node_id, models.catalog);
                }
            }
            entry.probe.finish();
            std::lock_guard lock(entry.probe.mutex);
            entry.models = std::move(models);
        },
        [&](size_t index) { pending[index]->probe.cancel(); });

    std::vector<MeshPortPeerModels> result;
    const auto fetched = std::chrono::steady_clock::now();
    std::lock_guard lock(cache_mutex_);
    for (auto& probe : probes) {
        std::optional<MeshPortPeerModels> models;
        {
            std::lock_guard probe_lock(probe->probe.mutex);
            models = probe->models;
        }
        if (!models) continue;
        // A peer that did not answer by the deadline is cached as offering nothing,
        // so it is not probed again before the next interval.
        if (std::find(pending.begin(), pending.end(), probe.get()) != pending.end()) {
            peer_cache_[std::make_pair(probe->probe.member->node_id, peer_authorization)] = {fetched, *models};
        }
        result.push_back(std::move(*models));
    }
    return result;
}

}  // namespace lnsdk
