// Pressure tests for HttpClient's parallel ranged download path.
//
// Every case compares the produced file byte-for-byte against the served body:
// length-correct but content-wrong is the failure mode a size assertion waves
// through.
//
// Checks use an explicit pass/fail counter (not assert()) so the test stays
// effective under the Release CI build, where -DNDEBUG no-ops assert().

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
#define CLOSESOCK closesocket
static const sock_t kInvalidSock = INVALID_SOCKET;
// Winsock needs process-wide init before any socket call; this test no longer
// includes httplib, which is what used to do it.
struct WsaInit {
    WsaInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~WsaInit() { WSACleanup(); }
};
static WsaInit g_wsa_init;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
#define CLOSESOCK close
static const sock_t kInvalidSock = -1;
#endif

#include <lemon/utils/http_client.h>

namespace fs = std::filesystem;
using lemon::utils::DownloadOptions;
using lemon::utils::DownloadResult;
using lemon::utils::HttpClient;
using lemon::utils::HttpSecurityPolicy;

struct TestResult {
    int passed = 0;
    int failed = 0;

    void check(bool cond, const std::string& name) {
        if (cond) {
            printf("[PASS] %s\n", name.c_str());
            ++passed;
        } else {
            printf("[FAIL] %s\n", name.c_str());
            ++failed;
        }
    }
};

namespace {

// Deterministic high-entropy body. A repeating or all-zero pattern would let a
// part that wrote at the wrong offset still compare equal.
std::string make_body(size_t size, uint32_t seed) {
    std::mt19937 rng(seed);
    std::string out(size, '\0');
    for (size_t i = 0; i < size; ++i) {
        out[i] = static_cast<char>(rng() & 0xFF);
    }
    return out;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Little-endian, matching the writer: magic, total, part count, then one
// durable offset per part.
std::string make_journal(uint64_t total, const std::vector<uint64_t>& offsets) {
    std::string out("LEMPART1", 8);
    auto put = [&out](uint64_t v) {
        for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    };
    put(total);
    put(offsets.size());
    for (uint64_t o : offsets) put(o);
    return out;
}

struct Faults {
    std::atomic<bool> ignore_ranges{false};
    std::atomic<int> fail_every_n{0};      // 503 on every Nth ranged request
    std::atomic<int> truncate_every_n{0};  // drop the connection mid-body
    std::atomic<bool> lie_content_range{false};  // 206 labelled with the wrong offset
    std::atomic<int> requests{0};
    std::atomic<long long> bytes_served{0};
};

// A deliberately hand-rolled origin. httplib cannot stand in here: it owns
// Range parsing and re-slices whatever a handler returns, so it can neither be
// made to emit the hostile responses these tests need (a 200 carrying the whole
// body in reply to a ranged request, a body shorter than its own
// Content-Length) nor be prevented from double-applying the range.
//
// Every response sets Connection: close, so each request is its own socket and
// a mid-body close is an unambiguous transport failure.
class RangeServer {
public:
    RangeServer(const std::string& body, Faults& faults) : body_(body), faults_(faults) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int yes = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&yes), sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // ephemeral: never collides with a running lemond
        ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(listen_fd_, 128);

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len);
        port_ = ::ntohs(bound.sin_port);

