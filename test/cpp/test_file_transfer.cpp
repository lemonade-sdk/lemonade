// The transfer gate the spec names: scheme dispatch across http and file, one
// mixed-scheme plan reporting one progress total, a file://-only plan that
// needs no network, and auth_headers() that belong to the origin that minted
// them — kept across a same-origin redirect, stripped at a cross-origin one,
// and never restored on a later hop. A loopback httplib server stands in for
// the registry; two ports are two origins. A resume accepts a body only from the
// offset it asked for.

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <lemon/models/artifact_transfer.h>
#include <lemon/utils/cancel_token.h>
#include <lemon/utils/path_utils.h>

namespace fs = std::filesystem;
using lemon::models::ArtifactTransfer;
using lemon::models::DownloadFile;
using lemon::models::DownloadPlan;
using lemon::models::FileTransfer;
using lemon::models::HttpFileTransfer;
using lemon::models::LocalCopyFileTransfer;
using lemon::utils::CancelToken;
using lemon::utils::HttpSecurityPolicy;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

// Names carrying a control character are unreadable in a check label.
static std::string display(const std::string& text) {
    std::string out;
    for (const char raw : text) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if (c < 0x20 || c == 0x7f) {
            out += "\\x";
            static const char* hex = "0123456789abcdef";
            out += hex[c >> 4];
            out += hex[c & 0x0f];
        } else {
            out += raw;
        }
    }
    return out;
}

// Windows needs a privilege to make a symlink; a run without one says so rather than
// passing a containment check it never ran.
static bool make_symlink(const fs::path& target, const fs::path& link, std::string& why) {
    std::error_code ec;
    fs::create_symlink(target, link, ec);
    if (!ec) {
        return true;
    }
    why = ec.message();
    return false;
}

// A host that answers a range request with bytes from elsewhere. httplib derives
// Content-Range from the request and turns a 206 it cannot slice into a 200, so a
// lying answer is written out here. An empty content_range omits the header.
class LyingCdn {
public:
    LyingCdn(std::string body, std::string content_range)
        : body_(std::move(body)), content_range_(std::move(content_range)) {
        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
        const char reuse = 1;
        ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(listen_, 8);
        socklen_t length = sizeof(addr);
        ::getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &length);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { answer_every_request(); });
    }

    ~LyingCdn() {
        stop_.store(true);
        stop_socket(listen_);
        if (thread_.joinable()) thread_.join();
    }

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/blob"; }
    int hits() const { return hits_.load(); }

private:
    static void stop_socket(::socket_t sock) {
#ifdef _WIN32
        ::shutdown(sock, SD_BOTH);
        ::closesocket(sock);
#else
        ::shutdown(sock, SHUT_RDWR);
        ::close(sock);
#endif
    }

    void answer_every_request() {
        while (!stop_.load()) {
            const ::socket_t fd = ::accept(listen_, nullptr, nullptr);
            if (fd == INVALID_SOCKET) return;
            char buffer[512];
            std::string request;
            while (request.find("\r\n\r\n") == std::string::npos) {
                const int got = ::recv(fd, buffer, sizeof(buffer), 0);
                if (got <= 0) break;
                request.append(buffer, static_cast<size_t>(got));
            }
            ++hits_;
            std::string response = "HTTP/1.1 206 Partial Content\r\n";
            if (!content_range_.empty()) {
                response += "Content-Range: " + content_range_ + "\r\n";
            }
            response += "Content-Length: " + std::to_string(body_.size()) +
                        "\r\nConnection: close\r\n\r\n";
            response += body_;
            std::size_t sent = 0;
            while (sent < response.size()) {
                const int written = ::send(fd, response.data() + sent,
                                           static_cast<int>(response.size() - sent), 0);
                if (written <= 0) break;
                sent += static_cast<std::size_t>(written);
            }
            stop_socket(fd);
        }
    }

    std::string body_;
    std::string content_range_;
    ::socket_t listen_ = INVALID_SOCKET;
    int port_ = 0;
    std::atomic<int> hits_{0};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

