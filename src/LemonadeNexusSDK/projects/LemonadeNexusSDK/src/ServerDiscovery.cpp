#include <LemonadeNexusSDK/ServerDiscovery.hpp>

#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <cctype>
#include <ctime>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

#if OPENSSL_VERSION_NUMBER < 0x30000000L
#error "server discovery needs OpenSSL 3 (BIO_do_connect_retry, BIO_wait)"
#endif

namespace lnsdk {

namespace {

constexpr std::size_t kMaxAddresses = 16;
constexpr int kHandshakeTimeoutSeconds = 5;
constexpr unsigned int kNapMilliseconds = 50;

std::string canonical(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!name.empty() && name.back() == '.') name.pop_back();
    return name;
}

bool hostname_chars(const std::string& name) {
    if (name.empty() || name.size() > 253 || name.front() == '.' || name.back() == '.' ||
        name.find("..") != std::string::npos) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '.';
    });
}

std::vector<std::string> resolve_ipv4_all(const std::string& name) {
    std::vector<std::string> out;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(name.c_str(), nullptr, &hints, &res) != 0 || !res) return out;
    for (auto* p = res; p; p = p->ai_next) {
        char buf[INET_ADDRSTRLEN] = {};
        auto* sin = reinterpret_cast<sockaddr_in*>(p->ai_addr);
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof buf) &&
            std::find(out.begin(), out.end(), buf) == out.end()) {
            out.emplace_back(buf);
        }
    }
    freeaddrinfo(res);
    return out;
}

/// The subject common name of the leaf certificate at address:port, or "".
/// No verification here: the name is a candidate, and the verified-TLS
/// transport checks the certificate against that name before anything the
/// server says is trusted.
std::string certificate_name(const std::string& address, uint16_t port) {
    std::string name;
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return name;
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    if (BIO* bio = BIO_new_ssl_connect(ctx)) {
        SSL* ssl = nullptr;
        BIO_get_ssl(bio, &ssl);
        if (ssl) {
            SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);
            BIO_set_conn_hostname(bio, address.c_str());
            BIO_set_conn_port(bio, std::to_string(port).c_str());
            BIO_set_nbio(bio, 1);
            const time_t deadline = std::time(nullptr) + kHandshakeTimeoutSeconds;
            if (BIO_do_connect_retry(bio, kHandshakeTimeoutSeconds, kNapMilliseconds) == 1) {
                int rc = BIO_do_handshake(bio);
                while (rc != 1 && BIO_should_retry(bio) &&
                       BIO_wait(bio, deadline, kNapMilliseconds) == 1) {
                    rc = BIO_do_handshake(bio);
                }
                if (rc == 1) {
                    if (X509* cert = SSL_get1_peer_certificate(ssl)) {
                        char buf[256] = {};
                        if (X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName,
                                                      buf, sizeof buf) > 0) {
                            name = buf;
                        }
                        X509_free(cert);
                    }
                }
            }
        }
        BIO_free_all(bio);
    }
    SSL_CTX_free(ctx);
    return name;
}

template <typename T>
Result<T> fail(std::string why) {
    Result<T> r;
    r.ok = false;
    r.http_status = 0;
    r.error = std::move(why);
    return r;
}

}  // namespace

bool discovered_name_in_domain(const std::string& name, const std::string& domain) {
    const auto host = canonical(name);
    const auto zone = canonical(domain);
    if (zone.empty() || host.size() <= zone.size() + 1) return false;
    if (host.compare(host.size() - zone.size(), zone.size(), zone) != 0) return false;
    if (host[host.size() - zone.size() - 1] != '.') return false;
    return hostname_chars(host);
}

Result<std::vector<DiscoveredServer>> discover_servers_at(const std::vector<std::string>& addresses,
                                                          const std::string& domain,
                                                          uint16_t port) {
    if (!hostname_chars(canonical(domain))) {
        return fail<std::vector<DiscoveredServer>>("the network domain is not a hostname");
    }
    if (port == 0) return fail<std::vector<DiscoveredServer>>("the server port must be 1 to 65535");
    Result<std::vector<DiscoveredServer>> result;
    std::size_t probed = 0;
    for (const auto& address : addresses) {
        if (probed++ == kMaxAddresses) break;
        const auto name = canonical(certificate_name(address, port));
        if (!discovered_name_in_domain(name, domain)) continue;
        const bool seen = std::any_of(result.value.begin(), result.value.end(),
                                      [&](const DiscoveredServer& s) { return s.host == name; });
        if (!seen) result.value.push_back({name, port, address});
    }
    if (result.value.empty()) {
        return fail<std::vector<DiscoveredServer>>(
            "no server at " + std::to_string(addresses.size()) +
            " address(es) presented a certificate name below " + canonical(domain));
    }
    result.ok = true;
    result.http_status = 200;
    return result;
}

Result<std::vector<DiscoveredServer>> discover_servers(const std::string& domain,
                                                       const std::string& region,
                                                       uint16_t port) {
    const auto zone = canonical(domain);
    const auto area = canonical(region);
    if (!hostname_chars(zone) || !hostname_chars(area) || area.find('.') != std::string::npos) {
        return fail<std::vector<DiscoveredServer>>(
            "the network domain and region must be hostname labels");
    }
    const auto region_name = area + ".seip." + zone;
    const auto addresses = resolve_ipv4_all(region_name);
    if (addresses.empty()) {
        return fail<std::vector<DiscoveredServer>>(region_name + " resolved to no IPv4 address");
    }
    auto found = discover_servers_at(addresses, zone, port);
    if (!found.ok) found.error = "from " + region_name + ": " + found.error;
    return found;
}

}  // namespace lnsdk