        accept_thread_ = std::thread([this]() { accept_loop(); });
    }

    ~RangeServer() {
        running_ = false;
        // Unblock accept() by connecting to ourselves; portable and avoids
        // depending on socket shutdown semantics that differ across platforms.
        sock_t waker = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = ::htons(static_cast<uint16_t>(port_));
        ::connect(waker, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        CLOSESOCK(waker);

        if (accept_thread_.joinable()) accept_thread_.join();
        CLOSESOCK(listen_fd_);

        std::lock_guard<std::mutex> lock(workers_mutex_);
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/file"; }

private:
    void accept_loop() {
        while (running_) {
            sock_t fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd == kInvalidSock) continue;
            if (!running_) {
                CLOSESOCK(fd);
                break;
            }
            std::lock_guard<std::mutex> lock(workers_mutex_);
            workers_.emplace_back([this, fd]() {
                serve(fd);
                CLOSESOCK(fd);
            });
        }
    }

    static void send_all(sock_t fd, const char* data, size_t len) {
        size_t sent = 0;
        while (sent < len) {
            const auto n = ::send(fd, data + sent, static_cast<int>(len - sent), 0);
            if (n <= 0) return;
            sent += static_cast<size_t>(n);
        }
    }

    void serve(sock_t fd) {
        std::string request;
        char buf[4096];
        while (request.find("\r\n\r\n") == std::string::npos) {
            const auto n = ::recv(fd, buf, static_cast<int>(sizeof(buf)), 0);
            if (n <= 0) return;
            request.append(buf, static_cast<size_t>(n));
            if (request.size() > 64 * 1024) return;
        }

        const int n = ++faults_.requests;

        size_t start = 0, end = 0;
        const bool has_range = parse_range_header(request, &start, &end);

        if (!has_range || faults_.ignore_ranges.load()) {
            // An origin that does not honour Range answers with the whole body.
            // Writing that at a part's offset is the corruption this guards.
            std::string head = "HTTP/1.1 200 OK\r\nContent-Length: " +
                               std::to_string(body_.size()) +
                               "\r\nConnection: close\r\n\r\n";
            send_all(fd, head.data(), head.size());
            send_all(fd, body_.data(), body_.size());
            faults_.bytes_served += static_cast<long long>(body_.size());
            return;
        }

        if (start > end || end >= body_.size()) {
            std::string head = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\n"
                               "Connection: close\r\n\r\n";
            send_all(fd, head.data(), head.size());
            return;
        }

        const int fail_n = faults_.fail_every_n.load();
        if (fail_n > 0 && (n % fail_n) == 0) {
            std::string head = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
                               "Connection: close\r\n\r\n";
            send_all(fd, head.data(), head.size());
            return;
        }

        const size_t len = end - start + 1;
        const size_t labelled_start = faults_.lie_content_range.load() ? start + 7 : start;
        std::string head = "HTTP/1.1 206 Partial Content\r\nContent-Length: " +
                           std::to_string(len) + "\r\nContent-Range: bytes " +
                           std::to_string(labelled_start) + "-" + std::to_string(end) + "/" +
                           std::to_string(body_.size()) + "\r\nConnection: close\r\n\r\n";
        send_all(fd, head.data(), head.size());

        const int trunc_n = faults_.truncate_every_n.load();
        // Promise len bytes, deliver half, then close: a real mid-body drop
        // rather than an honest short Content-Length.
        const size_t to_send = (trunc_n > 0 && (n % trunc_n) == 0) ? len / 2 : len;
        send_all(fd, body_.data() + start, to_send);
        faults_.bytes_served += static_cast<long long>(to_send);
    }

    static bool parse_range_header(const std::string& request, size_t* start, size_t* end) {
        // Header names are case-insensitive; curl sends "Range".
        std::string lowered;
        lowered.reserve(request.size());
        for (char c : request) lowered.push_back(static_cast<char>(::tolower(c)));

        const auto pos = lowered.find("\r\nrange:");
        if (pos == std::string::npos) return false;
        const auto eol = lowered.find("\r\n", pos + 2);
        std::string value = lowered.substr(pos + 8, eol - (pos + 8));

        const auto eq = value.find("bytes=");
        if (eq == std::string::npos) return false;
        value = value.substr(eq + 6);
        const auto dash = value.find('-');
        if (dash == std::string::npos) return false;
        try {
            *start = static_cast<size_t>(std::stoull(value.substr(0, dash)));
            const std::string tail = value.substr(dash + 1);
            if (tail.empty()) return false;
            *end = static_cast<size_t>(std::stoull(tail));
        } catch (...) {
            return false;
        }
        return true;
    }

    const std::string& body_;
    Faults& faults_;
    sock_t listen_fd_ = kInvalidSock;
    int port_ = 0;
    std::atomic<bool> running_{true};
    std::thread accept_thread_;
    std::mutex workers_mutex_;
    std::vector<std::thread> workers_;
};

DownloadOptions test_options(int parts, size_t total) {
    DownloadOptions o;
    o.parallel_parts = parts;
    o.parallel_min_bytes_per_part = 1024;
    o.expected_total_bytes = total;
    o.max_retries = 10;
    o.initial_retry_delay_ms = 20;
    o.max_retry_delay_ms = 200;
    o.no_progress_timeout = 20;
    o.connect_timeout = 10;
    return o;
}

