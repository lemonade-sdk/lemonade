#include "lemon/utils/network_utils.h"

#include <cassert>
#include <cstdio>
#include <string>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

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

static int get_free_port(int family) {
#ifdef _WIN32
    SOCKET s = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        return -1;
    }
#else
    int s = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        return -1;
    }
#endif

    sockaddr_storage ss{};
    socklen_t len;
    if (family == AF_INET) {
        auto* a = reinterpret_cast<sockaddr_in*>(&ss);
        a->sin_family = AF_INET;
        a->sin_addr.s_addr = inet_addr("127.0.0.1");
        a->sin_port = 0;
        len = sizeof(sockaddr_in);
    } else {
        auto* a = reinterpret_cast<sockaddr_in6*>(&ss);
        a->sin6_family = AF_INET6;
        if (inet_pton(AF_INET6, "::1", &a->sin6_addr) != 1) {
#ifdef _WIN32
            closesocket(s);
#else
            close(s);
#endif
            return -1;
        }
        a->sin6_port = 0;
        len = sizeof(sockaddr_in6);
    }

    if (bind(s, reinterpret_cast<sockaddr*>(&ss), len) != 0) {
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return -1;
    }

    sockaddr_storage bound_ss{};
    socklen_t bound_len = sizeof(bound_ss);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&bound_ss), &bound_len) != 0) {
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return -1;
    }

    int port = 0;
    if (family == AF_INET) {
        port = ntohs(reinterpret_cast<sockaddr_in*>(&bound_ss)->sin_port);
    } else {
        port = ntohs(reinterpret_cast<sockaddr_in6*>(&bound_ss)->sin6_port);
    }

#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif

    return port;
}

static void test_inactive_port(TestResult& r) {
    int port_v4 = get_free_port(AF_INET);
    if (port_v4 > 0) {
        bool active = lemon::utils::is_tcp_listener_active(AF_INET, "127.0.0.1", port_v4);
        if (!active) {
            r.ok("inactive port probe (IPv4)");
        } else {
            r.fail("inactive port probe (IPv4): reported active on free port");
        }
    } else {
        r.fail("inactive port probe (IPv4): failed to get free port");
    }

    int port_v6 = get_free_port(AF_INET6);
    if (port_v6 > 0) {
        bool active_v6 = lemon::utils::is_tcp_listener_active(AF_INET6, "::1", port_v6);
        if (!active_v6) {
            r.ok("inactive port probe (IPv6)");
        } else {
            r.fail("inactive port probe (IPv6): reported active on free port");
        }
    } else {
        r.ok("inactive port probe (IPv6) - skipped (IPv6 loopback not supported/bind failed)");
    }
}

static void test_active_port(TestResult& r) {
#ifdef _WIN32
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(listener != INVALID_SOCKET);
#else
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(listener >= 0);
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0;

    int bind_res = bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    assert(bind_res == 0);
    int listen_res = listen(listener, 1);
    assert(listen_res == 0);

    sockaddr_in bound_addr{};
    socklen_t addr_len = sizeof(bound_addr);
    int name_res = getsockname(listener, reinterpret_cast<sockaddr*>(&bound_addr), &addr_len);
    assert(name_res == 0);
    int port = ntohs(bound_addr.sin_port);

    bool active = lemon::utils::is_tcp_listener_active(AF_INET, "127.0.0.1", port);
    if (active) {
        r.ok("active port probe (IPv4)");
    } else {
        r.fail("active port probe (IPv4): reported inactive when listener is running");
    }

#ifdef _WIN32
    closesocket(listener);
#else
    close(listener);
#endif

    bool inactive_again = false;
    for (int i = 0; i < 5; ++i) {
        if (!lemon::utils::is_tcp_listener_active(AF_INET, "127.0.0.1", port)) {
            inactive_again = true;
            break;
        }
#ifdef _WIN32
        Sleep(50);
#else
        usleep(50000);
#endif
    }

    if (inactive_again) {
        r.ok("port probe after closing listener (IPv4)");
    } else {
        r.fail("port probe after closing listener (IPv4): still reported active");
    }
}

int main() {
#ifdef _WIN32
    WSADATA wsaData;
    int wsa_res = WSAStartup(MAKEWORD(2, 2), &wsaData);
    assert(wsa_res == 0);
#endif

    TestResult r;
    printf("=== NetworkUtils Unit Tests ===\n\n");

    test_inactive_port(r);
    test_active_port(r);

    printf("\n%d/%d tests passed\n", r.passed, r.passed + r.failed);

#ifdef _WIN32
    WSACleanup();
#endif

    return r.failed == 0 ? 0 : 1;
}
