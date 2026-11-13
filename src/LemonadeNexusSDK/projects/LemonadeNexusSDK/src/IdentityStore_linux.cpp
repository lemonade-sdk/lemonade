// Linux keystore backend: the KEK is an item in the user's Secret Service
// (GNOME Keyring, KDE Wallet with its Secret Service bridge) through
// libsecret. The library is loaded at run time with dlopen so the SDK has
// no build-time or package dependency on it. Without the library, without a
// session bus, or without a running Secret Service the backend is
// unavailable; the store then refuses and never writes a plaintext file.

#if defined(__linux__)

#include <LemonadeNexusSDK/IdentityStore.hpp>

#include <LemonadeNexus/Crypto/SecureBuffer.hpp>

#include <sodium.h>
#include <spdlog/spdlog.h>

#include <dlfcn.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

namespace lnsdk {

namespace {

// Minimal GLib and libsecret ABI, as declared in <glib.h> and
// <libsecret/secret-schema.h>. Field order and sizes must match the
// installed library; they have been stable since libsecret 0.18.
using gboolean = int;
using gchar = char;
using gint = int;
using gpointer = void*;
using GQuark = std::uint32_t;

struct GError {
    GQuark domain;
    gint code;
    gchar* message;
};

enum SecretSchemaAttributeType : int { SECRET_SCHEMA_ATTRIBUTE_STRING = 0 };
enum SecretSchemaFlags : int { SECRET_SCHEMA_NONE = 0 };
enum SecretError : int { SECRET_ERROR_IS_LOCKED = 2, SECRET_ERROR_NO_SUCH_OBJECT = 3 };

struct SecretSchemaAttribute {
    const gchar* name;
    SecretSchemaAttributeType type;
};

struct SecretSchema {
    const gchar* name;
    SecretSchemaFlags flags;
    SecretSchemaAttribute attributes[32];
    gint reserved;
    gpointer reserved1;
    gpointer reserved2;
    gpointer reserved3;
    gpointer reserved4;
    gpointer reserved5;
    gpointer reserved6;
    gpointer reserved7;
};

constexpr const char* kCollectionDefault = "default";  // SECRET_COLLECTION_DEFAULT
constexpr const char* kAttrPubkey = "pubkey";

const SecretSchema kSchema = {
    "ai.lemonadeserver.nexus.identity-kek",
    SECRET_SCHEMA_NONE,
    {{kAttrPubkey, SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}},
    0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
};

struct Libsecret {
    void* handle{nullptr};
    gboolean (*store)(const SecretSchema*, const gchar*, const gchar*, const gchar*, gpointer,
                      GError**, ...){nullptr};
    gchar* (*lookup)(const SecretSchema*, gpointer, GError**, ...){nullptr};
    gboolean (*clear)(const SecretSchema*, gpointer, GError**, ...){nullptr};
    void (*password_free)(gchar*){nullptr};
    GQuark (*error_quark)(){nullptr};
    void (*g_error_free)(GError*){nullptr};

    [[nodiscard]] bool loaded() const noexcept { return handle != nullptr; }
};

template <typename Fn>
bool resolve(void* handle, const char* name, Fn& out) {
    out = reinterpret_cast<Fn>(::dlsym(handle, name));
    return out != nullptr;
}

const Libsecret& libsecret() {
    static const Libsecret lib = [] {
        Libsecret l;
        void* handle = ::dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            spdlog::info("[IdentityStore] libsecret-1.so.0 not available; no Linux keystore");
            return l;
        }
        // g_error_free comes from GLib, a dependency of libsecret, so the
        // libsecret handle resolves it as well.
        if (resolve(handle, "secret_password_store_sync", l.store) &&
            resolve(handle, "secret_password_lookup_sync", l.lookup) &&
            resolve(handle, "secret_password_clear_sync", l.clear) &&
            resolve(handle, "secret_password_free", l.password_free) &&
            resolve(handle, "secret_error_get_quark", l.error_quark) &&
            resolve(handle, "g_error_free", l.g_error_free)) {
            l.handle = handle;
        } else {
            spdlog::warn("[IdentityStore] libsecret-1.so.0 lacks the expected symbols");
            ::dlclose(handle);
        }
        return l;
    }();
    return lib;
}

/// Translate and free a GError. A null error means success for the caller.
KeystoreStatus take_error(const Libsecret& lib, GError* error) {
    if (!error) return KeystoreStatus::Ok;
    KeystoreStatus status = KeystoreStatus::Unavailable;  // D-Bus or service failure
    if (error->domain == lib.error_quark()) {
        if (error->code == SECRET_ERROR_IS_LOCKED) {
            status = KeystoreStatus::Denied;
        } else if (error->code == SECRET_ERROR_NO_SUCH_OBJECT) {
            status = KeystoreStatus::NotFound;
        } else {
            status = KeystoreStatus::Failed;
        }
    }
    lib.g_error_free(error);
    return status;
}

class LibsecretBackend final : public KeystoreBackend {
public:
    std::string name() const override { return "libsecret"; }

