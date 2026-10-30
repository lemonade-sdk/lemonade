#include <lemon/utils/url_utils.h>

#include <cassert>
#include <cstdio>
#include <string>

struct TestResult {
    int passed = 0;
    int failed = 0;

    void ok(const std::string& name) {
        printf("[PASS] %s\n", name.c_str());
        ++passed;
    }

    void fail(const std::string& name) {
        printf("[FAIL] %s\n", name.c_str());
        ++failed;
    }
};

int main() {
    TestResult result;

    // Test case 1: simple HTTP URL
    {
        std::string clean_host;
        int port = 13305;
        bool is_ssl = false;
        lemon::utils::parse_target_url("http://localhost:8080", clean_host, port, is_ssl, true);
        if (clean_host == "localhost" && port == 8080 && !is_ssl) {
            result.ok("simple HTTP URL");
        } else {
            result.fail("simple HTTP URL");
        }
    }

    // Test case 2: simple HTTPS URL
    {
        std::string clean_host;
        int port = 13305;
        bool is_ssl = false;
        lemon::utils::parse_target_url("https://my-server.com", clean_host, port, is_ssl, true);
        if (clean_host == "my-server.com" && port == 443 && is_ssl) {
            result.ok("simple HTTPS URL");
        } else {
            result.fail("simple HTTPS URL");
        }
    }

    // Test case 3: HTTPS URL with explicit port
    {
        std::string clean_host;
        int port = 13305;
        bool is_ssl = false;
        lemon::utils::parse_target_url("https://my-server.com:12345", clean_host, port, is_ssl, true);
        if (clean_host == "my-server.com" && port == 12345 && is_ssl) {
            result.ok("HTTPS URL with explicit port");
        } else {
            result.fail("HTTPS URL with explicit port");
        }
    }

    // Test case 4: no scheme, with port
    {
        std::string clean_host;
        int port = 13305;
        bool is_ssl = false;
        lemon::utils::parse_target_url("localhost:9000", clean_host, port, is_ssl, true);
        if (clean_host == "localhost" && port == 9000 && !is_ssl) {
            result.ok("no scheme, with port");
        } else {
            result.fail("no scheme, with port");
        }
    }

    // Test case 5: no scheme, no port
    {
        std::string clean_host;
        int port = 13305;
        bool is_ssl = false;
        lemon::utils::parse_target_url("localhost", clean_host, port, is_ssl, true);
        if (clean_host == "localhost" && port == 13305 && !is_ssl) {
            result.ok("no scheme, no port");
        } else {
            result.fail("no scheme, no port");
        }
    }

    // Test case 6: HTTPS URL, no port, override_default_port = false
    {
        std::string clean_host;
        int port = 12345;
        bool is_ssl = false;
        lemon::utils::parse_target_url("https://my-server.com", clean_host, port, is_ssl, false);
        if (clean_host == "my-server.com" && port == 12345 && is_ssl) {
            result.ok("HTTPS URL, no port, override_default_port = false");
        } else {
            result.fail("HTTPS URL, no port, override_default_port = false");
        }
    }

    // Test case 7: URL with query parameters and fragments
    {
        std::string clean_host;
        int port = 13305;
        bool is_ssl = false;
        lemon::utils::parse_target_url("http://localhost:8080?foo=bar#frag", clean_host, port, is_ssl, true);
        if (clean_host == "localhost" && port == 8080 && !is_ssl) {
            result.ok("URL with query parameters and fragments");
        } else {
            result.fail("URL with query parameters and fragments");
        }
    }

    // Test case 8: Case-insensitive schemes
    {
        std::string clean_host;
        int port = 13305;
        bool is_ssl = false;
        lemon::utils::parse_target_url("HTTPS://my-server.com", clean_host, port, is_ssl, true);
        if (clean_host == "my-server.com" && port == 443 && is_ssl) {
            result.ok("Case-insensitive schemes");
        } else {
            result.fail("Case-insensitive schemes");
        }
    }

    // Test case 9: Bracketed IPv6 with port
    {
        std::string clean_host;
        int port = 13305;
        bool is_ssl = false;
        lemon::utils::parse_target_url("[::1]:8080", clean_host, port, is_ssl, true);
        if (clean_host == "[::1]" && port == 8080) {
            result.ok("Bracketed IPv6 with port");
        } else {
            result.fail("Bracketed IPv6 with port");
        }
    }

    // Test case 10: Bracketless IPv6 (no port)
    {
        std::string clean_host;
        int port = 13305;
        bool is_ssl = false;
        lemon::utils::parse_target_url("::1", clean_host, port, is_ssl, true);
        if (clean_host == "::1" && port == 13305) {
            result.ok("Bracketless IPv6");
        } else {
            result.fail("Bracketless IPv6");
        }
    }

    // Test case 11: Invalid port format
    {
        std::string clean_host;
        int port = 13305;
        bool is_ssl = false;
        lemon::utils::parse_target_url("localhost:abc", clean_host, port, is_ssl, true);
        if (port == -1) {
            result.ok("Invalid port format handled");
        } else {
            result.fail("Invalid port format handled");
        }
    }

    // Test case 12: Port 0
    {
        std::string clean_host;
        int port = 13305;
        bool is_ssl = false;
        lemon::utils::parse_target_url("localhost:0", clean_host, port, is_ssl, true);
        if (port == -1) {
            result.ok("Port 0 handled as invalid");
        } else {
            result.fail("Port 0 handled as invalid");
        }
    }

    // Test case 13: Empty port string
    {
        std::string clean_host;
        int port = 13305;
        bool is_ssl = false;
        lemon::utils::parse_target_url("localhost:", clean_host, port, is_ssl, true);
        if (port == -1) {
            result.ok("Empty port string handled as invalid");
        } else {
            result.fail("Empty port string handled as invalid");
        }
    }

    // Test case 14: Trailing garbage in port
    {
        std::string clean_host;
        int port = 13305;
        bool is_ssl = false;
        lemon::utils::parse_target_url("example.com:443junk", clean_host, port, is_ssl, true);
        if (port == -1) {
            result.ok("Trailing garbage in port handled as invalid");
        } else {
            result.fail("Trailing garbage in port handled as invalid");
        }
    }

    // Test case 15: Port out of range (> 65535)
    {
        std::string clean_host;
        int port = 13305;
        bool is_ssl = false;
        lemon::utils::parse_target_url("localhost:65536", clean_host, port, is_ssl, true);
        if (port == -1) {
            result.ok("Out-of-range port handled as invalid");
        } else {
            result.fail("Out-of-range port handled as invalid");
        }
    }

    return result.failed > 0 ? 1 : 0;
}
