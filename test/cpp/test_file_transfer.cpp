// The transfer gate the spec names: scheme dispatch across http and file, one
// mixed-scheme plan reporting one progress total, a file://-only plan that
// needs no network, and auth_headers() that belong to the origin that minted
// them — kept across a same-origin redirect, stripped at a cross-origin one,
// and never restored on a later hop. A loopback httplib server stands in for
// the registry; two ports are two origins.

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
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
