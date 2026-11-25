// A loopback server answers 403 a few times before serving the file, the way
// some release CDNs reject a valid object transiently. download_file must stop
// at the first 403 by default and retry it only with retry_forbidden set.

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#include <httplib.h>

#include <lemon/utils/http_client.h>

using lemon::utils::DownloadOptions;
using lemon::utils::HttpClient;
using lemon::utils::HttpSecurityPolicy;

namespace fs = std::filesystem;

static int passed = 0;
static int failed = 0;

static void check(bool cond, const char* name) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", name);
    (cond ? passed : failed)++;
}

int main() {
    const std::string body = "bundle-bytes";
    std::atomic<int> requests{0};
    std::atomic<int> forbidden_left{0};

    httplib::Server server;
    server.Get("/bundle.tar.gz", [&](const httplib::Request&, httplib::Response& res) {
        ++requests;
        if (forbidden_left.load() > 0) {
            --forbidden_left;
            res.status = 403;
            res.set_content("Forbidden", "text/plain");
            return;
        }
        res.set_content(body, "application/octet-stream");
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    std::thread thread([&] { server.listen_after_bind(); });
    server.wait_until_ready();

    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/bundle.tar.gz";
    const fs::path dir = fs::temp_directory_path() / "lemonade-retry-forbidden-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string out = (dir / "bundle.tar.gz").string();

    DownloadOptions options;
    options.max_retries = 3;
    options.initial_retry_delay_ms = 10;
    options.max_retry_delay_ms = 20;

    requests = 0;
    forbidden_left = 2;
    auto plain = HttpClient::download_file(url, out, nullptr, {}, options,
                                           HttpSecurityPolicy::AllowInsecureHttp);
    check(!plain.success && plain.http_code == 403, "a 403 fails the download by default");
    check(requests.load() == 1, "a 403 is not retried by default");

    options.retry_forbidden = true;
    requests = 0;
    forbidden_left = 2;
    auto retried = HttpClient::download_file(url, out, nullptr, {}, options,
                                             HttpSecurityPolicy::AllowInsecureHttp);
    check(retried.success, "retry_forbidden succeeds once the 403s stop");
    check(requests.load() == 3, "retry_forbidden retried both 403s");
    std::string got;
    if (FILE* f = std::fopen(out.c_str(), "rb")) {
        char buf[64];
        size_t n = std::fread(buf, 1, sizeof(buf), f);
        got.assign(buf, n);
        std::fclose(f);
    }
    check(got == body, "the retried download wrote the served bytes");

    requests = 0;
    forbidden_left = 100;
    auto exhausted = HttpClient::download_file(url, out + ".2", nullptr, {}, options,
                                               HttpSecurityPolicy::AllowInsecureHttp);
    check(!exhausted.success, "a persistent 403 still fails with retry_forbidden");
    check(requests.load() == options.max_retries + 1, "a persistent 403 stops after max_retries");

    server.stop();
    thread.join();
    fs::remove_all(dir);

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
