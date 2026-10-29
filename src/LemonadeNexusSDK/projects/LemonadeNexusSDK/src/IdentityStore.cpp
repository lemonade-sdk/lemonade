#include <LemonadeNexusSDK/IdentityStore.hpp>

#include "OwnerOnlyFile.hpp"

#include <LemonadeNexus/Crypto/SecureBuffer.hpp>

#include <sodium.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <objbase.h>
#include <shlobj.h>
#else
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;
using nexus::crypto::SecureBuffer;

namespace lnsdk {

namespace {

constexpr std::size_t kKekBytes = crypto_secretbox_KEYBYTES;
constexpr std::size_t kNonceBytes = crypto_secretbox_NONCEBYTES;
constexpr std::size_t kWrappedSeedBytes = kEd25519SeedSize + crypto_secretbox_MACBYTES;

StoreResult from_keystore(KeystoreStatus status) {
    switch (status) {
    case KeystoreStatus::Ok:          return StoreResult::Ok;
    case KeystoreStatus::NotFound:    return StoreResult::Invalid;
    case KeystoreStatus::Denied:      return StoreResult::Denied;
    case KeystoreStatus::Unavailable: return StoreResult::Unsupported;
    case KeystoreStatus::Failed:      break;
    }
    return StoreResult::Failed;
}

/// Create `dir` and its parents. The leaf directory is readable by its owner
/// only, so the identity file and its temporary siblings are never listed by
/// other users.
bool ensure_private_dir(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec && !fs::is_directory(dir, ec)) return false;
#ifndef _WIN32
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
    if (ec) return false;
#endif
    return true;
}

std::mutex g_override_mutex;
std::optional<fs::path> g_override_dir;
std::shared_ptr<KeystoreBackend> g_override_backend;

#if !defined(__APPLE__) && !defined(_WIN32) && !defined(__linux__)
/// Platforms without a keystore integration: everything is unavailable.
class NoKeystore final : public KeystoreBackend {
public:
    std::string name() const override { return "none"; }
    bool available() const override { return false; }
    KeystoreStatus put(std::span<const uint8_t>, const std::string&, json&) override {
        return KeystoreStatus::Unavailable;
    }
    KeystoreStatus get(const json&, SecureBuffer&) override { return KeystoreStatus::Unavailable; }
    KeystoreStatus remove(const json&) override { return KeystoreStatus::Unavailable; }
};
#endif

}  // namespace

#if !defined(__APPLE__) && !defined(_WIN32) && !defined(__linux__)
std::shared_ptr<KeystoreBackend> detail::make_platform_backend() {
    return std::make_shared<NoKeystore>();
}
#endif

/// The parsed identity file. Nothing in it is secret.
struct IdentityStore::Record {
    std::string public_key;  ///< base64 of the raw Ed25519 public key
    std::string backend;
    std::vector<uint8_t> nonce;
    std::vector<uint8_t> ct;
    json kek_ref;
};

IdentityStore::IdentityStore(fs::path dir, std::shared_ptr<KeystoreBackend> backend)
    : dir_(std::move(dir)), backend_(std::move(backend)) {
    if (!backend_) {
        throw std::invalid_argument("IdentityStore requires a keystore backend");
    }
}

IdentityStore IdentityStore::for_user() {
    {
        std::lock_guard lock(g_override_mutex);
        if (g_override_backend) {
            return IdentityStore(*g_override_dir, g_override_backend);
        }
    }
    return IdentityStore(user_dir().value_or(fs::path{}), platform_backend());
}

std::shared_ptr<KeystoreBackend> IdentityStore::platform_backend() {
    static std::shared_ptr<KeystoreBackend> backend = detail::make_platform_backend();
    return backend;
}

std::optional<fs::path> IdentityStore::user_dir() {
#ifdef _WIN32
    PWSTR folder = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder) != S_OK || !folder) {
        if (folder) CoTaskMemFree(folder);
        return std::nullopt;
    }
    fs::path base(folder);
    CoTaskMemFree(folder);
    return base / "LemonadeNexus" / "identity";
#else
    // The effective user owns the store. SUDO_USER is never consulted: a
    // process elevated with sudo belongs to root and gets root's store.
    std::string home;
    {
        std::vector<char> buf(16384);
        struct passwd pw {};
        struct passwd* result = nullptr;
        if (::getpwuid_r(::geteuid(), &pw, buf.data(), buf.size(), &result) == 0 && result &&
            result->pw_dir && result->pw_dir[0] == '/') {
            home = result->pw_dir;
        }
    }
    if (home.empty()) {
        // Last resort: the account database has no home directory for the
        // effective user (for example a container with a synthetic uid).
        if (const char* env_home = std::getenv("HOME"); env_home && env_home[0] == '/') {
            home = env_home;
        }
    }
    if (home.empty()) return std::nullopt;
