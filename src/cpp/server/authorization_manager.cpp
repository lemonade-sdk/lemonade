#include "lemon/authorization_manager.h"

#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <lemon/utils/aixlog.hpp>

namespace fs = std::filesystem;

namespace lemon {

namespace {
constexpr const char* kPersistFileName = "authorized_clients.json";
constexpr const char* kSchemaVersion = "1";
} // namespace

AuthorizationManager::AuthorizationManager(std::string cache_dir)
    : cache_dir_(std::move(cache_dir))
    , persist_path_((fs::path(cache_dir_) / kPersistFileName).string())
{
    load_from_disk();
}

int64_t AuthorizationManager::now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool AuthorizationManager::is_allowed(const std::string& ip) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = decisions_.find(ip);
    return it != decisions_.end() && it->second.decision == AuthDecision::Allow;
}

bool AuthorizationManager::is_denied(const std::string& ip) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = decisions_.find(ip);
    return it != decisions_.end() && it->second.decision == AuthDecision::Deny;
}

int64_t AuthorizationManager::register_pending(const std::string& ip,
                                               const std::string& hostname) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = now_unix();
    auto it = pending_.find(ip);
    if (it == pending_.end()) {
        PendingAuthEntry entry{ip, hostname, now, now};
        pending_.emplace(ip, std::move(entry));
        return now;  // First-time registration; caller may want to notify.
    }
    // Refresh hostname only when we actually got one — don't overwrite a real
    // hostname with an empty header from a follow-up retry.
    if (!hostname.empty()) it->second.hostname = hostname;
    it->second.last_seen_unix = now;
    return it->second.first_seen_unix;
}

void AuthorizationManager::allow(const std::string& ip,
                                 const std::string& hostname) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        AuthEntry entry;
        entry.ip = ip;
        entry.hostname = hostname.empty()
            ? (pending_.count(ip) ? pending_[ip].hostname : "")
            : hostname;
        entry.decision = AuthDecision::Allow;
        entry.decided_at_unix = now_unix();
        decisions_[ip] = std::move(entry);
        pending_.erase(ip);
    }
    persist_to_disk();
}

void AuthorizationManager::deny(const std::string& ip,
                                const std::string& hostname) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        AuthEntry entry;
        entry.ip = ip;
        entry.hostname = hostname.empty()
            ? (pending_.count(ip) ? pending_[ip].hostname : "")
            : hostname;
        entry.decision = AuthDecision::Deny;
        entry.decided_at_unix = now_unix();
        decisions_[ip] = std::move(entry);
        pending_.erase(ip);
    }
    persist_to_disk();
}

bool AuthorizationManager::revoke(const std::string& ip) {
    bool removed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        removed = decisions_.erase(ip) > 0;
        // A revoked client may legitimately try again; clear any pending entry
        // too so the tray UI doesn't hold a stale duplicate.
        pending_.erase(ip);
    }
    if (removed) persist_to_disk();
    return removed;
}

std::vector<PendingAuthEntry> AuthorizationManager::list_pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PendingAuthEntry> out;
    out.reserve(pending_.size());
    for (const auto& [_, entry] : pending_) out.push_back(entry);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.first_seen_unix < b.first_seen_unix;
    });
    return out;
}

std::vector<AuthEntry> AuthorizationManager::list_decisions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AuthEntry> out;
    out.reserve(decisions_.size());
    for (const auto& [_, entry] : decisions_) out.push_back(entry);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.decided_at_unix < b.decided_at_unix;
    });
    return out;
}

nlohmann::json AuthorizationManager::pending_to_json() const {
    auto entries = list_pending();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : entries) {
        arr.push_back({
            {"ip", e.ip},
            {"hostname", e.hostname},
            {"first_seen", e.first_seen_unix},
            {"last_seen", e.last_seen_unix},
        });
    }
    return arr;
}

nlohmann::json AuthorizationManager::decisions_to_json() const {
    auto entries = list_decisions();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : entries) {
        arr.push_back({
            {"ip", e.ip},
            {"hostname", e.hostname},
            {"decision", e.decision == AuthDecision::Allow ? "allow" : "deny"},
            {"decided_at", e.decided_at_unix},
        });
    }
    return arr;
}

void AuthorizationManager::load_from_disk() {
    std::ifstream file(persist_path_);
    if (!file.is_open()) {
        // File doesn't exist yet — that's the normal first-run case, not an
        // error worth warning about.
        return;
    }
    try {
        nlohmann::json root;
        file >> root;
        if (!root.is_object() || !root.contains("entries") || !root["entries"].is_array()) {
            LOG(WARNING) << "[Auth] " << persist_path_
                         << " has unexpected shape; ignoring." << std::endl;
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& e : root["entries"]) {
            if (!e.is_object() || !e.contains("ip")) continue;
            AuthEntry entry;
            entry.ip = e.value("ip", "");
            entry.hostname = e.value("hostname", "");
            std::string d = e.value("decision", "");
            entry.decision = (d == "deny") ? AuthDecision::Deny : AuthDecision::Allow;
            entry.decided_at_unix = e.value("decided_at", static_cast<int64_t>(0));
            if (!entry.ip.empty()) decisions_[entry.ip] = std::move(entry);
        }
        LOG(INFO) << "[Auth] Loaded " << decisions_.size()
                  << " stored authorization decision(s) from " << persist_path_
                  << std::endl;
    } catch (const std::exception& e) {
        LOG(WARNING) << "[Auth] Failed to parse " << persist_path_ << ": "
                     << e.what() << " (continuing with empty allowlist)"
                     << std::endl;
    }
}

void AuthorizationManager::persist_to_disk() {
    nlohmann::json root;
    root["schema_version"] = kSchemaVersion;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& [_, entry] : decisions_) {
            arr.push_back({
                {"ip", entry.ip},
                {"hostname", entry.hostname},
                {"decision", entry.decision == AuthDecision::Allow ? "allow" : "deny"},
                {"decided_at", entry.decided_at_unix},
            });
        }
        root["entries"] = std::move(arr);
    }

    // Atomic write: serialize to a sibling .tmp file, then rename. Avoids a
    // half-written file if the process is killed mid-flush.
    fs::path tmp_path = fs::path(persist_path_).string() + ".tmp";
    try {
        fs::create_directories(fs::path(persist_path_).parent_path());
        {
            std::ofstream out(tmp_path);
            if (!out.is_open()) {
                LOG(WARNING) << "[Auth] Could not open " << tmp_path
                             << " for writing." << std::endl;
                return;
            }
            out << root.dump(2);
        }
        fs::rename(tmp_path, persist_path_);
    } catch (const std::exception& e) {
        LOG(WARNING) << "[Auth] Failed to persist " << persist_path_ << ": "
                     << e.what() << std::endl;
    }
}

} // namespace lemon
