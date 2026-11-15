// Windows keystore backend: the KEK is protected with the Data Protection
// API (DPAPI) in user scope. The protected blob is kept in the identity
// file as `kek_ref.dpapi`; only a process running as the same Windows user
// can unprotect it. No prompt is ever shown (CRYPTPROTECT_UI_FORBIDDEN).

#ifdef _WIN32

#include <LemonadeNexusSDK/IdentityStore.hpp>

#include <LemonadeNexus/Crypto/SecureBuffer.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <wincrypt.h>

#include <sodium.h>

#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "crypt32.lib")
#endif

namespace lnsdk {

namespace {

// Fixed optional entropy: a blob protected for another purpose by the same
// user cannot be presented as a Nexus KEK.
constexpr char kEntropy[] = "ai.lemonadeserver.nexus.identity-kek.v2";

DATA_BLOB entropy_blob() {
    DATA_BLOB blob{};
    blob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy));
    blob.cbData = static_cast<DWORD>(sizeof(kEntropy) - 1);
    return blob;
}

KeystoreStatus from_last_error() {
    switch (GetLastError()) {
    case ERROR_INVALID_DATA:    // not this user's blob, or altered
    case ERROR_ACCESS_DENIED:
    case static_cast<DWORD>(NTE_BAD_KEY_STATE):
    case static_cast<DWORD>(NTE_BAD_KEY):
    case static_cast<DWORD>(NTE_BAD_DATA):
        return KeystoreStatus::Denied;
    default:
        return KeystoreStatus::Failed;
    }
}

class DpapiBackend final : public KeystoreBackend {
public:
    std::string name() const override { return "dpapi"; }
    bool available() const override { return true; }

    KeystoreStatus put(std::span<const uint8_t> kek, const std::string& /*pubkey_b64*/,
                       nlohmann::json& kek_ref) override {
        DATA_BLOB in{};
        in.pbData = const_cast<BYTE*>(kek.data());
        in.cbData = static_cast<DWORD>(kek.size());
        DATA_BLOB entropy = entropy_blob();
        DATA_BLOB out{};
        if (!CryptProtectData(&in, L"Lemonade Nexus device identity", &entropy, nullptr, nullptr,
                              CRYPTPROTECT_UI_FORBIDDEN, &out)) {
            return from_last_error();
        }
        const std::span<const uint8_t> blob(out.pbData, out.cbData);
        kek_ref = nlohmann::json{{"dpapi", Identity::to_base64(blob)}};
        LocalFree(out.pbData);
        return KeystoreStatus::Ok;
    }

    KeystoreStatus get(const nlohmann::json& kek_ref, nexus::crypto::SecureBuffer& kek) override {
        if (!kek_ref.is_object() || !kek_ref.contains("dpapi") || !kek_ref["dpapi"].is_string()) {
            return KeystoreStatus::NotFound;
        }
        std::vector<uint8_t> blob = Identity::from_base64(kek_ref["dpapi"].get<std::string>());
        if (blob.empty()) return KeystoreStatus::NotFound;

        DATA_BLOB in{};
        in.pbData = blob.data();
        in.cbData = static_cast<DWORD>(blob.size());
        DATA_BLOB entropy = entropy_blob();
        DATA_BLOB out{};
        if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                                &out)) {
            return from_last_error();
        }
        kek = nexus::crypto::SecureBuffer(std::span<const uint8_t>(out.pbData, out.cbData));
        sodium_memzero(out.pbData, out.cbData);
        LocalFree(out.pbData);
        return KeystoreStatus::Ok;
    }

    KeystoreStatus remove(const nlohmann::json& kek_ref) override {
        // The protected blob lives in the identity file; removing the file
        // removes it. Nothing is kept elsewhere.
        return kek_ref.is_object() ? KeystoreStatus::Ok : KeystoreStatus::NotFound;
    }
};

}  // namespace

std::shared_ptr<KeystoreBackend> detail::make_platform_backend() {
    return std::make_shared<DpapiBackend>();
}

}  // namespace lnsdk

#endif  // _WIN32