struct Scratch {
    fs::path dir;
    explicit Scratch(const std::string& name) {
        static std::atomic<int> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        dir = fs::temp_directory_path() /
              ("lemon_par_test_" + name + "_" + std::to_string(stamp) + "_" +
               std::to_string(counter.fetch_add(1)));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path out() const { return dir / "model.bin"; }
};

}  // namespace

int main() {
    TestResult r;
    printf("=== HttpClient parallel download pressure tests ===\n\n");

    // ---- 1. Correctness across part counts and awkward sizes ----------------
    {
        // Prime and power-of-two sizes, including sizes that do not divide by
        // the part count, so a lost remainder or an off-by-one in the tiling
        // shows up as a content mismatch.
        // One size that divides evenly and one prime, against a low and a high
        // part count: a lost remainder or an off-by-one in the tiling shows up
        // as a content mismatch.
        const std::vector<size_t> sizes = {1024 * 1024, 999983};
        const std::vector<int> part_counts = {3, 16};
        bool all_ok = true;
        for (size_t size : sizes) {
            const std::string body = make_body(size, static_cast<uint32_t>(size));
            Faults faults;
            RangeServer server(body, faults);
            for (int parts : part_counts) {
                Scratch s("basic");
                auto opts = test_options(parts, size);
                auto res = HttpClient::download_file(server.url(), s.out().string(), nullptr, {},
                                                     opts, HttpSecurityPolicy::AllowInsecureHttp);
                const std::string got = read_file(s.out());
                if (!res.success || got != body) {
                    printf("       size=%zu parts=%d success=%d bytes=%zu/%zu match=%d\n",
                           size, parts, (int)res.success, got.size(), body.size(),
                           (int)(got == body));
                    all_ok = false;
                }
                if (res.parts_used < 2) {
                    printf("       size=%zu parts=%d did not engage parallel path\n", size, parts);
                    all_ok = false;
                }
            }
        }
        r.check(all_ok, "byte-exact tiling across even and prime sizes at 3 and 16 parts");
    }

    // ---- 2. Origin that ignores Range must fall back, not corrupt -----------
    {
        const size_t size = 1024 * 1024;
        const std::string body = make_body(size, 7);
        Faults faults;
        RangeServer server(body, faults);
        faults.ignore_ranges = true;

        Scratch s("norange");
        auto opts = test_options(8, size);
        auto res = HttpClient::download_file(server.url(), s.out().string(), nullptr, {}, opts,
                                             HttpSecurityPolicy::AllowInsecureHttp);
        const std::string got = read_file(s.out());
        r.check(res.success && got == body,
                "origin ignoring Range falls back to single-stream with correct bytes");
        r.check(!fs::exists(s.out().string() + ".partial.parts"),
                "no part journal left behind after fallback");
    }

    // ---- 3. Transient 503s on a fraction of ranged requests -----------------
    {
        const size_t size = 2 * 1024 * 1024;
        const std::string body = make_body(size, 11);
        Faults faults;
        RangeServer server(body, faults);
        faults.fail_every_n = 3;  // every third request is rejected

        Scratch s("flaky");
        auto opts = test_options(8, size);
        auto res = HttpClient::download_file(server.url(), s.out().string(), nullptr, {}, opts,
                                             HttpSecurityPolicy::AllowInsecureHttp);
        const std::string got = read_file(s.out());
        r.check(res.success && got == body, "recovers from 503 on every 3rd ranged request");
    }

    // ---- 4. Mid-body connection drops --------------------------------------
    {
        const size_t size = 2 * 1024 * 1024;
        const std::string body = make_body(size, 13);
        Faults faults;
        RangeServer server(body, faults);
        faults.truncate_every_n = 2;  // half the responses die mid-body

        Scratch s("truncate");
        auto opts = test_options(8, size);
        auto res = HttpClient::download_file(server.url(), s.out().string(), nullptr, {}, opts,
                                             HttpSecurityPolicy::AllowInsecureHttp);
        const std::string got = read_file(s.out());
        r.check(res.success && got == body,
                "recovers from mid-body connection drops and resumes inside the part");
    }

    // ---- 5. Cancellation, journal, and resume across runs -------------------
    {
        const size_t size = 4 * 1024 * 1024;
        const std::string body = make_body(size, 17);
        Faults faults;
        RangeServer server(body, faults);

        Scratch s("resume");
        auto opts = test_options(8, size);

        // Cancel once a quarter of the file has arrived.
        std::atomic<bool> cancelled_once{false};
        auto cancelling = [&](size_t done, size_t total) -> bool {
            (void)total;
            if (done > size / 4) {
                cancelled_once = true;
                return false;
            }
            return true;
        };
        auto first = HttpClient::download_file(server.url(), s.out().string(), cancelling, {}, opts,
                                               HttpSecurityPolicy::AllowInsecureHttp);
        r.check(first.cancelled && cancelled_once, "callback returning false cancels the transfer");
        r.check(fs::exists(s.out().string() + ".partial.parts"),
                "cancelled transfer leaves a part journal for resume");
        r.check(!fs::exists(s.out()), "cancelled transfer does not publish the final file");

        const long long served_after_cancel = faults.bytes_served.load();

        auto second = HttpClient::download_file(server.url(), s.out().string(), nullptr, {}, opts,
                                                HttpSecurityPolicy::AllowInsecureHttp);
        const std::string got = read_file(s.out());
        r.check(second.success && got == body, "resumed transfer produces byte-exact file");
        r.check(!fs::exists(s.out().string() + ".partial.parts"),
                "journal removed once the transfer completes");

        const long long served_in_resume = faults.bytes_served.load() - served_after_cancel;
        r.check(served_in_resume < static_cast<long long>(size),
                "resume re-fetches less than the whole file");
    }

    // ---- 6. Journals that do not describe this transfer are rejected --------
    {
        const size_t size = 1024 * 1024;
        const std::string body = make_body(size, 19);
        Faults faults;
        RangeServer server(body, faults);
        auto opts = test_options(8, size);

        // (a) Malformed: too short to even carry a header.
        {
            Scratch s("journalShort");
            const std::string partial = s.out().string() + ".partial";
            { std::ofstream(partial, std::ios::binary) << std::string(size, 'x'); }
            { std::ofstream(partial + ".parts", std::ios::binary) << "LEMPART1garbage"; }
            auto res = HttpClient::download_file(server.url(), s.out().string(), nullptr, {}, opts,
                                                 HttpSecurityPolicy::AllowInsecureHttp);
            r.check(res.success && read_file(s.out()) == body,
                    "malformed journal is discarded and the file restarts cleanly");
        }

        // (b) Well-formed, but describing a differently sized transfer, and
        // claiming every part is complete. Trusting it would publish the
        // placeholder bytes without transferring anything.
        {
            Scratch s("journalWrongTotal");
            const std::string partial = s.out().string() + ".partial";
            { std::ofstream(partial, std::ios::binary) << std::string(size, 'x'); }
            std::vector<uint64_t> full(8, size / 8);
            { std::ofstream(partial + ".parts", std::ios::binary) << make_journal(size + 1, full); }
            auto res = HttpClient::download_file(server.url(), s.out().string(), nullptr, {}, opts,
                                                 HttpSecurityPolicy::AllowInsecureHttp);
            r.check(res.success && read_file(s.out()) == body,
                    "journal for a differently sized transfer is rejected, not resumed into");
        }
    }

    // ---- 7. A parallel partial must never be resumed by the single-stream path
    {
        const size_t size = 1024 * 1024;
        const std::string body = make_body(size, 23);
        Faults faults;
        RangeServer server(body, faults);

        Scratch s("crossPath");
        const std::string partial = s.out().string() + ".partial";
        // Pre-sized partial plus journal, exactly what an interrupted parallel
        // run leaves. The single-stream path would read its size as "already
        // downloaded" and append past real data.
        { std::ofstream(partial, std::ios::binary) << std::string(size, '\0'); }
        { std::ofstream(partial + ".parts", std::ios::binary) << "LEMPART1xxxxxxxx"; }

        DownloadOptions opts;  // parallel_parts defaults to 1 -> single-stream
        opts.max_retries = 3;
        auto res = HttpClient::download_file(server.url(), s.out().string(), nullptr, {}, opts,
                                             HttpSecurityPolicy::AllowInsecureHttp);
        r.check(res.success && read_file(s.out()) == body,
                "single-stream path discards a pre-sized parallel partial instead of resuming it");
    }

    // ---- 8. Progress callback: single-threaded, monotonic, terminates at total
    {
        const size_t size = 2 * 1024 * 1024;
        const std::string body = make_body(size, 29);
        Faults faults;
        RangeServer server(body, faults);

        Scratch s("progress");
        std::mutex m;
        std::set<std::thread::id> caller_threads;
        std::vector<size_t> samples;
        auto cb = [&](size_t done, size_t total) -> bool {
            std::lock_guard<std::mutex> lock(m);
            caller_threads.insert(std::this_thread::get_id());
            samples.push_back(done);
            (void)total;
            return true;
        };

        auto opts = test_options(8, size);
        auto res = HttpClient::download_file(server.url(), s.out().string(), cb, {}, opts,
                                             HttpSecurityPolicy::AllowInsecureHttp);

        bool monotonic = true;
        for (size_t i = 1; i < samples.size(); ++i) {
            if (samples[i] < samples[i - 1]) monotonic = false;
        }
        r.check(res.success, "progress run succeeded");
        r.check(caller_threads.size() == 1,
                "progress callback is invoked from exactly one thread");
        r.check(monotonic, "aggregated progress never goes backwards");
        r.check(!samples.empty() && samples.back() == size,
                "final progress callback reports the full size");
    }

    // ---- 9. Hash verification is enforced on the parallel result -----------
    {
        const size_t size = 1024 * 1024;
        const std::string body = make_body(size, 31);
        Faults faults;
        RangeServer server(body, faults);

        Scratch s("badhash");
        auto opts = test_options(8, size);
        opts.expected_hash_algorithm = "sha256";
        opts.expected_hash = std::string(64, 'a');  // deliberately wrong
        opts.max_retries = 1;
        auto res = HttpClient::download_file(server.url(), s.out().string(), nullptr, {}, opts,
                                             HttpSecurityPolicy::AllowInsecureHttp);
        r.check(!res.success, "wrong expected hash fails the download");
        r.check(!fs::exists(s.out()), "failed verification does not publish the final file");
    }

    // ---- 10. A configured rate limit keeps the transfer single-stream -------
    {
        const size_t size = 1024 * 1024;
        const std::string body = make_body(size, 37);
        Faults faults;
        RangeServer server(body, faults);

        HttpClient::set_download_rate_limit(4 * 1024 * 1024);
        Scratch s("ratelimit");
        auto opts = test_options(8, size);
        auto res = HttpClient::download_file(server.url(), s.out().string(), nullptr, {}, opts,
                                             HttpSecurityPolicy::AllowInsecureHttp);
        HttpClient::set_download_rate_limit(0);

        r.check(res.success && read_file(s.out()) == body,
                "rate-limited download still succeeds");
        r.check(res.parts_used == 1,
                "rate limit forces single-stream so the cap is not multiplied per connection");
    }

    // ---- 12. An origin that mislabels Content-Range must not corrupt --------
    {
        const size_t size = 1024 * 1024;
        const std::string body = make_body(size, 41);
        Faults faults;
        RangeServer server(body, faults);
        faults.lie_content_range = true;

        Scratch s("liarRange");
        auto opts = test_options(8, size);
        opts.max_retries = 2;
        auto res = HttpClient::download_file(server.url(), s.out().string(), nullptr, {}, opts,
                                             HttpSecurityPolicy::AllowInsecureHttp);
        r.check(res.success && read_file(s.out()) == body,
                "Content-Range disagreeing with the request degrades to single-stream, bytes intact");
        r.check(res.parts_used == 1 || !fs::exists(s.out().string() + ".partial.parts"),
                "mislabelled range leaves no parallel artifacts behind");
    }

    // ---- 16. Sizes too small to split must degrade, not fail ---------------
    {
        bool all_ok = true;
        for (size_t size : {size_t{1}, size_t{2048}}) {
            const std::string body = make_body(size, static_cast<uint32_t>(size + 500));
            Faults faults;
            RangeServer server(body, faults);
            Scratch s("tiny");
            auto opts = test_options(8, size);
            auto res = HttpClient::download_file(server.url(), s.out().string(), nullptr, {}, opts,
                                                 HttpSecurityPolicy::AllowInsecureHttp);
            if (!res.success || read_file(s.out()) != body) {
                printf("       tiny size=%zu success=%d\n", size, (int)res.success);
                all_ok = false;
            }
        }
        r.check(all_ok, "sizes below the per-part floor fall back and still transfer exactly");
    }

    printf("\n=== %d passed, %d failed ===\n", r.passed, r.failed);
    return r.failed == 0 ? 0 : 1;
}
