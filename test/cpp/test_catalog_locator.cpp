#include <lemon/models/catalog.h>
#include <lemon/models/catalog_locator.h>
#include <lemon/models/json_catalog.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "support/test_state_isolation.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using lemon::json;
using lemon::ModelInfo;
using lemon::models::CatalogAccumulator;
using lemon::models::CatalogLocator;
using lemon::models::CatalogState;
using lemon::models::file_catalog_locator;
using lemon::models::HttpCatalogSource;
using lemon::models::http_catalog_locator;
using lemon::models::JsonCatalog;
using lemon::models::make_catalog_descriptor;
using lemon::models::Conflict;
using lemon::models::catalog_rank::scan;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

static void write_file(const fs::path& path, const std::string& body) {
    std::ofstream out(path, std::ios::binary);
    out << body;
}

static std::shared_ptr<JsonCatalog> catalog(const std::shared_ptr<CatalogLocator>& locator,
                                            bool required) {
    return std::make_shared<JsonCatalog>(
        make_catalog_descriptor("located", scan, Conflict::KeepExisting,
                                lemon::models::DocEntry{"located", "Located", "located", {}, ""},
                                "", false, required),
        locator, lemon::models::RegistryRowRules{});
}

static std::size_t enumerate(const JsonCatalog& subject) {
    CatalogAccumulator accumulator;
    subject.enumerate(accumulator);
    return accumulator.size();
}

static std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// One document of model rows over HTTP, answering the conditional requests a
// catalog locator makes.
struct ServingDocument {
    httplib::Server svr;
    std::thread thread;
    std::mutex mutex;
    int port = 0;
    bool send_validators = true;
    std::atomic<bool> refuses_head{false};
    std::atomic<int> conditional_reads{0};
    std::string body = R"({"HTTP-Model":{"recipe":"llamacpp","labels":["chat"]}})";
    std::string etag = "\"v1\"";

    void start() {
        port = svr.bind_to_any_port("127.0.0.1");
        if (port <= 0) {
            return;
        }
        svr.Get("/catalog.json", [this](const httplib::Request& req, httplib::Response& res) {
            if (req.method == "HEAD") {
                if (refuses_head.load()) {
                    res.status = 405;
                    return;
                }
            } else if (req.has_header("If-None-Match")) {
                ++conditional_reads;
            }
            const std::string match = req.get_header_value("If-None-Match");
            std::lock_guard<std::mutex> lock(mutex);
            if (!etag.empty() && match == etag) {
                res.status = 304;
                return;
            }
            if (send_validators && !etag.empty()) {
                res.set_header("ETag", etag);
            }
            res.set_content(body, "application/json");
        });
        // A host that names no validators at all: length is the only comparison.
        svr.Get("/opaque.json", [this](const httplib::Request&, httplib::Response& res) {
            std::lock_guard<std::mutex> lock(mutex);
            res.set_content(body, "application/json");
        });
        // A host that answers 200 whatever it was asked, so a status line says
        // nothing and only the validators mean anything.
        svr.Get("/always200.json", [this](const httplib::Request&, httplib::Response& res) {
            std::lock_guard<std::mutex> lock(mutex);
            res.set_header("ETag", etag);
            res.set_content(body, "application/json");
        });
        thread = std::thread([this] { svr.listen_after_bind(); });
        svr.wait_until_ready();
    }

    void stop() {
        svr.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    void rewrite(const std::string& new_body, const std::string& new_etag) {
        std::lock_guard<std::mutex> lock(mutex);
        body = new_body;
        etag = new_etag;
    }

    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port) + path;
    }
};

static HttpCatalogSource serving(const ServingDocument& document, const std::string& path,
                                 const fs::path& cache) {
    HttpCatalogSource source;
    source.url = document.url(path);
    source.cache_path = cache.string();
    source.allow_insecure_http = true;
    return source;
}

// A port nothing listens on.
static const char* kUnreachable = "http://127.0.0.1:9/catalog.json";

