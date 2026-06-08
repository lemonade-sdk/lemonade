#pragma once

// AuthorizationManager — host-side gate for cross-machine inference requests.
//
// When a Lemonade client on another machine targets this host (e.g. via the
// "Run on" picker in the Tauri app), the host's owner deserves a chance to
// approve before their CPU/GPU/NPU starts running someone else's prompts.
// This class tracks per-IP decisions, surfaces pending requests for the tray
// UI to render, and persists allow/deny choices across restarts.
//
// Design notes:
// - State is keyed by client IP. Hostnames are best-effort labels for the UI;
//   they come from an X-Lemonade-Client-Hostname header on incoming requests.
// - Decisions persist to <cache_dir>/authorized_clients.json. Pending entries
//   are NEVER persisted — they vanish on restart, which matches user intuition
//   ("I haven't been at my desk; the prompts shouldn't queue forever").
// - Loopback requests are ALWAYS allowed and never touch this manager. The
//   gate is purely for non-loopback origins.
// - Methods are thread-safe. lemond's HTTP server is multi-threaded, the
//   tray polls on its own thread, and persistence runs on whichever thread
//   makes the decision.

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace lemon {

enum class AuthDecision {
    Allow,
    Deny,
};

struct AuthEntry {
    std::string ip;
    std::string hostname;       // Display label; may be empty if header was absent.
    AuthDecision decision;
    int64_t decided_at_unix;    // seconds since epoch
};

struct PendingAuthEntry {
    std::string ip;
    std::string hostname;
    int64_t first_seen_unix;    // when we first saw this requester in this lemond run
    int64_t last_seen_unix;     // refreshed every blocked request
};

class AuthorizationManager {
public:
    // `cache_dir` is the lemonade cache directory; the persistence file lives
    // at `<cache_dir>/authorized_clients.json`. Loads existing entries on
    // construction; missing/corrupt files are treated as empty (with a log).
    explicit AuthorizationManager(std::string cache_dir);

    // Lookup helpers used by the HTTP pre-routing handler.
    bool is_allowed(const std::string& ip) const;
    bool is_denied(const std::string& ip) const;

    // Called when a non-loopback request hits a gated endpoint without an
    // existing decision. Adds/refreshes the pending entry so the tray can
    // surface it; returns the updated entry's `first_seen_unix` so the
    // caller can decide if this is a "new" request worth notifying about.
    int64_t register_pending(const std::string& ip, const std::string& hostname);

    // Tray-driven decision endpoints.
    void allow(const std::string& ip, const std::string& hostname);
    void deny(const std::string& ip, const std::string& hostname);

    // Forget a stored decision (both allow and deny). Used by the tray's
    // "Revoke" action. Returns true if anything was removed.
    bool revoke(const std::string& ip);

    // Snapshot accessors for the tray. Returned vectors are sorted by
    // first_seen_unix (pending) / decided_at_unix (decisions) for stable UI.
    std::vector<PendingAuthEntry> list_pending() const;
    std::vector<AuthEntry> list_decisions() const;

    // JSON helpers used by the /internal/auth/* HTTP handlers.
    nlohmann::json pending_to_json() const;
    nlohmann::json decisions_to_json() const;

private:
    void load_from_disk();      // Best-effort; logs and returns on parse error.
    void persist_to_disk();     // Atomic write via tmp-then-rename.
    static int64_t now_unix();

    std::string cache_dir_;
    std::string persist_path_;

    mutable std::mutex mutex_;
    // ip -> entry. Decisions live here; pending lives in `pending_` until
    // resolved or process restarts.
    std::unordered_map<std::string, AuthEntry> decisions_;
    std::unordered_map<std::string, PendingAuthEntry> pending_;
};

} // namespace lemon