static fs::path make_temp_dir(const char* tag) {
    fs::path dir = fs::temp_directory_path();
    dir /= std::string("file_transfer_") + tag + "_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);
    return dir;
}

static void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

static std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string file_url(const fs::path& path) {
    return "file://" + lemon::utils::path_to_utf8(path);
}

// The shipped pair, with the HTTP transfer opted into plaintext for a
// loopback registry. Production keeps the HTTPS-only default.
static ArtifactTransfer loopback_transfer() {
    std::vector<std::unique_ptr<FileTransfer>> transfers;
    transfers.push_back(std::make_unique<HttpFileTransfer>(HttpSecurityPolicy::AllowInsecureHttp));
    transfers.push_back(std::make_unique<LocalCopyFileTransfer>());
    return ArtifactTransfer(std::move(transfers));
}

struct Recorded {
    std::string authorization = "<none-seen>";
    int hits = 0;
};

static Recorded g_plain;
static Recorded g_final_a;
static Recorded g_final_b;
static Recorded g_chain_end_b;

static void record(Recorded& slot, const httplib::Request& req) {
    slot.hits += 1;
    const auto header = req.headers.find("Authorization");
    slot.authorization = header != req.headers.end() ? header->second : std::string();
}

static const std::string kWholeBody = [] {
    std::string body;
    for (int i = 0; i < 1000; ++i) body += static_cast<char>('a' + (i % 26));
    return body;
}();
constexpr const char kWholeSha256[] =
    "915e53a44c18b19bb06ba5b3f5fcaf1dc4651e8404c63425cfc6174e74659d87";
constexpr std::size_t kPartialBytes = 400;

// What a host saw and sent for the first request it got.
struct RangeSeen {
    int hits = 0;
    std::string requested = "<none-seen>";
    std::size_t delivered = 0;
};

static RangeSeen g_cdn;
static RangeSeen g_cdn_whole;

static std::size_t record_range(const httplib::Request& req, RangeSeen& slot) {
    const auto range = req.headers.find("Range");
    const std::string asked = range == req.headers.end() ? std::string() : range->second;
    if (slot.hits == 0) slot.requested = asked;
    ++slot.hits;
    if (asked.rfind("bytes=", 0) != 0) return 0;
    const std::size_t dash = slot.requested.find('-', 6);
    if (dash == std::string::npos) return 0;
    return static_cast<std::size_t>(
        strtoull(slot.requested.substr(6, dash).c_str(), nullptr, 10));
}

