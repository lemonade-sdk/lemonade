#include <LemonadeNexusSDK/AccountAvatarClient.hpp>

#include <httplib.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>

#ifdef _WIN32
#include <windows.h>
#endif

namespace lnsdk {
namespace {
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr std::size_t kImageLimit = 128 * 1024;
constexpr std::string_view kReferenceType = "nexus.account_avatar.ref.v2";

std::string base64(std::span<const uint8_t> bytes) {
    std::string out(sodium_base64_encoded_len(bytes.size(), sodium_base64_VARIANT_ORIGINAL), '\0');
    sodium_bin2base64(out.data(), out.size(), bytes.data(), bytes.size(), sodium_base64_VARIANT_ORIGINAL);
    out.pop_back();
    return out;
}

std::vector<uint8_t> unbase64(const std::string& value) {
    std::vector<uint8_t> out(value.size());
    std::size_t size = 0;
    const char* end = nullptr;
    if (sodium_base642bin(out.data(), out.size(), value.data(), value.size(), nullptr,
                         &size, &end, sodium_base64_VARIANT_ORIGINAL) != 0 ||
        end != value.data() + value.size()) return {};
    out.resize(size);
    return out;
}

std::string digest(const std::string& value) {
    std::array<uint8_t, crypto_hash_sha256_BYTES> hash{};
    crypto_hash_sha256(hash.data(), reinterpret_cast<const uint8_t*>(value.data()), value.size());
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex{};
    sodium_bin2hex(hex.data(), hex.size(), hash.data(), hash.size());
    return hex.data();
}

bool hash_shape(const std::string& hash) {
    return hash.empty() || (hash.size() == 64 && hash.find_first_not_of("0123456789abcdef") == std::string::npos);
}

std::string key_id(std::string key) {
    if (key.starts_with("ed25519:")) key.erase(0, 8);
    const auto decoded = Identity::from_base64(key);
    return decoded.size() == kEd25519PublicKeySize ? base64(decoded) : "";
}

bool verify_signed(json value, const char* key_field) {
    const auto key = unbase64(value.value(key_field, ""));
    const auto signature = unbase64(value.value("signature", ""));
    if (key.size() != kEd25519PublicKeySize || signature.size() != kEd25519SignatureSize) return false;
    value.erase("signature");
    const auto canonical = value.dump();
    return crypto_sign_verify_detached(signature.data(), reinterpret_cast<const uint8_t*>(canonical.data()),
                                        canonical.size(), key.data()) == 0;
}

void sign(json& value, const Identity& identity) {
    const auto canonical = value.dump();
    value["signature"] = base64(identity.sign(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(canonical.data()), canonical.size())));
}

Result<json> failure(int status, const std::string& error) { return {false, {}, status, error}; }

json bounded_post(httplib::Client& client, const std::string& path, const json& body, std::size_t limit) {
    httplib::Request request;
    request.method = "POST";
    request.path = path;
    request.body = body.dump();
    request.headers.emplace("Content-Type", "application/json");
    std::string received;
    request.content_receiver = [&](const char* data, std::size_t size, uint64_t, uint64_t) {
        if (size > limit - received.size()) return false;
        received.append(data, size);
        return true;
    };
    const auto response = client.send(request);
    if (!response || response->status != 200) return {};
    return json::parse(received, nullptr, false);
}
} // namespace

