// Real-network check: exercises the parallel path against Hugging Face itself
// (TLS, redirect to the CDN, the xet bridge's real 206 behavior) and verifies
// the result against the LFS sha256 the Hub publishes. Not a CI test: it needs
// the network.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <lemon/utils/http_client.h>

using lemon::utils::DownloadOptions;
using lemon::utils::HttpClient;
using lemon::utils::HttpSecurityPolicy;
namespace fs = std::filesystem;

struct Target {
    std::string url;
    size_t size;
    std::string sha256;
};

// Defaults to a small file so the correctness check stays cheap. A larger
// target can be passed on the command line when measuring throughput, where
// per-part slices need to be big enough for parallelism to pay for its
// connection setup.
Target g_target{
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin",
    147964211,
    "a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002"};

int run(const char* label, int parts, const std::string& out) {
    const std::string url = g_target.url;
    DownloadOptions o;
    o.parallel_parts = parts;
    o.expected_total_bytes = g_target.size;
    o.expected_hash_algorithm = "sha256";
    o.expected_hash = g_target.sha256;
    o.max_retries = 5;
    o.no_progress_timeout = 60;

    std::error_code ec;
    fs::remove(out, ec);
    fs::remove(out + ".partial", ec);
    fs::remove(out + ".partial.parts", ec);

    const auto t0 = std::chrono::steady_clock::now();
    auto res = HttpClient::download_file(url, out, nullptr, {}, o,
                                         HttpSecurityPolicy::ExternalHttpsOnly);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const auto size = fs::exists(out) ? fs::file_size(out) : 0;
    printf("%-18s parts=%2d success=%d parts_used=%2d size=%llu  %.1fs  %.1f MB/s  hash=%s\n",
           label, parts, (int)res.success, res.parts_used, (unsigned long long)size, secs,
           (size / 1e6) / secs, res.success ? "VERIFIED" : "n/a");
    if (!res.success) printf("    error: %s\n", res.error_message.c_str());
    fs::remove(out, ec);
    return res.success ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc == 4) {
        g_target = {argv[1], static_cast<size_t>(std::stoull(argv[2])), argv[3]};
        printf("target: %s (%.0f MB)\n", g_target.url.c_str(), g_target.size / 1e6);
    }
    int bad = 0;
    bad += run("single-stream", 1, (fs::temp_directory_path() / "lemon_e2e_1.bin").string());
    bad += run("parallel x16", 16, (fs::temp_directory_path() / "lemon_e2e_16.bin").string());
    printf("\n%s\n", bad == 0 ? "both paths verified against the Hub's sha256" : "FAILURE");
    return bad;
}