#ifdef __APPLE__
    return fs::path(home) / "Library" / "Application Support" / "LemonadeNexus" / "identity";
#else
    fs::path base;
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && xdg[0] == '/') {
        base = xdg;
    } else {
        base = fs::path(home) / ".local" / "share";
    }
    return base / "lemonade-nexus" / "identity";
#endif
#endif
}

StoreResult IdentityStore::read_record(Record& out) const {
    if (dir_.empty()) return StoreResult::Failed;
    const fs::path file = file_path();
    std::error_code ec;
    if (!fs::exists(file, ec)) return StoreResult::NotFound;

    std::string text;
    {
        std::ifstream in(file, std::ios::binary);
        if (!in.is_open()) return StoreResult::Failed;
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || j.value("v", 0) != kFormatVersion) return StoreResult::Invalid;
    const auto pub = j.find("public_key");
    const auto backend = j.find("backend");
    const auto wrapped = j.find("wrapped_seed");
    const auto kek_ref = j.find("kek_ref");
    if (pub == j.end() || !pub->is_string() || backend == j.end() || !backend->is_string() ||
        wrapped == j.end() || !wrapped->is_object() || kek_ref == j.end()) {
        return StoreResult::Invalid;
    }
    const auto nonce = wrapped->find("nonce");
    const auto ct = wrapped->find("ct");
    if (nonce == wrapped->end() || !nonce->is_string() || ct == wrapped->end() || !ct->is_string()) {
        return StoreResult::Invalid;
    }

    Record record;
    record.public_key = pub->get<std::string>();
    record.backend = backend->get<std::string>();
    record.nonce = Identity::from_base64(nonce->get<std::string>());
    record.ct = Identity::from_base64(ct->get<std::string>());
    record.kek_ref = *kek_ref;
    if (Identity::from_base64(record.public_key).size() != kEd25519PublicKeySize ||
        record.nonce.size() != kNonceBytes || record.ct.size() != kWrappedSeedBytes) {
        return StoreResult::Invalid;
    }
    out = std::move(record);
    return StoreResult::Ok;
}

StoreResult IdentityStore::load(Identity& out) const {
    if (sodium_init() < 0) return StoreResult::Failed;
    Record record;
    if (const auto rc = read_record(record); rc != StoreResult::Ok) return rc;
    if (!backend_->available()) return StoreResult::Unsupported;
    if (record.backend != backend_->name()) {
        spdlog::warn("[IdentityStore] identity in {} was wrapped for backend {}, this platform uses {}",
                     dir_.string(), record.backend, backend_->name());
        return StoreResult::Unsupported;
    }

    SecureBuffer kek;
    if (const auto st = backend_->get(record.kek_ref, kek); st != KeystoreStatus::Ok) {
        return from_keystore(st);
    }
    if (kek.size() != kKekBytes) return StoreResult::Invalid;

    SecureBuffer seed(kEd25519SeedSize);
    if (crypto_secretbox_open_easy(seed.data(), record.ct.data(), record.ct.size(),
                                   record.nonce.data(), kek.data()) != 0) {
        spdlog::warn("[IdentityStore] wrapped seed in {} failed verification", dir_.string());
        return StoreResult::Invalid;
    }

    Identity identity;
    identity.from_seed(seed.span());
    if (!identity.is_valid()) return StoreResult::Failed;
    if (Identity::to_base64(identity.public_key()) != record.public_key) {
        spdlog::warn("[IdentityStore] public key in {} does not match the wrapped seed", dir_.string());
        return StoreResult::Invalid;
    }
    out = std::move(identity);
    return StoreResult::Ok;
}

StoreResult IdentityStore::create(Identity& out) {
    if (sodium_init() < 0) return StoreResult::Failed;
    if (dir_.empty()) return StoreResult::Failed;
    std::error_code ec;
    if (fs::exists(file_path(), ec)) return StoreResult::Exists;
    if (!backend_->available()) return StoreResult::Unsupported;

    SecureBuffer seed(kEd25519SeedSize);
    randombytes_buf(seed.data(), seed.size());
    Identity identity;
    identity.from_seed(seed.span());
    if (!identity.is_valid()) return StoreResult::Failed;

    if (const auto rc = store_seed(seed.span(), identity); rc != StoreResult::Ok) return rc;
    out = std::move(identity);
    return StoreResult::Ok;
}