bool valid_avatar_image(const std::string& image) {
    if (image.empty()) return true;
    constexpr std::string_view prefix = "data:image/png;base64,";
    if (image.size() > kImageLimit || !image.starts_with(prefix)) return false;
    const auto bytes = unbase64(image.substr(prefix.size()));
    constexpr std::array<uint8_t, 16> header{137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
    if (bytes.size() < 45 || !std::equal(header.begin(), header.end(), bytes.begin())) return false;
    const auto u32 = [&](std::size_t offset) {
        return (uint32_t(bytes[offset]) << 24) | (uint32_t(bytes[offset + 1]) << 16) |
               (uint32_t(bytes[offset + 2]) << 8) | uint32_t(bytes[offset + 3]);
    };
    return u32(16) > 0 && u32(16) <= 128 && u32(20) > 0 && u32(20) <= 128;
}

std::string avatar_image_hash(const std::string& image) { return image.empty() ? "" : digest(image); }

json sign_avatar_reference(const Identity& identity, const std::string& group,
                           const std::string& image, uint64_t revision) {
    if (!identity.is_valid() || group.empty() || group.size() > 128 || !valid_avatar_image(image) ||
        revision == 0 || revision > 9007199254740991ULL) throw std::invalid_argument("Invalid profile image reference");
    json record{{"type", kReferenceType}, {"group_id", group}, {"author_pubkey", base64(identity.public_key())},
                {"revision", revision}, {"image_hash", avatar_image_hash(image)}};
    sign(record, identity);
    return record;
}

bool verify_avatar_reference(const json& r) {
    try {
        if (!r.is_object() || r.size() != 6 || r.value("type", "") != kReferenceType ||
            r.value("group_id", "").empty() || r.at("group_id").get_ref<const std::string&>().size() > 128 ||
            !r.at("revision").is_number_integer() || r.at("revision") <= 0 || r.at("revision") > 9007199254740991ULL ||
            !hash_shape(r.at("image_hash").get<std::string>()) ||
            r.at("author_pubkey").get_ref<const std::string&>().size() > 64 ||
            r.at("signature").get_ref<const std::string&>().size() > 128) return false;
        return verify_signed(r, "author_pubkey");
    } catch (...) { return false; }
}

struct AccountAvatarClient::Impl {
    Identity identity;
    std::string own_key;
    std::filesystem::path root;
    Api api;
    Expose expose;
    Egress egress;
    Peers peers;
    httplib::Server server;
    int port{};
    bool exposed{false};
    std::thread thread;
    std::mutex operation_mutex, cache_mutex, challenge_mutex;
    std::map<std::string, Clock::time_point> challenges;
    std::string attempted_hash;
    std::size_t next_peer{0};
    Clock::time_point retry_after{};

    Impl(const Identity& id, std::filesystem::path path, Api a, Expose x, Egress e, Peers p)
        : identity(id), own_key(base64(id.public_key())), root(std::move(path)),
          api(std::move(a)), expose(std::move(x)), egress(std::move(e)), peers(std::move(p)) {
        if (!identity.is_valid()) throw std::invalid_argument("Device identity required");
        std::filesystem::create_directories(root);
#ifndef _WIN32
        std::filesystem::permissions(root, std::filesystem::perms::owner_all);
#endif
        server.set_payload_max_length(2048);
        server.set_read_timeout(3);
        server.set_write_timeout(3);
        server.new_task_queue = [] { return new httplib::ThreadPool(2, 64); };
        server.Post("/avatar/challenge", [this](const httplib::Request&, httplib::Response& response) {
            std::lock_guard lock(challenge_mutex);
            const auto now = Clock::now();
            std::erase_if(challenges, [&](const auto& entry) { return entry.second < now; });
            if (challenges.size() >= 64) { response.status = 429; return; }
            std::array<uint8_t, 32> random{};
            randombytes_buf(random.data(), random.size());
            const auto nonce = base64(random);
            challenges[nonce] = now + std::chrono::seconds(30);
            response.set_header("Cache-Control", "no-store");
            response.set_content(json{{"nonce", nonce}, {"recipient_pubkey", own_key}}.dump(), "application/json");
        });
        server.Post("/avatar/image", [this](const httplib::Request& request, httplib::Response& response) {
            response.set_header("Cache-Control", "no-store");
            response.status = 404;
            try {
                auto proof = json::parse(request.body, nullptr, false);
                if (!proof.is_object() || proof.size() != 7 ||
                    proof.value("type", "") != "nexus.account_avatar.fetch.v1" ||
                    proof.value("recipient_pubkey", "") != own_key) return;
                {
                    std::lock_guard lock(challenge_mutex);
                    const auto found = challenges.find(proof.value("nonce", ""));
                    if (found == challenges.end()) return;
                    const bool fresh = found->second >= Clock::now();
                    challenges.erase(found);
                    if (!fresh) return;
                }
                if (!verify_signed(proof, "requester_pubkey")) return;
                const auto current = manifest();
                if (!current || current.value["group_id"] != proof["group_id"] || current.value["record"].is_null() ||
                    current.value["record"]["image_hash"] != proof["image_hash"] || proof["image_hash"] == "") return;
                bool allowed = false;
                for (const auto& device : current.value["devices"]) {
                    if (key_id(device.value("pubkey", "")) == proof["requester_pubkey"].get<std::string>() &&
                        device.value("can_fetch_from_me", false)) allowed = true;
                }
                if (!allowed) return;
                const auto image = cached(proof["group_id"], proof["image_hash"]);
                if (image.empty()) return;
                response.status = 200;
                response.set_content(json{{"image", image}}.dump(), "application/json");
            } catch (...) { response.status = 404; }
        });
        port = server.bind_to_any_port("127.0.0.1");
        if (port <= 0) throw std::runtime_error("Cannot start device image service");
        thread = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
    }

    ~Impl() { server.stop(); if (thread.joinable()) thread.join(); }

    bool publish_service() {
        if (!exposed) exposed = expose(kAccountAvatarPort, "tcp:127.0.0.1:" + std::to_string(port));
        return exposed;
    }

    Result<json> manifest() {
        const auto reply = api("GET", "/api/account/avatar", "");
        if (!reply || reply.http_status != 200) return failure(reply.http_status, reply.error);
        const auto value = json::parse(reply.value, nullptr, false);
        if (!value.is_object() || value.value("protocol", "") != kReferenceType)
            return failure(501, "This mesh needs an update to share device-hosted profile images.");
        if (!value.contains("group_id") || !value["group_id"].is_string() || value["group_id"] == "" ||
            value["group_id"].get_ref<const std::string&>().size() > 128 ||
            !value.contains("record") || !value.contains("devices") || !value["devices"].is_array() ||
            !value.contains("can_write") || !value["can_write"].is_boolean()) return failure(502, "Invalid profile image reference");
        const auto& r = value["record"];
        if ((!r.is_null() && (!verify_avatar_reference(r) || r["group_id"] != value["group_id"] || r["revision"] != value["revision"])) ||
            (r.is_null() && value.value("revision", -1) != 0)) return failure(502, "Invalid profile image signature");
        return {true, value, 200, ""};
    }

    std::filesystem::path directory(const std::string& group) { return root / digest(group); }

    std::string cached(const std::string& group, const std::string& hash) {
        if (hash.empty() || !hash_shape(hash)) return {};
        std::lock_guard lock(cache_mutex);
        const auto path = directory(group) / hash;
        std::error_code ec;
        if (std::filesystem::file_size(path, ec) > kImageLimit || ec) return {};
        std::ifstream input(path, std::ios::binary);
        const std::string image((std::istreambuf_iterator<char>(input)), {});
        return valid_avatar_image(image) && avatar_image_hash(image) == hash ? image : "";
    }

    bool write_local(const std::filesystem::path& target, const std::string& data) {
        std::filesystem::create_directories(target.parent_path());
        auto temporary = target;
        temporary += ".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(data.data(), static_cast<std::streamsize>(data.size()));
        output.close();
        if (!output) return false;
        std::error_code ec;
#ifdef _WIN32
        return MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        std::filesystem::permissions(temporary, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, ec);
        if (ec) return false;
        std::filesystem::rename(temporary, target, ec);
        return !ec;
#endif
    }

    bool cache(const std::string& group, const std::string& image) {
        std::lock_guard lock(cache_mutex);
        return write_local(directory(group) / avatar_image_hash(image), image);
    }

    bool stage(const json& reference) {
        std::lock_guard lock(cache_mutex);
        return write_local(directory(reference["group_id"]) / "pending.ref", reference.dump());
    }

    void clear_pending(const std::string& group) {
        std::lock_guard lock(cache_mutex);
        std::error_code ec;
        std::filesystem::remove(directory(group) / "pending.ref", ec);
    }

    std::string pending_hash(const json& value) {
        std::lock_guard lock(cache_mutex);
        const auto path = directory(value["group_id"]) / "pending.ref";
        std::error_code ec;
        if (std::filesystem::file_size(path, ec) > 2048 || ec) return {};
        std::ifstream input(path);
        const auto pending = json::parse(input, nullptr, false);
        if (!verify_avatar_reference(pending) || pending["group_id"] != value["group_id"] ||
            pending["author_pubkey"] != own_key) return {};
        const auto order = [](const json& r) {
            return std::tuple(r["revision"].get<uint64_t>(), r["author_pubkey"].get<std::string>(), r["image_hash"].get<std::string>());
        };
        if (!value["record"].is_null() && order(value["record"]) >= order(pending)) {
            std::filesystem::remove(path, ec);
            return {};
        }
        return pending["image_hash"];
    }

    void prune(const std::string& group, const std::string& keep, const std::string& pending = "") {
        std::lock_guard lock(cache_mutex);
        std::error_code ec;
        const auto dir = directory(group);
        if (!std::filesystem::exists(dir, ec)) return;
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            const auto name = entry.path().filename().string();
            if (entry.is_regular_file() && name != "pending.ref" && name != keep && name != pending)
                std::filesystem::remove(entry.path(), ec);
        }
    }

    std::string download(const json& value, const std::string& hash) {
        const auto group = value["group_id"].get<std::string>();
        if (attempted_hash == group + ":" + hash && Clock::now() < retry_after) return {};
        if (attempted_hash != group + ":" + hash) next_peer = 0;
        attempted_hash = group + ":" + hash;
        retry_after = Clock::now() + std::chrono::seconds(10);
        unsigned attempts = 0;
        const auto available = peers();
        const auto& directory = value["devices"];
        const auto start_peer = next_peer;
        for (std::size_t i = 0; i < directory.size(); ++i) {
            const auto index = (start_peer + i) % directory.size();
            const auto& device = directory[index];
            const auto remote_key = key_id(device.value("pubkey", ""));
            if (remote_key.empty() || remote_key == own_key || !device.value("can_serve_me", false)) continue;
            const auto found = std::find_if(available.begin(), available.end(), [&](const MeshPeer& peer) {
                return peer.node_id == device.value("node_id", "");
            });
            if (found == available.end()) continue;
            auto ip = found->tunnel_ip.substr(0, found->tunnel_ip.find('/'));
            if (ip.empty() || ip.size() > 15 || ip.find_first_not_of("0123456789.") != std::string::npos) continue;
            if (++attempts > 2) break;
            // Continue from the next device after a bounded attempt, so offline
            // peers cannot permanently hide a later device with a cached copy.
            next_peer = (index + 1) % directory.size();
            const auto loopback = egress(ip, kAccountAvatarPort);
            if (!loopback) continue;
            httplib::Client peer("127.0.0.1", loopback);
            peer.set_connection_timeout(1);
            peer.set_read_timeout(3);
            peer.set_write_timeout(2);
            peer.set_follow_location(false);
            const auto challenge = bounded_post(peer, "/avatar/challenge", json::object(), 512);
            if (!challenge.is_object() || challenge.value("recipient_pubkey", "") != remote_key ||
                challenge.value("nonce", "").size() != 44) continue;
            json proof{{"type", "nexus.account_avatar.fetch.v1"}, {"group_id", group}, {"image_hash", hash},
                       {"requester_pubkey", own_key}, {"recipient_pubkey", remote_key}, {"nonce", challenge["nonce"]}};
            sign(proof, identity);
            const auto response = bounded_post(peer, "/avatar/image", proof, kImageLimit + 128);
            if (!response.is_object() || !response.contains("image") || !response["image"].is_string()) continue;
            auto image = response["image"].get<std::string>();
            if (!valid_avatar_image(image) || avatar_image_hash(image) != hash) continue;
            if (!cache(group, image)) continue;
            return image;
        }
        return {};
    }

    Result<json> hydrate(const json& value) {
        const std::string hash = value["record"].is_null() ? "" : value["record"].value("image_hash", "");
        const auto group = value["group_id"].get<std::string>();
        const auto staged = pending_hash(value);
        std::string image;
        if (!hash.empty()) {
            image = cached(group, hash);
            if (image.empty()) image = download(value, hash);
        }
        if (hash.empty() || !image.empty()) prune(group, hash, staged);
        return {true, {{"group_id", group}, {"revision", value["revision"]}, {"can_write", value["can_write"]},
                       {"image", image.empty() ? json() : json(image)}, {"pending", !hash.empty() && image.empty()}}, 200, ""};
    }
};

