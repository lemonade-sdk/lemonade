// Windows header discipline — must precede all other includes.
#ifdef _WIN32
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#ifdef ERROR
#undef ERROR
#endif
#endif

#include "lemon/utils/network_utils.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <thread>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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

static void test_listener_startup_state(TestResult& r) {
    {
        lemon::utils::ListenerStartupState state(1);
        if (!state.bind_attempts_complete() && !state.failed()) {
            r.ok("single listener starts pending");
        } else {
            r.fail("single listener starts pending");
        }
        state.record_bind_success();
        if (state.bind_attempts_complete() && !state.failed()) {
            r.ok("single listener bind succeeds");
        } else {
            r.fail("single listener bind succeeds");
        }
    }

    {
        lemon::utils::ListenerStartupState state(1);
        state.record_bind_failure();
        if (state.bind_attempts_complete() && state.failed()) {
            r.ok("single listener bind fails");
        } else {
            r.fail("single listener bind fails");
        }
    }

    {
        lemon::utils::ListenerStartupState state(2);
        state.record_bind_success();
        state.record_bind_failure();
        if (state.bind_attempts_complete() && state.failed()) {
            r.ok("dual listener success then failure is fatal");
        } else {
            r.fail("dual listener success then failure is fatal");
        }
    }

    {
        lemon::utils::ListenerStartupState state(2);
        state.record_bind_failure();
        state.record_bind_success();
        if (state.bind_attempts_complete() && state.failed()) {
            r.ok("dual listener failure then success is fatal");
        } else {
            r.fail("dual listener failure then success is fatal");
        }
    }

    {
        lemon::utils::ListenerStartupState state(2);
        std::thread first([&state]() { state.record_bind_success(); });
        std::thread second([&state]() { state.record_bind_failure(); });
        first.join();
        second.join();
        if (state.bind_attempts_complete() && state.failed()) {
            r.ok("concurrent listener results are recorded");
        } else {
            r.fail("concurrent listener results are recorded");
        }
    }

    {
        lemon::utils::ListenerStartupState state(2);
        state.record_bind_success();
        state.record_bind_success();
        state.record_listen_failure();
        if (state.bind_attempts_complete() && state.failed()) {
            r.ok("listen failure after successful binds is fatal");
        } else {
            r.fail("listen failure after successful binds is fatal");
        }
    }
}