int main() {
    std::printf("=== FileTransfer unit tests ===\n\n");

    httplib::Server a;
    httplib::Server b;
    const int port_a = a.bind_to_any_port("127.0.0.1");
    const int port_b = b.bind_to_any_port("127.0.0.1");
    if (port_a <= 0 || port_b <= 0) {
        std::printf("[FAIL] could not bind the two loopback origins\n");
        return 1;
    }
    const std::string base_a = "http://127.0.0.1:" + std::to_string(port_a);
    const std::string base_b = "http://127.0.0.1:" + std::to_string(port_b);

    a.Get("/plain", [](const httplib::Request& req, httplib::Response& res) {
        record(g_plain, req);
        res.set_content("plain bytes", "application/octet-stream");
    });
    a.Get("/same-origin-hop", [](const httplib::Request&, httplib::Response& res) {
        res.status = 302;
        res.set_header("Location", "/final-a");
    });
    a.Get("/final-a", [](const httplib::Request& req, httplib::Response& res) {
        record(g_final_a, req);
        res.set_content("final bytes on a", "application/octet-stream");
    });
    a.Get("/cross-hop", [base_b](const httplib::Request&, httplib::Response& res) {
        res.status = 302;
        res.set_header("Location", base_b + "/final-b");
    });
    a.Get("/chain-1", [base_b](const httplib::Request&, httplib::Response& res) {
        res.status = 302;
        res.set_header("Location", base_b + "/chain-2");
    });
    a.Get("/to-file", [](const httplib::Request&, httplib::Response& res) {
        res.status = 302;
        res.set_header("Location", "file:///etc/passwd");
    });
    // A registry that names a file host in its redirect, which is the shape a
    // repository resolve URL and its CDN make together.
    a.Get("/resolve-range", [base_b](const httplib::Request&, httplib::Response& res) {
        res.status = 302;
        res.set_header("Location", base_b + "/cdn-range");
    });
    a.Get("/resolve-range-ignored", [base_b](const httplib::Request&, httplib::Response& res) {
        res.status = 302;
        res.set_header("Location", base_b + "/cdn-range-ignored");
    });
    // Drips bytes so a token set during the first chunk can catch the middle.
    a.Get("/slow", [](const httplib::Request&, httplib::Response& res) {
        res.set_chunked_content_provider(
            "application/octet-stream",
            [](size_t offset, httplib::DataSink& sink) -> bool {
                (void)offset;
                static const std::string chunk(64 * 1024, 's');
                for (int i = 0; i < 32; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    if (!sink.write(chunk.data(), chunk.size())) {
                        return false;
                    }
                }
                sink.done();
                return false;
            });
    });
    b.Get("/final-b", [](const httplib::Request& req, httplib::Response& res) {
        record(g_final_b, req);
        res.set_content("final bytes on b", "application/octet-stream");
    });
    b.Get("/chain-2", [](const httplib::Request&, httplib::Response& res) {
        res.status = 302;
        res.set_header("Location", "/chain-end-b");
    });
    b.Get("/chain-end-b", [](const httplib::Request& req, httplib::Response& res) {
        record(g_chain_end_b, req);
        res.set_content("chain end on b", "application/octet-stream");
    });
    b.Get("/cdn-range", [](const httplib::Request& req, httplib::Response& res) {
        const std::size_t from = std::min(record_range(req, g_cdn), kWholeBody.size());
        if (g_cdn.hits == 1) g_cdn.delivered = kWholeBody.size() - from;
        // The server trims this body to the range it was asked for.
        res.status = 206;
        res.set_content(kWholeBody, "application/octet-stream");
    });
    b.Get("/cdn-range-ignored", [](const httplib::Request& req, httplib::Response& res) {
        record_range(req, g_cdn_whole);
        if (g_cdn_whole.hits == 1) g_cdn_whole.delivered = kWholeBody.size();
        // A whole file at 200 is how a host that reads no ranges answers.
        res.status = 200;
        res.set_content(kWholeBody, "application/octet-stream");
    });

    std::thread server_a([&a] { a.listen_after_bind(); });
    std::thread server_b([&b] { b.listen_after_bind(); });
    a.wait_until_ready();
    b.wait_until_ready();

    const std::map<std::string, std::string> credentials{{"Authorization", "Bearer minted-by-a"}};

    // --- credentials and hops --------------------------------------------
    {
        const fs::path dir = make_temp_dir("direct");
        DownloadPlan plan;
        plan.download_path = lemon::utils::path_to_utf8(dir);
        plan.files.push_back(DownloadFile{"m.bin", base_a + "/plain", 11, {}, {}, {}});
        loopback_transfer().execute(plan, credentials, nullptr, {});
        check("a direct fetch carries the credential to its origin",
              g_plain.authorization == "Bearer minted-by-a");
        check("the direct body lands", read_file(dir / "m.bin") == "plain bytes");
        fs::remove_all(dir);
    }
    {
        const fs::path dir = make_temp_dir("same_origin");
        DownloadPlan plan;
        plan.download_path = lemon::utils::path_to_utf8(dir);
        plan.files.push_back(DownloadFile{"m.bin", base_a + "/same-origin-hop", 0, {}, {}, {}});
        loopback_transfer().execute(plan, credentials, nullptr, {});
        check("a same-origin redirect still carries the credential",
              g_final_a.authorization == "Bearer minted-by-a");
        check("the redirected body lands",
              read_file(dir / "m.bin") == "final bytes on a");
        fs::remove_all(dir);
    }
    {
        const fs::path dir = make_temp_dir("cross_origin");
        DownloadPlan plan;
        plan.download_path = lemon::utils::path_to_utf8(dir);
        plan.files.push_back(DownloadFile{"m.bin", base_a + "/cross-hop", 0, {}, {}, {}});
        loopback_transfer().execute(plan, credentials, nullptr, {});
        check("a cross-origin redirect arrives without the credential",
              g_final_b.authorization.empty());
        check("the cross-origin body lands",
              read_file(dir / "m.bin") == "final bytes on b");
        fs::remove_all(dir);
    }
    {
        const fs::path dir = make_temp_dir("chain");
        DownloadPlan plan;
        plan.download_path = lemon::utils::path_to_utf8(dir);
        plan.files.push_back(DownloadFile{"m.bin", base_a + "/chain-1", 0, {}, {}, {}});
        loopback_transfer().execute(plan, credentials, nullptr, {});
        check("a same-origin hop after a strip does not restore the credential",
              g_chain_end_b.authorization.empty());
        check("the chained body lands", read_file(dir / "m.bin") == "chain end on b");
        fs::remove_all(dir);
    }
    {
        const fs::path dir = make_temp_dir("bad_scheme");
        DownloadPlan plan;
        plan.download_path = lemon::utils::path_to_utf8(dir);
        plan.files.push_back(DownloadFile{"m.bin", base_a + "/to-file", 0, {}, {}, {}});
        bool threw = false;
        std::string message;
        try {
            loopback_transfer().execute(plan, credentials, nullptr, {});
        } catch (const std::exception& e) {
            threw = true;
            message = e.what();
        }
        check("a redirect to a disallowed scheme fails", threw);
        check("the failure names the rejected scheme",
              message.find("disallowed scheme") != std::string::npos &&
                  message.find("file") != std::string::npos);
        check("nothing lands for a rejected hop", !fs::exists(dir / "m.bin"));
        fs::remove_all(dir);
    }

    // --- one plan, one total, two schemes ---------------------------------
    {
        const fs::path dir = make_temp_dir("mixed");
        const std::string local_content(1024, 'l');
        write_file(dir / "source.bin", local_content);

        DownloadPlan plan;
        plan.download_path = lemon::utils::path_to_utf8(dir / "cache");
        plan.files.push_back(DownloadFile{"http.bin", base_a + "/plain", 11, {}, {}, {}});
        plan.files.push_back(
            DownloadFile{"local.bin", file_url(dir / "source.bin"), local_content.size(), {}, {},
                         {}});

        std::size_t one_total = 0;
        bool totals_agree = true;
        std::map<int, bool> announced;
        std::map<int, int> last_percent;
        loopback_transfer().execute(
            plan, {},
            [&](const lemon::DownloadProgress& update) {
                if (one_total == 0) one_total = update.total_download_size;
                if (update.total_download_size != one_total) totals_agree = false;
                if (update.percent == 0) announced[update.file_index] = true;
                last_percent[update.file_index] = update.percent;
                return true;
            },
            {});

        check("a mixed plan reports one total across all files",
              totals_agree && one_total == 11 + local_content.size());
        check("both files are announced and numbered", announced[1] && announced[2]);
        check("both files reach 100 percent",
              last_percent[1] == 100 && last_percent[2] == 100);
        check("the http file of the mixed plan lands",
              read_file(dir / "cache" / "http.bin") == "plain bytes");
        check("the file:// file of the mixed plan lands",
              read_file(dir / "cache" / "local.bin") == local_content);
        fs::remove_all(dir);
    }

    // --- a file:// plan needs no network -----------------------------------
    {
        const fs::path dir = make_temp_dir("file_only");
        const std::string content = "no socket was opened for this plan";
        write_file(dir / "source.bin", content);

        DownloadPlan plan;
        plan.download_path = lemon::utils::path_to_utf8(dir / "cache");
        plan.files.push_back(DownloadFile{"model.bin", file_url(dir / "source.bin"),
                                          content.size(), {}, {}, {}});

        std::vector<std::unique_ptr<FileTransfer>> local_transfers;
        local_transfers.push_back(std::make_unique<LocalCopyFileTransfer>());
        const ArtifactTransfer local_only(std::move(local_transfers));
        local_only.execute(plan, {}, nullptr, {});
        check("a file://-only plan completes with no HTTP transfer registered",
              read_file(dir / "cache" / "model.bin") == content);
        check("the source is untouched", read_file(dir / "source.bin") == content);
        fs::remove_all(dir);
    }

    // --- a planned name stays inside its directory ---------------------------
    {
        const fs::path dir = make_temp_dir("placement");
        const std::string content = "bytes a listing asked for";
        write_file(dir / "source.bin", content);

        std::vector<std::unique_ptr<FileTransfer>> local_transfers;
        local_transfers.push_back(std::make_unique<LocalCopyFileTransfer>());
        const ArtifactTransfer local_only(std::move(local_transfers));

        DownloadPlan nested;
        nested.download_path = lemon::utils::path_to_utf8(dir / "cache");
        nested.files.push_back(
            DownloadFile{"sub/dir/model.bin", file_url(dir / "source.bin"), content.size(),
                         {}, {}, {}});
        local_only.execute(nested, {}, nullptr, {});
        check("a relative subdirectory inside the target still lands",
              read_file(dir / "cache" / "sub" / "dir" / "model.bin") == content);

        for (const std::string& escaped : {"../../escaped.bin", "/escaped.bin", ".", "side/.."}) {
            DownloadPlan plan;
            plan.download_path = lemon::utils::path_to_utf8(dir / "other");
            plan.files.push_back(
                DownloadFile{escaped, file_url(dir / "source.bin"), content.size(), {}, {}, {}});

            bool threw = false;
            std::string message;
            try {
                local_only.execute(plan, {}, nullptr, {});
            } catch (const std::exception& e) {
                threw = true;
                message = e.what();
            }
            check("a name leaving its directory is refused: " + escaped, threw);
            check("the refusal names the file it refused", message.find(escaped) != std::string::npos);
        }

        // Grammar, not only containment. `C:evil` is not absolute on Windows yet replaces
        // the target when appended; `a\b` is one legal name on POSIX and two components on
        // Windows; a control character names nothing. All are refused on every platform.
        for (const std::string& unusable : {"C:evil", "name\n.gguf", "model\tx", "nul:name"}) {
            DownloadPlan plan;
            plan.download_path = lemon::utils::path_to_utf8(dir / "other");
            plan.files.push_back(
                DownloadFile{unusable, file_url(dir / "source.bin"), content.size(), {}, {}, {}});

            bool threw = false;
            std::string message;
            try {
                local_only.execute(plan, {}, nullptr, {});
            } catch (const std::exception& e) {
                threw = true;
                message = e.what();
            }
            check("an unusable name is refused: " + display(unusable), threw);
            check("the refusal names the unusable name", display(message).find(display(unusable)) !=
                                                            std::string::npos);
        }

        // A backslash is a separator, not an illegal character: `a\b` means the same nested
        // name `a/b` on the platform that reads it that way, and nested names are placed.
        {
            const std::string nested = "back\\slash/model.bin";
            DownloadPlan plan;
            plan.download_path = lemon::utils::path_to_utf8(dir / "slashes");
            plan.files.push_back(
                DownloadFile{nested, file_url(dir / "source.bin"), content.size(), {}, {}, {}});
            local_only.execute(plan, {}, nullptr, {});
            const bool as_one_name = fs::exists(dir / "slashes" / nested);
            const bool as_two_levels = fs::exists(dir / "slashes" / "back" / "slash" / "model.bin");
            check("a backslash nests rather than escaping", as_one_name || as_two_levels);
            check("and it stays under the target",
                  !fs::exists(dir / "back") && !fs::exists(dir / "back\\slash"));
        }
        check("no escaped file was created beside the target",
              !fs::exists(fs::temp_directory_path() / "escaped.bin") &&
                  !fs::exists("/escaped.bin"));
        check("a refused plan opens no directory", !fs::exists(dir / "other"));
        fs::remove_all(dir);
    }

    // --- a symlink standing where the bytes go ------------------------------
    {
        const fs::path dir = make_temp_dir("symlink_in_the_way");
        const std::string content = "bytes a listing asked for";
        write_file(dir / "source.bin", content);
        const fs::path outside = dir / "outside_target.bin";
        const std::string untouched = "must still say this afterwards";
        write_file(outside, untouched);

        std::vector<std::unique_ptr<FileTransfer>> local_transfers;
        local_transfers.push_back(std::make_unique<LocalCopyFileTransfer>());
        const ArtifactTransfer local_only(std::move(local_transfers));

        for (const std::string& suffix : {std::string(), std::string(".partial")}) {
            const std::string which = suffix.empty() ? "final" : "staged";
            const fs::path cache = dir / ("cache_" + which);
            fs::create_directories(cache);
            const fs::path link = cache / ("model.bin" + suffix);
            std::string why;
            if (!make_symlink(outside, link, why)) {
                std::printf("[SKIP] %s symlink unavailable (%s): containment needs one\n",
                            which.c_str(), why.c_str());
                continue;
            }

            DownloadPlan plan;
            plan.download_path = lemon::utils::path_to_utf8(cache);
            plan.files.push_back(DownloadFile{"model.bin", file_url(dir / "source.bin"),
                                              content.size(), {}, {}, {}});
            bool threw = false;
            std::string message;
            try {
                local_only.execute(plan, {}, nullptr, {});
            } catch (const std::exception& e) {
                threw = true;
                message = e.what();
            }
            check("a " + which + " symlink stops the plan before any byte moves", threw);
            check("the " + which + " symlink's target keeps its own bytes",
                  read_file(outside) == untouched);
            check("the " + which + " symlink is left alone, not replaced",
                  fs::is_symlink(link));
            (void)message;
        }
        fs::remove_all(dir);
    }

    // --- a shared hub cache filled by another tool -------------------------
    {
        const fs::path dir = make_temp_dir("shared_hub_cache");
        const std::string fetched = "bytes lemonade would have written";
        write_file(dir / "source.bin", fetched);

        const fs::path repo = dir / "models--unsloth--Qwen3-0.6B-GGUF";
        const fs::path snapshot = repo / "snapshots" / "commitsha";
        fs::create_directories(snapshot);
        const std::string shared = "bytes hf download already put here";
        write_file(repo / "blobs" / "sha256-bbb", shared);
        const std::string foreign = "bytes that belong to another application";
        write_file(dir / "outside.bin", foreign);

        write_file(dir / "models--other--repo" / "blobs" / "blob.bin", foreign);

        const fs::path entry = snapshot / "model.bin";
        std::string why;
        const std::vector<std::pair<std::string, bool>> links = {
            {"../../blobs/sha256-bbb", true},
            {"../../../outside.bin", false},
            {"../../../models--other--repo/blobs/blob.bin", false}};
        for (const auto& [link_target, shares_the_repository] : links) {
            fs::remove(entry);
            if (!make_symlink(link_target, entry, why)) {
                std::printf("[SKIP] shared hub cache symlink unavailable (%s)\n", why.c_str());
                break;
            }

            DownloadPlan plan;
            plan.download_path = lemon::utils::path_to_utf8(snapshot);
            plan.files.push_back(DownloadFile{"model.bin", file_url(dir / "source.bin"),
                                              fetched.size(), {}, {}, {}});
            bool threw = false;
            std::string message;
            try {
                ArtifactTransfer::with_default_transfers().execute(plan, {}, nullptr, {});
            } catch (const std::exception& e) {
                threw = true;
                message = e.what();
            }
            check("a snapshot entry linking " + link_target +
                      (shares_the_repository ? " counts as already downloaded"
                                             : " is refused"),
                  threw != shares_the_repository);
            if (threw) {
                std::printf("       %s\n", display(message).c_str());
            }
            check("the shared blob keeps its bytes", read_file(repo / "blobs" / "sha256-bbb") == shared);
            check("every foreign byte is untouched",
                  read_file(dir / "outside.bin") == foreign &&
                      read_file(dir / "models--other--repo" / "blobs" / "blob.bin") == foreign);
            check("the snapshot entry is still a link", fs::is_symlink(entry));
            check("nothing was staged beside it", !fs::exists(snapshot / "model.bin.partial"));
        }
        fs::remove_all(dir);
    }

    // --- a resume asks the host that holds the bytes ------------------------
    {
        const fs::path dir = make_temp_dir("resume_hop");
        write_file(dir / "model.bin.partial", kWholeBody.substr(0, kPartialBytes));
        DownloadPlan plan;
        plan.download_path = lemon::utils::path_to_utf8(dir);
        plan.files.push_back(
            DownloadFile{"model.bin", base_a + "/resolve-range", kWholeBody.size(),
                         "sha256", kWholeSha256, {}});
        loopback_transfer().execute(plan, {}, nullptr, {});
        check("the range request survives the redirect",
              g_cdn.requested == "bytes=" + std::to_string(kPartialBytes) + "-");
        check("only the missing bytes are fetched",
              g_cdn.delivered == kWholeBody.size() - kPartialBytes);
        check("the resumed download needs no retry", g_cdn.hits == 1);
        check("the resumed file is whole and hashes as expected",
              read_file(dir / "model.bin") == kWholeBody);
        fs::remove_all(dir);
    }
    {
        const fs::path dir = make_temp_dir("resume_refused");
        write_file(dir / "model.bin.partial", kWholeBody.substr(0, kPartialBytes));
        DownloadPlan plan;
        plan.download_path = lemon::utils::path_to_utf8(dir);
        plan.files.push_back(
            DownloadFile{"model.bin", base_a + "/resolve-range-ignored", kWholeBody.size(),
                         "sha256", kWholeSha256, {}});
        loopback_transfer().execute(plan, {}, nullptr, {});
        check("a host that ignores the range is asked with it",
              g_cdn_whole.requested == "bytes=" + std::to_string(kPartialBytes) + "-");
        check("the whole file is not appended to the partial one",
              read_file(dir / "model.bin") == kWholeBody);
        check("refusing that answer starts the download over", g_cdn_whole.hits > 1);
        fs::remove_all(dir);
    }
    // --- a resume answered with the wrong bytes -----------------------------
    // No size and no hash in these plans: nothing downstream would notice bytes
    // from another offset inside the finished file.
    const auto resume_from_partial = [](const std::string& url, const char* tag) {
        const fs::path dir = make_temp_dir(tag);
        write_file(dir / "model.bin.partial", kWholeBody.substr(0, kPartialBytes));
        DownloadPlan plan;
        plan.download_path = lemon::utils::path_to_utf8(dir);
        plan.files.push_back(DownloadFile{"model.bin", url, 0, {}, {}, {}});
        std::string message;
        try {
            loopback_transfer().execute(plan, {}, nullptr, {});
        } catch (const std::exception& e) {
            message = e.what();
        }
        const std::string landed =
            fs::exists(dir / "model.bin") ? read_file(dir / "model.bin") : std::string();
        fs::remove_all(dir);
        return std::make_pair(landed.empty() || landed == kWholeBody, message);
    };

    LyingCdn offset_elsewhere(
        kWholeBody, "bytes 0-" + std::to_string(kWholeBody.size() - 1) + "/" +
                        std::to_string(kWholeBody.size()));
    LyingCdn no_content_range(kWholeBody, "");
    {
        const auto outcome = resume_from_partial(offset_elsewhere.url(), "resume_shifted");
        check("a 206 naming another offset is not appended to the partial file", outcome.first);
        check("a 206 naming another offset is not taken as the resume",
              offset_elsewhere.hits() > 1);
        if (!outcome.second.empty()) {
            std::printf("       %s\n", display(outcome.second).c_str());
        }
    }
    {
        const auto outcome = resume_from_partial(no_content_range.url(), "resume_unlabelled");
        check("a 206 without a Content-Range is not appended to the partial file", outcome.first);
        check("a 206 without a Content-Range is not taken as the resume",
              no_content_range.hits() > 1);
        if (!outcome.second.empty()) {
            std::printf("       %s\n", display(outcome.second).c_str());
        }
    }

    // --- the token stops bytes in flight -----------------------------------
    {
        const fs::path dir = make_temp_dir("mid_cancel");
        DownloadPlan plan;
        plan.download_path = lemon::utils::path_to_utf8(dir);
        plan.files.push_back(DownloadFile{"slow.bin", base_a + "/slow", 0, {}, {}, {}});

        std::atomic<bool> cancelled{false};
        CancelToken token;
        token.flag = &cancelled;

        const auto started = std::chrono::steady_clock::now();
        bool threw = false;
        std::string message;
        try {
            loopback_transfer().execute(
                plan, {},
                [&](const lemon::DownloadProgress& update) {
                    if (update.bytes_downloaded > 0) {
                        cancelled.store(true);
                    }
                    return true;
                },
                token);
        } catch (const std::exception& e) {
            threw = true;
            message = e.what();
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();

        check("a token set mid-transfer cancels the download", threw);
        check("the cancellation message is stable",
                                              message == std::string("Download cancelled"));
        check("the cancelled transfer stops early, not after the drip", elapsed < 1500);
        check("no final file is put in place", !fs::exists(dir / "slow.bin"));
        check("the partial file survives for a resume", fs::exists(dir / "slow.bin.partial"));
        fs::remove_all(dir);
    }

    // --- a closed update channel stops bytes in flight ----------------------
    // How an Ollama client leaving POST /api/pull says so: no token, only a
    // progress callback whose write failed.
    {
        const fs::path dir = make_temp_dir("client_left");
        DownloadPlan plan;
        plan.download_path = lemon::utils::path_to_utf8(dir);
        plan.files.push_back(DownloadFile{"slow.bin", base_a + "/slow", 0, {}, {}, {}});

        bool refused = false;
        int after_refusal = 0;
        bool threw = false;
        std::string message;
        try {
            loopback_transfer().execute(
                plan, {},
                [&](const lemon::DownloadProgress& update) {
                    if (refused) ++after_refusal;
                    refused = refused || update.bytes_downloaded > 0;
                    return !refused;
                },
                {});
        } catch (const std::exception& e) {
            threw = true;
            message = e.what();
        }
        check("a client that stops reading cancels the download", threw);
        check("the cancellation message is stable",
              message == std::string("Download cancelled"));
        check("the drip stops at the closed report rather than the end of the file",
              after_refusal == 0);
        check("no final file is put in place", !fs::exists(dir / "slow.bin"));
        check("the partial file survives for a resume", fs::exists(dir / "slow.bin.partial"));
        fs::remove_all(dir);
    }

    a.stop();
    b.stop();
    server_a.join();
    server_b.join();

    if (g_failures == 0) {
        std::printf("All file transfer tests passed.\n");
    } else {
        std::printf("%d file transfer test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
