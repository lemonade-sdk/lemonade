// macOS keystore backend: the KEK is a generic password in the user's login
// keychain. The login keychain is unlocked at login, so a user-session
// process reads the item without a prompt. A root daemon has no user
// security session and cannot open it, which is intended: the identity
// belongs to the user, and lemond receives it through the hand-off.

#ifdef __APPLE__

#include <LemonadeNexusSDK/IdentityStore.hpp>

#include <LemonadeNexus/Crypto/SecureBuffer.hpp>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace lnsdk {

namespace {

constexpr std::string_view kAccountPrefix = "device-identity-kek:";
constexpr std::size_t kAccountPubkeyChars = 16;

/// Releases a Core Foundation object at scope exit.
template <typename T>
class CFHolder {
public:
    CFHolder() = default;
    explicit CFHolder(T ref) : ref_(ref) {}
    ~CFHolder() {
        if (ref_) CFRelease(ref_);
    }
    CFHolder(const CFHolder&) = delete;
    CFHolder& operator=(const CFHolder&) = delete;
    [[nodiscard]] T get() const noexcept { return ref_; }
    [[nodiscard]] T* out() noexcept { return &ref_; }

private:
    T ref_{};
};

KeystoreStatus from_os_status(OSStatus status) {
    switch (status) {
    case errSecSuccess:
        return KeystoreStatus::Ok;
    case errSecItemNotFound:
        return KeystoreStatus::NotFound;
    case errSecAuthFailed:
    case errSecUserCanceled:
    case errSecInteractionNotAllowed:
    case errSecInteractionRequired:
        return KeystoreStatus::Denied;
    case errSecNotAvailable:
    case errSecNoDefaultKeychain:
        return KeystoreStatus::Unavailable;
    default:
        return KeystoreStatus::Failed;
    }
}

CFMutableDictionaryRef new_query(const std::string& account) {
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, CFSTR("ai.lemonadeserver.nexus"));
    CFHolder<CFStringRef> cf_account(
        CFStringCreateWithCString(kCFAllocatorDefault, account.c_str(), kCFStringEncodingUTF8));
    CFDictionarySetValue(query, kSecAttrAccount, cf_account.get());
    return query;
}

class KeychainBackend final : public KeystoreBackend {
public:
    std::string name() const override { return "keychain"; }
    bool available() const override { return true; }

    KeystoreStatus put(std::span<const uint8_t> kek, const std::string& pubkey_b64,
                       nlohmann::json& kek_ref) override {
        const std::string account =
            std::string(kAccountPrefix) + pubkey_b64.substr(0, kAccountPubkeyChars);

        // The dictionary references the caller's bytes; no second copy of
        // the KEK is made on this side of the framework boundary.
        CFHolder<CFDataRef> value(CFDataCreateWithBytesNoCopy(
            kCFAllocatorDefault, kek.data(), static_cast<CFIndex>(kek.size()), kCFAllocatorNull));
        CFHolder<CFMutableDictionaryRef> add(new_query(account));
        CFDictionarySetValue(add.get(), kSecValueData, value.get());
        CFDictionarySetValue(add.get(), kSecAttrLabel, CFSTR("Lemonade Nexus device identity"));
        CFDictionarySetValue(add.get(), kSecAttrAccessible, kSecAttrAccessibleWhenUnlockedThisDeviceOnly);
        CFDictionarySetValue(add.get(), kSecAttrSynchronizable, kCFBooleanFalse);

        OSStatus status = SecItemAdd(add.get(), nullptr);
        if (status == errSecDuplicateItem) {
            // An item for this public key exists without a file that refers
            // to it (the file was removed by hand). Replace it.
            CFHolder<CFMutableDictionaryRef> stale(new_query(account));
            SecItemDelete(stale.get());
            status = SecItemAdd(add.get(), nullptr);
        }
        if (status != errSecSuccess) return from_os_status(status);
        kek_ref = account;
        return KeystoreStatus::Ok;
    }

    KeystoreStatus get(const nlohmann::json& kek_ref, nexus::crypto::SecureBuffer& kek) override {
        if (!kek_ref.is_string()) return KeystoreStatus::NotFound;
        CFHolder<CFMutableDictionaryRef> query(new_query(kek_ref.get<std::string>()));
        CFDictionarySetValue(query.get(), kSecReturnData, kCFBooleanTrue);
        CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitOne);

        CFHolder<CFTypeRef> result;
        const OSStatus status = SecItemCopyMatching(query.get(), result.out());
        if (status != errSecSuccess) return from_os_status(status);
        if (!result.get() || CFGetTypeID(result.get()) != CFDataGetTypeID()) return KeystoreStatus::Failed;
        const auto data = static_cast<CFDataRef>(result.get());
        const auto len = static_cast<std::size_t>(CFDataGetLength(data));
        kek = nexus::crypto::SecureBuffer(std::span<const uint8_t>(CFDataGetBytePtr(data), len));
        return KeystoreStatus::Ok;
    }

    KeystoreStatus remove(const nlohmann::json& kek_ref) override {
        if (!kek_ref.is_string()) return KeystoreStatus::NotFound;
        CFHolder<CFMutableDictionaryRef> query(new_query(kek_ref.get<std::string>()));
        return from_os_status(SecItemDelete(query.get()));
    }
};

}  // namespace

std::shared_ptr<KeystoreBackend> detail::make_platform_backend() {
    return std::make_shared<KeychainBackend>();
}

}  // namespace lnsdk

#endif  // __APPLE__