StoreResult IdentityStore::import(const Identity& identity) {
    if (sodium_init() < 0) return StoreResult::Failed;
    if (!identity.is_valid()) return StoreResult::Invalid;
    if (dir_.empty()) return StoreResult::Failed;
    std::error_code ec;
    if (fs::exists(file_path(), ec)) return StoreResult::Exists;
    if (!backend_->available()) return StoreResult::Unsupported;
    return store_seed(identity.seed(), identity);
}

StoreResult IdentityStore::store_seed(std::span<const uint8_t> seed, const Identity& identity) {
    SecureBuffer kek(kKekBytes);
    randombytes_buf(kek.data(), kek.size());
    std::vector<uint8_t> nonce(kNonceBytes);
    randombytes_buf(nonce.data(), nonce.size());
    std::vector<uint8_t> ct(kWrappedSeedBytes);
    if (crypto_secretbox_easy(ct.data(), seed.data(), seed.size(), nonce.data(), kek.data()) != 0) {
        return StoreResult::Failed;
    }

    const std::string public_key = Identity::to_base64(identity.public_key());
    json kek_ref;
    if (const auto st = backend_->put(kek.span(), public_key, kek_ref); st != KeystoreStatus::Ok) {
        spdlog::warn("[IdentityStore] keystore {} refused to store the key", backend_->name());
        return st == KeystoreStatus::NotFound ? StoreResult::Failed : from_keystore(st);
    }
    kek.clear();

    json j;
    j["v"] = kFormatVersion;
    j["public_key"] = public_key;
    j["backend"] = backend_->name();
    j["wrapped_seed"] = {{"nonce", Identity::to_base64(nonce)}, {"ct", Identity::to_base64(ct)}};
    j["kek_ref"] = kek_ref;

    if (!ensure_private_dir(dir_) || !detail::replace_owner_only_file(file_path(), j.dump(2))) {
        spdlog::error("[IdentityStore] cannot write the identity file in {}", dir_.string());
        // Do not leave a key in the keystore that no file refers to.
        (void)backend_->remove(kek_ref);
        return StoreResult::Failed;
    }
    spdlog::info("[IdentityStore] stored identity (backend={}, dir={})", backend_->name(), dir_.string());
    return StoreResult::Ok;
}

StoreResult IdentityStore::remove() {
    if (dir_.empty()) return StoreResult::Failed;
    const fs::path file = file_path();
    std::error_code ec;
    if (!fs::exists(file, ec)) return StoreResult::NotFound;

    // Remove the keystore item first; the file is the only reference to it.
    // A file that no longer parses, or that belongs to another keystore, is
    // removed without touching the keystore.
    Record record;
    if (read_record(record) == StoreResult::Ok && backend_->available() &&
        record.backend == backend_->name()) {
        const auto st = backend_->remove(record.kek_ref);
        if (st == KeystoreStatus::Denied || st == KeystoreStatus::Failed) {
            spdlog::warn("[IdentityStore] keystore {} did not remove the key", backend_->name());
            return from_keystore(st);
        }
    }
    if (!fs::remove(file, ec) || ec) return StoreResult::Failed;
    spdlog::info("[IdentityStore] removed identity (backend={}, dir={})", backend_->name(), dir_.string());
    return StoreResult::Ok;
}

IdentityStoreStatus IdentityStore::status() const {
    IdentityStoreStatus st;
    st.dir = dir_;
    st.backend = backend_->available() ? backend_->name() : "none";
    Record record;
    if (read_record(record) == StoreResult::Ok) {
        st.present = true;
        st.public_key = "ed25519:" + record.public_key;
        Identity identity;
        st.unlocked = load(identity) == StoreResult::Ok;
    }
    return st;
}

IdentityStore::ScopedOverride::ScopedOverride(fs::path dir, std::shared_ptr<KeystoreBackend> backend) {
    std::lock_guard lock(g_override_mutex);
    g_override_dir = std::move(dir);
    g_override_backend = std::move(backend);
}

IdentityStore::ScopedOverride::~ScopedOverride() {
    std::lock_guard lock(g_override_mutex);
    g_override_dir.reset();
    g_override_backend.reset();
}

}  // namespace lnsdk
