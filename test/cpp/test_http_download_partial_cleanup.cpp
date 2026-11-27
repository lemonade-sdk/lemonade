// A loopback server cuts every transfer short. download_file must keep the
// partial file for a later resume by default, and delete it when the caller
// downloads with resume_partial=false (it could never be reused).

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
    httplib::Server server;
    server.Get("/bundle.tar.gz", [](const httplib::Request&, httplib::Response& res) {
        res.set_content_provider(
            100000, "application/octet-stream",
            [](size_t offset, size_t, httplib::DataSink& sink) {
                if (offset == 0) {
                    const std::string chunk(1000, 'x');
                    sink.write(chunk.data(), chunk.size());
                    return true;
                }
                return false;  // drop the connection after the first chunk
            });
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    std::thread thread([&] { server.listen_after_bind(); });
    server.wait_until_ready();

    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/bundle.tar.gz";
    const fs::path dir = fs::temp_directory_path() / "lemonade-partial-cleanup-test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    DownloadOptions options;
    options.max_retries = 1;
    options.initial_retry_delay_ms = 10;
    options.max_retry_delay_ms = 20;

    const std::string kept = (dir / "kept.tar.gz").string();
    auto resumable = HttpClient::download_file(url, kept, nullptr, {}, options,
                                               HttpSecurityPolicy::AllowInsecureHttp);
    check(!resumable.success, "a cut-short download fails");
    check(fs::exists(kept + ".partial"), "the partial is kept when it can be resumed");

    options.resume_partial = false;
    const std::string discarded = (dir / "discarded.tar.gz").string();
    auto fresh = HttpClient::download_file(url, discarded, nullptr, {}, options,
                                           HttpSecurityPolicy::AllowInsecureHttp);
    check(!fresh.success, "a cut-short download without resume fails");
    check(!fs::exists(discarded + ".partial"), "the partial is deleted when it cannot be resumed");
    check(fresh.error_message.find("Partial file preserved") == std::string::npos,
          "the error does not offer a resume that cannot happen");

    server.stop();
    thread.join();
    fs::remove_all(dir);

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
