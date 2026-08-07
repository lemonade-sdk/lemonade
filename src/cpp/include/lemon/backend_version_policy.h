#pragma once

#include <string>

// Pure decision logic for resolving a user's "latest" *_bin config pin into a
// concrete release tag. Kept free of I/O (no network, no filesystem, no global
// config) so the policy can be unit-tested in isolation; callers gather the
// inputs (live GitHub lookup result + installed version.txt) and apply the
// result. See resolve_user_version() in backend_manager.cpp.
namespace lemon {

struct LatestPinResolution {
    // Concrete release tag to use. Empty only when resolution failed, in which
    // case `error` explains why.
    std::string version;
    // Human-readable reason the pin could not be resolved; empty on success.
    std::string error;
    // True when `version` came from the installed binary rather than a live
    // GitHub lookup (so the caller can log a "falling back" warning).
    bool used_installed_fallback = false;
};

// Resolve a "latest" pin.
//
//   offline           — true when the caller is in offline mode and so did not
//                        attempt the live lookup; when true the GitHub result
//                        is ignored. Every other reason the live tag is absent
//                        (HTTP 504, network error, no_fetch_executables, etc.)
//                        is conveyed by an empty `fetched_latest`, not this flag.
//   fetched_latest     — tag returned by the live GitHub lookup; empty when the
//                        lookup failed (e.g. HTTP 504, network error) or was
//                        skipped.
//   installed_version  — contents of the installed version.txt for this
//                        (recipe, backend); empty when nothing is installed.
//
// Policy: prefer a freshly-fetched tag; otherwise fall back to the binary
// already installed so a transient GitHub failure (or offline mode) does not
// block loading a model that is otherwise ready. Only when neither is available
// does resolution fail.
inline LatestPinResolution resolve_latest_pin(const std::string& recipe,
                                              const std::string& backend,
                                              bool offline,
                                              const std::string& fetched_latest,
                                              const std::string& installed_version) {
    if (!offline && !fetched_latest.empty()) {
        return {fetched_latest, "", false};
    }
    if (!installed_version.empty()) {
        return {installed_version, "", true};
    }
    const std::string why = offline
        ? "offline mode and no installed version found"
        : "GitHub lookup failed and no installed version found";
    return {"", "Cannot resolve 'latest' for " + recipe + ":" + backend + ": " + why, false};
}

} // namespace lemon