    bool available() const override {
        const Libsecret& lib = libsecret();
        if (!lib.loaded()) return false;
        // A lookup of an item that cannot exist reaches the Secret Service
        // without touching any real item: a service error means unavailable.
        GError* error = nullptr;
        gchar* found = lib.lookup(&kSchema, nullptr, &error, kAttrPubkey, "", nullptr);
        if (found) lib.password_free(found);
        const KeystoreStatus status = take_error(lib, error);
        return status != KeystoreStatus::Unavailable;
    }

    KeystoreStatus put(std::span<const uint8_t> kek, const std::string& pubkey_b64,
                       nlohmann::json& kek_ref) override {
        const Libsecret& lib = libsecret();
        if (!lib.loaded()) return KeystoreStatus::Unavailable;

        // libsecret stores text; the KEK travels as base64 and is wiped
        // after the call.
        std::string password = Identity::to_base64(kek);
        GError* error = nullptr;
        const gboolean ok = lib.store(&kSchema, kCollectionDefault, "Lemonade Nexus device identity",
                                      password.c_str(), nullptr, &error, kAttrPubkey,
                                      pubkey_b64.c_str(), nullptr);
        sodium_memzero(password.data(), password.size());
        const KeystoreStatus status = take_error(lib, error);
        if (status != KeystoreStatus::Ok) return status;
        if (!ok) return KeystoreStatus::Failed;
        kek_ref = pubkey_b64;
        return KeystoreStatus::Ok;
    }

    KeystoreStatus get(const nlohmann::json& kek_ref, nexus::crypto::SecureBuffer& kek) override {
        const Libsecret& lib = libsecret();
        if (!lib.loaded()) return KeystoreStatus::Unavailable;
        if (!kek_ref.is_string()) return KeystoreStatus::NotFound;

        GError* error = nullptr;
        gchar* password = lib.lookup(&kSchema, nullptr, &error, kAttrPubkey,
                                     kek_ref.get<std::string>().c_str(), nullptr);
        const KeystoreStatus status = take_error(lib, error);
        if (status != KeystoreStatus::Ok) {
            if (password) lib.password_free(password);
            return status;
        }
        if (!password) return KeystoreStatus::NotFound;

        const std::size_t len = std::strlen(password);
        nexus::crypto::SecureBuffer decoded(len == 0 ? 1 : len);
        std::size_t bin_len = 0;
        const int rc = sodium_base642bin(decoded.data(), decoded.size(), password, len, nullptr,
                                         &bin_len, nullptr, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
        lib.password_free(password);
        if (rc != 0) return KeystoreStatus::Failed;
        kek = nexus::crypto::SecureBuffer(std::span<const uint8_t>(decoded.data(), bin_len));
        return KeystoreStatus::Ok;
    }

    KeystoreStatus remove(const nlohmann::json& kek_ref) override {
        const Libsecret& lib = libsecret();
        if (!lib.loaded()) return KeystoreStatus::Unavailable;
        if (!kek_ref.is_string()) return KeystoreStatus::NotFound;

        GError* error = nullptr;
        const gboolean removed = lib.clear(&kSchema, nullptr, &error, kAttrPubkey,
                                           kek_ref.get<std::string>().c_str(), nullptr);
        const KeystoreStatus status = take_error(lib, error);
        if (status != KeystoreStatus::Ok) return status;
        return removed ? KeystoreStatus::Ok : KeystoreStatus::NotFound;
    }
};

}  // namespace

std::shared_ptr<KeystoreBackend> detail::make_platform_backend() {
    return std::make_shared<LibsecretBackend>();
}

}  // namespace lnsdk

#endif  // __linux__