AccountAvatarClient::AccountAvatarClient(const Identity& identity, std::filesystem::path root,
    Api api, Expose expose, Egress egress, Peers peers)
    : impl_(std::make_unique<Impl>(identity, std::move(root), std::move(api), std::move(expose), std::move(egress), std::move(peers))) {}

AccountAvatarClient::~AccountAvatarClient() = default;

Result<json> AccountAvatarClient::read() {
    std::lock_guard lock(impl_->operation_mutex);
    try {
        const auto current = impl_->manifest();
        if (!current) return current;
        if (!impl_->publish_service()) return failure(503, "Device image sharing is unavailable");
        return impl_->hydrate(current.value);
    } catch (...) { return failure(502, "Profile image could not be read"); }
}

Result<json> AccountAvatarClient::write(const json& reference, const std::string& image) {
    std::lock_guard lock(impl_->operation_mutex);
    try {
        if (!verify_avatar_reference(reference) || reference["author_pubkey"] != impl_->own_key ||
            !valid_avatar_image(image) || reference["image_hash"] != avatar_image_hash(image)) return failure(400, "Invalid profile image");
        const auto current = impl_->manifest();
        if (!current) return current;
        if (current.value["group_id"] != reference["group_id"] || !current.value["can_write"].get<bool>()) return failure(403, "Profile image write access required");
        if (reference["revision"] <= current.value["revision"]) return failure(409, "Profile image changed; refresh and try again");
        if (!impl_->publish_service()) return failure(503, "Device image sharing is unavailable");
        const auto group = reference["group_id"].get<std::string>();
        const std::string old_hash = current.value["record"].is_null() ? "" : current.value["record"].value("image_hash", "");
        const auto staged = impl_->pending_hash(current.value);
        const auto hash = avatar_image_hash(image);
        const bool was_uncertain = !staged.empty() && staged != old_hash;
        if (was_uncertain && hash != staged)
            return failure(409, "The previous profile image change is awaiting mesh confirmation. Select the same image to retry, or refresh before choosing another.");
        impl_->prune(group, old_hash, hash);
        if (!image.empty() && !impl_->cache(group, image)) return failure(500, "Profile image could not be saved on this device");
        if (!impl_->stage(reference)) return failure(500, "Profile image reference could not be saved on this device");
        // Keep the staged copy if the response is lost: the reference may already be live.
        const auto result = impl_->api("POST", "/api/account/avatar", reference.dump());
        if (!result || result.http_status != 200) {
            if (!was_uncertain && result.http_status >= 400 && result.http_status < 500) {
                impl_->clear_pending(group);
                impl_->prune(group, old_hash);
            }
            return failure(result.http_status, result.error.empty() ? "Profile image reference was not confirmed" : result.error);
        }
        const auto updated = impl_->manifest();
        return updated ? impl_->hydrate(updated.value) : updated;
    } catch (...) { return failure(502, "Profile image could not be saved"); }
}

} // namespace lnsdk
