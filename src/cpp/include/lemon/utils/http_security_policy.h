#pragma once

namespace lemon {
namespace utils {

// Trust boundary for outgoing HTTP requests. Selects which URL schemes are
// allowed for the initial request and, for request types that follow redirects,
// which schemes are allowed for redirect targets.
enum class HttpSecurityPolicy {
    // External hosts (Hugging Face, GitHub, release CDNs). Require https for the
    // initial request and every redirect hop; bound the redirect chain. This is
    // the default so untrusted destinations are HTTPS-only unless a caller opts
    // out for a specific trust boundary.
    ExternalHttpsOnly,
    // Lemonade-managed backends reached over loopback http://127.0.0.1. Require
    // http and never follow redirects.
    TrustedLoopback,
    // User-configured plaintext endpoints that explicitly opted in via
    // allow_insecure_http. Permit http and https; when redirects are enabled,
    // bound the redirect chain.
    AllowInsecureHttp,
};

}  // namespace utils
}  // namespace lemon
