#pragma once

#include <LemonadeNexusSDK/Error.hpp>
#include <LemonadeNexusSDK/Identity.hpp>
#include <LemonadeNexusSDK/Types.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lnsdk {

/// Mesh virtual port for device-hosted account images. No public listener uses it.
inline constexpr uint16_t kAccountAvatarPort = 9193;

/// Validate a PNG data URL, bounded to 128 KiB and dimensions 1..128.
/// The GUI performs the full image decode and re-encodes the selected photo.
[[nodiscard]] bool valid_avatar_image(const std::string& image);
/// Lowercase SHA-256 of the complete normalized PNG data URL. Empty means removal.
[[nodiscard]] std::string avatar_image_hash(const std::string& image);
/// Sign a reference without image bytes. Throws on invalid input or identity.
[[nodiscard]] nlohmann::json sign_avatar_reference(const Identity& identity,
    const std::string& group, const std::string& image, uint64_t revision);
/// Verify the bounded reference shape, domain and canonical device signature.
/// Current membership and permissions must also be verified by the private API.
[[nodiscard]] bool verify_avatar_reference(const nlohmann::json& reference);

/// Keeps account images on this device and retrieves them from authorized peers.
/// The API callbacks carry references and current permissions only. The mesh
/// callbacks expose a loopback-only service and open encrypted peer connections.
/// read/write serialize; request handlers independently recheck current access.
/// Destroy before the callbacks' client or dataplane. Destruction joins the server.
class AccountAvatarClient {
public:
    using Api = std::function<Result<std::string>(const std::string&, const std::string&, const std::string&)>;
    using Expose = std::function<bool(uint16_t, const std::string&)>;
    using Egress = std::function<uint16_t(const std::string&, uint16_t)>;
    using Peers = std::function<std::vector<MeshPeer>()>;

    AccountAvatarClient(const Identity& identity, std::filesystem::path cache_root,
                        Api api, Expose expose, Egress egress, Peers peers);
    ~AccountAvatarClient();
    AccountAvatarClient(const AccountAvatarClient&) = delete;
    AccountAvatarClient& operator=(const AccountAvatarClient&) = delete;

    /// Return the verified reference and matching cached or peer-fetched image.
    /// pending=true means a reference exists but no device has supplied its bytes.
    /// A network failure never turns a retained image into a removal.
    [[nodiscard]] Result<nlohmann::json> read();
    /// Cache image bytes locally before publishing only the signed reference.
    /// Retains an uncertain upload locally so a lost response cannot orphan it.
    [[nodiscard]] Result<nlohmann::json> write(const nlohmann::json& reference,
                                               const std::string& image);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lnsdk