static void test_configure_tcp_keepalive(TestResult& r) {
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

#ifdef _WIN32
    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(client != INVALID_SOCKET);
#else
    int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(client >= 0);
#endif

    int conn_res = connect(client, reinterpret_cast<sockaddr*>(&bound_addr), sizeof(bound_addr));
    assert(conn_res == 0);

#ifdef _WIN32
    SOCKET accepted = accept(listener, nullptr, nullptr);
    if (accepted == INVALID_SOCKET) {
        r.fail("configure_tcp_keepalive: accept socket");
        closesocket(client);
        closesocket(listener);
        return;
    }
#else
    int accepted = accept(listener, nullptr, nullptr);
    if (accepted < 0) {
        r.fail("configure_tcp_keepalive: accept socket");
        close(client);
        close(listener);
        return;
    }
#endif

    bool config_res = lemon::utils::configure_tcp_keepalive(accepted);
    if (config_res) {
        r.ok("configure_tcp_keepalive returns true on accepted socket");
    } else {
        r.fail("configure_tcp_keepalive returned false on accepted socket");
    }

    int opt = 0;
    socklen_t optlen = sizeof(opt);
#ifdef _WIN32
    int res = getsockopt(accepted, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<char*>(&opt), &optlen);
#else
    int res = getsockopt(accepted, SOL_SOCKET, SO_KEEPALIVE, &opt, &optlen);
#endif

    if (res == 0 && opt != 0) {
        r.ok("SO_KEEPALIVE enabled on accepted socket");
    } else {
        r.fail("SO_KEEPALIVE not enabled on accepted socket");
    }

#ifdef __linux__
    int idle = 0;
    optlen = sizeof(idle);
    if (getsockopt(accepted, IPPROTO_TCP, TCP_KEEPIDLE, &idle, &optlen) == 0 && idle == 15) {
        r.ok("TCP_KEEPIDLE == 15 on accepted socket");
    } else {
        r.fail("TCP_KEEPIDLE check failed");
    }

    int interval = 0;
    optlen = sizeof(interval);
    if (getsockopt(accepted, IPPROTO_TCP, TCP_KEEPINTVL, &interval, &optlen) == 0 && interval == 5) {
        r.ok("TCP_KEEPINTVL == 5 on accepted socket");
    } else {
        r.fail("TCP_KEEPINTVL check failed");
    }

    int count = 0;
    optlen = sizeof(count);
    if (getsockopt(accepted, IPPROTO_TCP, TCP_KEEPCNT, &count, &optlen) == 0 && count == 3) {
        r.ok("TCP_KEEPCNT == 3 on accepted socket");
    } else {
        r.fail("TCP_KEEPCNT check failed");
    }
#elif defined(__APPLE__)
    int idle = 0;
    optlen = sizeof(idle);
    if (getsockopt(accepted, IPPROTO_TCP, TCP_KEEPALIVE, &idle, &optlen) == 0 && idle == 15) {
        r.ok("TCP_KEEPALIVE == 15 on accepted socket");
    } else {
        r.fail("TCP_KEEPALIVE check failed");
    }

    int interval = 0;
    optlen = sizeof(interval);
    if (getsockopt(accepted, IPPROTO_TCP, TCP_KEEPINTVL, &interval, &optlen) == 0 && interval == 5) {
        r.ok("TCP_KEEPINTVL == 5 on accepted socket");
    } else {
        r.fail("TCP_KEEPINTVL check failed");
    }

    int count = 0;
    optlen = sizeof(count);
    if (getsockopt(accepted, IPPROTO_TCP, TCP_KEEPCNT, &count, &optlen) == 0 && count == 3) {
        r.ok("TCP_KEEPCNT == 3 on accepted socket");
    } else {
        r.fail("TCP_KEEPCNT check failed");
    }
#elif defined(_WIN32)
#ifdef TCP_KEEPIDLE
    int idle = 0;
    optlen = sizeof(idle);
    if (getsockopt(accepted, IPPROTO_TCP, TCP_KEEPIDLE, reinterpret_cast<char*>(&idle), &optlen) == 0 && idle == 15) {
        r.ok("TCP_KEEPIDLE == 15 on accepted socket");
    } else {
        r.fail("TCP_KEEPIDLE check failed");
    }
#endif
#ifdef TCP_KEEPINTVL
    int interval = 0;
    optlen = sizeof(interval);
    if (getsockopt(accepted, IPPROTO_TCP, TCP_KEEPINTVL, reinterpret_cast<char*>(&interval), &optlen) == 0 && interval == 5) {
        r.ok("TCP_KEEPINTVL == 5 on accepted socket");
    } else {
        r.fail("TCP_KEEPINTVL check failed");
    }
#endif
#ifdef TCP_KEEPCNT
    int count = 0;
    optlen = sizeof(count);
    if (getsockopt(accepted, IPPROTO_TCP, TCP_KEEPCNT, reinterpret_cast<char*>(&count), &optlen) == 0 && count == 3) {
        r.ok("TCP_KEEPCNT == 3 on accepted socket");
    } else {
        r.fail("TCP_KEEPCNT check failed");
    }
#endif
#endif

#ifdef _WIN32
    closesocket(accepted);
    closesocket(client);
    closesocket(listener);
#else
    close(accepted);
    close(client);
    close(listener);
#endif
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
    test_listener_startup_state(r);
    test_configure_tcp_keepalive(r);

    printf("\n%d/%d tests passed\n", r.passed, r.passed + r.failed);

#ifdef _WIN32
    WSACleanup();
#endif

    return r.failed == 0 ? 0 : 1;
}