static void http_catalog_tests(const fs::path& scratch) {
    ServingDocument document;
    document.start();
    if (document.port <= 0) {
        check("the catalog server bound a port", false);
        return;
    }

    const fs::path cache = scratch / "http" / "catalog.json";
    const auto locator = http_catalog_locator("http", serving(document, "/catalog.json", cache));

    CatalogState first;
    const json first_body = locator->read(&first).value_or(json::object());
    check("an HTTP document is read", first_body.contains("HTTP-Model"));
    check("the read records the validators it saw",
          first.present && first.size > 0 && !first.etag.empty());
    check("the last good body is left in the cache", read_text(cache) == document.body);

    check("a document answered with 304 is still enumerated", enumerate(*catalog(locator, false)) == 1);
    check("the second read asked with what it holds", document.conditional_reads == 1);
    check("the same document is not a change", !locator->changed(first));

    const std::string rewritten =
        R"({"A":{"recipe":"llamacpp","labels":["chat"]},
            "B":{"recipe":"llamacpp","labels":["chat"]}})";
    document.rewrite(rewritten, "\"v2\"");
    check("a rewritten document says it changed", locator->changed(first));

    CatalogState second;
    const json second_body = locator->read(&second).value_or(json::object());
    check("the rewritten document is read", second_body.size() == 2);
    check("the new validators replace the old", second.etag != first.etag && !locator->changed(second));

    document.refuses_head = true;
    check("a probe the server refuses asks the document", locator->changed(second));
    document.refuses_head = false;

    // A host that names no validators, so a length is the only comparison.
    const auto opaque = http_catalog_locator(
        "opaque", serving(document, "/opaque.json", scratch / "http" / "opaque.json"));
    CatalogState sized;
    opaque->read(&sized);

    {
        const auto offline =
            http_catalog_locator("offline", [&](HttpCatalogSource source) {
                source.url = kUnreachable;
                return source;
            }(serving(document, "/catalog.json", cache)));
        CatalogState served;
        const json served_body = offline->read(&served).value_or(json::object());
        check("an unreachable host serves the last good body", served_body.size() == 2);
        check("serving from the cache keeps the validators on record",
              served.present && served.etag == second.etag);
        check("an unreachable host offers no change", !offline->changed(served));
    }

    {
        HttpCatalogSource nowhere;
        nowhere.url = kUnreachable;
        nowhere.cache_path = (scratch / "http" / "nothing.json").string();
        nowhere.allow_insecure_http = true;
        const auto missing = http_catalog_locator("nowhere", std::move(nowhere));
        check("an unreachable catalog with no cache has no body", !missing->read(nullptr).has_value());
        check("an optional unreachable catalog contributes nothing",
              enumerate(*catalog(missing, false)) == 0);
        bool threw = false;
        try {
            enumerate(*catalog(missing, true));
        } catch (const std::exception&) {
            threw = true;
        }
        check("a required unreachable catalog with no cache fails the build", threw);
    }

    document.rewrite("{ this is not json", "\"v3\"");
    const json kept = locator->read(nullptr).value_or(json::object());
    check("a body that will not parse falls back to the last good one", kept.size() == 2);
    check("a body that will not parse never becomes the last good one",
          read_text(cache) == rewritten);

    const std::string three_rows =
        R"({"A":{"recipe":"llamacpp","labels":["chat"]},
            "B":{"recipe":"llamacpp","labels":["chat"]},
            "C":{"recipe":"llamacpp","labels":["chat"]}})";
    document.rewrite(three_rows, "\"v4\"");
    check("a document with no validators is compared by length", opaque->changed(sized));
    CatalogState grown;
    opaque->read(&grown);
    check("the length it measured is its own", grown.size > sized.size && !opaque->changed(grown));

    const auto relentless = http_catalog_locator(
        "relentless", serving(document, "/always200.json", scratch / "http" / "always200.json"));
    CatalogState answered;
    relentless->read(&answered);
    check("a host that ignores the condition is still a document that did not change",
          answered.present && !relentless->changed(answered));
    document.rewrite(three_rows, "\"v5\"");
    check("a validator that moved is a change even to a host that always answers",
          relentless->changed(answered));

    document.stop();
}

int main() {
    const fs::path scratch = lemon::test::make_scratch_dir("catalog_locator");
    const fs::path document = scratch / "catalog.json";
    const std::string id = "file:" + document.string();

    write_file(document, R"({"Locator-Model":{"recipe":"llamacpp","labels":["chat"]}})");

    const auto locator = file_catalog_locator(id, document.string());
    check("the locator id is its own", locator->id() == id);

    CatalogState read;
    const json body = locator->read(&read).value_or(json::object());
    check("the document is read", body.contains("Locator-Model"));
    check("the read records what it saw", read.present && read.size > 0 && read.modified != 0);

    check("a catalog over a locator contributes its models",
          enumerate(*catalog(locator, false)) == 1);

    check("nothing changed between two reads", !locator->changed(read));
    write_file(document, R"({"A":{"recipe":"llamacpp","labels":["chat"]},
                            "B":{"recipe":"llamacpp","labels":["chat"]}})");
    check("a rewritten document says it changed", locator->changed(read));

    CatalogState refreshed;
    locator->read(&refreshed);
    check("the refreshed state is current again", !locator->changed(refreshed));

    {
        const fs::path absent = scratch / "absent.json";
        const auto missing = file_catalog_locator("missing", absent.string());
        check("an absent document reads as no body", !missing->read(nullptr).has_value());
        check("absent and still absent is not a change", !missing->changed(CatalogState{}));
        check("an optional catalog contributes nothing",
              enumerate(*catalog(missing, false)) == 0);
        bool threw = false;
        try {
            enumerate(*catalog(missing, true));
        } catch (const std::exception&) {
            threw = true;
        }
        check("a required catalog with no document fails the build", threw);
    }

    {
        const fs::path broken = scratch / "broken.json";
        write_file(broken, "{ this is not json");
        const auto unreadable = file_catalog_locator("broken", broken.string());
        check("an optional catalog contributes nothing when the body will not parse",
              enumerate(*catalog(unreadable, false)) == 0);
        bool threw = false;
        try {
            enumerate(*catalog(unreadable, true));
        } catch (const std::exception&) {
            threw = true;
        }
        check("a required catalog with an unparseable body fails the build", threw);
    }

    {
        const fs::path array = scratch / "array.json";
        write_file(array, "[1,2,3]");
        const auto wrong_shape = file_catalog_locator("array", array.string());
        check("a document that is not a table of rows is refused, not half read",
              enumerate(*catalog(wrong_shape, false)) == 0);
    }

    http_catalog_tests(scratch);

    if (g_failures == 0) {
        std::printf("All catalog locator tests passed.\n");
    } else {
        std::printf("%d catalog locator test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
