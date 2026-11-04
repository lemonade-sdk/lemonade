// One binary, the suites below run in order in one process. Each keeps its scratch
// directories and its own failure count; the only shared state is the process-wide
// models directory, which every suite sets on entry.

#include <lemon/models/catalog.h>
#include <lemon/models/catalog_fetch_clock.h>
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
#include <lemon/models/catalog_set.h>
#include <lemon/models/configured_catalog.h>
#include <lemon/utils/path_utils.h>


namespace locator {

namespace fs = std::filesystem;
using lemon::json;
using lemon::ModelInfo;
using lemon::models::CatalogAccumulator;
using lemon::models::CatalogFetchClock;
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

struct ServingDocument {
    httplib::Server svr;
    std::thread thread;
    std::mutex mutex;
    int port = 0;
    bool send_validators = true;
    std::atomic<int> conditional_reads{0};
    std::atomic<int> reads{0};
    std::string body = R"({"HTTP-Model":{"recipe":"llamacpp","labels":["chat"]}})";
    std::string etag = "\"v1\"";

    void start() {
        port = svr.bind_to_any_port("127.0.0.1");
        if (port <= 0) {
            return;
        }
        svr.Get("/catalog.json", [this](const httplib::Request& req, httplib::Response& res) {
            if (req.method != "HEAD") {
                ++reads;
            }
            if (req.has_header("If-None-Match")) {
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
        // No validators: a length is the only comparison.
        svr.Get("/opaque.json", [this](const httplib::Request&, httplib::Response& res) {
            std::lock_guard<std::mutex> lock(mutex);
            res.set_content(body, "application/json");
        });
        // 200 whatever it was asked: only the validators mean anything.
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
    const std::string rewritten =
        R"({"A":{"recipe":"llamacpp","labels":["chat"]},
            "B":{"recipe":"llamacpp","labels":["chat"]}})";
    document.rewrite(rewritten, "\"v2\"");
    CatalogState second;
    const json second_body = locator->read(&second).value_or(json::object());
    check("the rewritten document is read", second_body.size() == 2);
    check("the new validators replace the old", second.etag != first.etag);

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
    CatalogState grown;
    opaque->read(&grown);
    check("the read records a size the next read can compare", grown.size > sized.size);

    document.stop();
}

// Each read builds its own locator, the way a store build does.
static void fetch_clock_tests(const fs::path& scratch) {
    ServingDocument document;
    document.start();
    if (document.port <= 0) {
        check("the catalog server bound a port for the fetch clock", false);
        return;
    }

    const auto clock = std::make_shared<CatalogFetchClock>();
    const fs::path cache = scratch / "fetched" / "clock.json";

    HttpCatalogSource source = serving(document, "/catalog.json", cache);
    source.fresh_for = std::chrono::seconds(60);
    source.clock = clock;

    const json first = http_catalog_locator("clocked", source)->read(nullptr).value_or(json());
    check("a remote catalog is fetched once", document.reads == 1 && first.contains("HTTP-Model"));

    const json again = http_catalog_locator("clocked", source)->read(nullptr).value_or(json());
    check("a rebuild inside the window asks no question", document.reads == 1);
    check("a rebuild inside the window still contributes its models", again.contains("HTTP-Model"));

    document.rewrite(R"({"HTTP-Model":{"recipe":"llamacpp","labels":["chat"]},
                        "Late":{"recipe":"llamacpp","labels":["chat"]}})", "\"v2\"");
    const json held = http_catalog_locator("clocked", source)->read(nullptr).value_or(json());
    check("an edit inside the window is not picked up", held.size() == 1);

    clock->forget_all();
    const json after_forget = http_catalog_locator("clocked", source)->read(nullptr).value_or(json());
    check("forgetting the clock asks again", document.reads == 2 && after_forget.size() == 2);

    HttpCatalogSource other = source;
    other.cache_path = (scratch / "fetched" / "other.json").string();
    const int before_other = document.reads.load();
    http_catalog_locator("other", other)->read(nullptr);
    check("one catalog's window does not cover another's", document.reads == before_other + 1);
    http_catalog_locator("clocked", source)->read(nullptr);
    check("the first catalog is still inside its window", document.reads == before_other + 1);

    clock->mark_fetched("clocked");
    fs::remove(cache);
    const int before_missing = document.reads.load();
    http_catalog_locator("clocked", source)->read(nullptr);
    check("a window with no cached body fetches", document.reads == before_missing + 1);

    HttpCatalogSource brief = serving(document, "/catalog.json", scratch / "fetched" / "brief.json");
    brief.fresh_for = std::chrono::seconds(1);
    brief.clock = clock;
    http_catalog_locator("brief", brief)->read(nullptr);
    const int after_brief = document.reads.load();
    http_catalog_locator("brief", brief)->read(nullptr);
    check("a window that has not expired asks nothing", document.reads == after_brief);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    http_catalog_locator("brief", brief)->read(nullptr);
    check("an expired window asks again", document.reads == after_brief + 1);
    http_catalog_locator("brief", brief)->read(nullptr);
    check("a 304 restarts the window", document.reads == after_brief + 1);

    HttpCatalogSource unclocked = serving(document, "/catalog.json", scratch / "fetched" / "unclocked.json");
    unclocked.fresh_for = std::chrono::seconds(60);
    const int before_unclocked = document.reads.load();
    http_catalog_locator("unclocked", unclocked)->read(nullptr);
    http_catalog_locator("unclocked", unclocked)->read(nullptr);
    check("a locator with no clock reads every time",
          document.reads == before_unclocked + 2);

    HttpCatalogSource nowindow = unclocked;
    nowindow.fresh_for = std::chrono::seconds(0);
    nowindow.clock = clock;
    nowindow.cache_path = (scratch / "fetched" / "nowindow.json").string();
    const int before_nowindow = document.reads.load();
    http_catalog_locator("nowindow", nowindow)->read(nullptr);
    http_catalog_locator("nowindow", nowindow)->read(nullptr);
    check("a window of zero reads every time", document.reads == before_nowindow + 2);

    document.stop();
}


static void body_limit_tests(const fs::path& scratch) {
    ServingDocument document;
    document.start();
    if (document.port <= 0) {
        check("the catalog server bound a port for the size limit", false);
        return;
    }

    const std::string small = R"({"HTTP-Model":{"recipe":"llamacpp","labels":["chat"]}})";
    const fs::path cache = scratch / "sized" / "body.json";

    HttpCatalogSource exact = serving(document, "/catalog.json", cache);
    exact.max_body_bytes = small.size();
    check("a body at the limit is read", http_catalog_locator("sized", exact)->read(nullptr).has_value());

    HttpCatalogSource over = exact;
    over.cache_path = (scratch / "sized" / "over.json").string();
    over.max_body_bytes = small.size() - 1;
    check("a body over the limit yields no document",
          !http_catalog_locator("over", over)->read(nullptr).has_value());
    check("an oversized body is never cached", !fs::exists(scratch / "sized" / "over.json"));

    document.rewrite(R"({"HTTP-Model":{"recipe":"llamacpp","labels":["chat"]},
                        "Padding":{"recipe":"llamacpp","labels":["chat"]}})", "\"v9\"");
    HttpCatalogSource too_small = exact;
    too_small.max_body_bytes = 8;
    const json served = http_catalog_locator("sized", too_small)->read(nullptr).value_or(json());
    check("an oversized answer serves the last good body", served.size() == 1);
    check("the cached body survives one that will not fit", read_text(cache) == small);

    document.stop();
}

int run() {
    const fs::path scratch = lemon::test::make_scratch_dir("catalog_locator");
    fetch_clock_tests(scratch);
    body_limit_tests(scratch);
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

    write_file(document, R"({"A":{"recipe":"llamacpp","labels":["chat"]},
                            "B":{"recipe":"llamacpp","labels":["chat"]}})");
    CatalogState refreshed;
    const json refreshed_body = locator->read(&refreshed).value_or(json::object());
    check("a rewritten document is read as what it is now", refreshed_body.size() == 2);
    check("the refreshed state is current again",
          refreshed.present && (refreshed.size != read.size || refreshed.modified != read.modified));

    {
        const fs::path absent = scratch / "absent.json";
        CatalogState absent_state;
        const auto missing = file_catalog_locator("missing", absent.string());
        check("an absent document reads as no body", !missing->read(nullptr).has_value());
        check("absent reads as an empty state", !missing->read(&absent_state) && !absent_state.present);
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

}  // namespace locator


namespace configured {

namespace fs = std::filesystem;
using lemon::json;
using lemon::ModelInfo;
using lemon::models::CatalogAccumulator;
using lemon::models::CatalogSet;
using lemon::models::CatalogFetchClock;
using lemon::models::CatalogSetDeps;
using lemon::models::ConfiguredCatalog;
using lemon::models::Conflict;
using lemon::models::configured_catalogs_to_config;
using lemon::models::DocEntry;
using lemon::models::EnumeratedCatalog;
using lemon::models::JsonCatalog;
using lemon::models::make_catalog_descriptor;
using lemon::models::make_configured_catalog;
using lemon::models::parse_configured_catalogs;
using lemon::models::prune_cached_bodies;
using lemon::models::product_catalogs;
using lemon::models::RegistryRowRules;
using lemon::models::catalog_rank::configured;
using lemon::models::catalog_rank::registrations;
using lemon::models::catalog_rank::scan;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

static ConfiguredCatalog parse_one(const json& entry) {
    const auto parsed = parse_configured_catalogs(json::array({entry}));
    return parsed.catalogs.empty() ? ConfiguredCatalog{} : parsed.catalogs.front();
}

static std::string refusal_for(const json& entry) {
    const auto parsed = parse_configured_catalogs(json::array({entry}));
    return parsed.refusals.empty() ? std::string() : parsed.refusals.front();
}

static std::shared_ptr<lemon::models::ModelCatalog> pick(
    const std::vector<std::shared_ptr<lemon::models::ModelCatalog>>& catalogs,
    const std::string& id) {
    for (const auto& catalog : catalogs) {
        if (catalog->id() == id) {
            return catalog;
        }
    }
    return nullptr;
}

static std::size_t enumerate(const std::shared_ptr<lemon::models::ModelCatalog>& catalog) {
    CatalogAccumulator accumulator;
    catalog->enumerate(accumulator);
    return accumulator.size();
}

struct CatalogHost {
    httplib::Server svr;
    std::thread thread;
    std::atomic<int> reads{0};
    int port = 0;
    std::string body = R"({"Fetched-Model":{"recipe":"llamacpp","labels":["chat"]}})";

    void start() {
        port = svr.bind_to_any_port("127.0.0.1");
        if (port <= 0) {
            return;
        }
        svr.Get("/models.json", [this](const httplib::Request&, httplib::Response& res) {
            ++reads;
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

    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port) + "/models.json";
    }
};

static std::shared_ptr<JsonCatalog> registration_catalog(const std::string& name, double size) {
    json rows{{name, {{"recipe", "llamacpp"}, {"labels", {"chat"}}, {"size", size}}}};
    return std::make_shared<JsonCatalog>(
        make_catalog_descriptor("user_models.json", registrations, Conflict::Overwrite,
                                DocEntry{"user_models", "Registrations", "registrations", {}, ""}),
        std::make_shared<const json>(std::move(rows)), RegistryRowRules{});
}

static std::shared_ptr<EnumeratedCatalog> scan_catalog() {
    return std::make_shared<EnumeratedCatalog>(
        make_catalog_descriptor("extra_models_dir", scan, Conflict::KeepExisting,
                                DocEntry{"scan", "Scan", "a scan", {}, ""}),
        [](const CatalogAccumulator&) {
            ModelInfo contested;
            contested.model_name = "Contested";
            contested.size = 9.0;
            ModelInfo shared;
            shared.model_name = "Shared";
            shared.size = 9.0;
            ModelInfo found;
            found.model_name = "Scanned";
            return std::vector<ModelInfo>{contested, shared, found};
        });
}

static void parse_tests() {
    const auto file_record = parse_one(json{{"id", "lab"}, {"path", "/srv/lab.json"}});
    check("a file record keeps its path", file_record.id == "lab" && file_record.path == "/srv/lab.json");

    const auto http_record =
        parse_one(json{{"id", "fleet"}, {"url", "https://example.org/models.json"},
                       {"required", true}});
    check("an http record keeps its url and its demand",
          http_record.url == "https://example.org/models.json" && http_record.required);

    const json round = configured_catalogs_to_config(parse_configured_catalogs(json::array(
                                                 {json{{"id", "lab"}, {"path", "/srv/lab.json"}},
                                                  json{{"id", "fleet"},
                                                       {"url", "https://example.org/models.json"},
                                                       {"required", true}}}))
                           .catalogs);
    check("the record survives the trip back to config",
          round.size() == 2 && round[1]["required"].get<bool>() &&
              !round[0].contains("required"));

    check("a record naming two sources is refused",
          !refusal_for(json{{"id", "a"}, {"path", "x"}, {"url", "https://e/y"}}).empty());
    check("a record naming no source is refused", !refusal_for(json{{"id", "a"}}).empty());
    check("a record naming no id is refused", !refusal_for(json{{"path", "x"}}).empty());
    check("an id that could name a path is refused",
          !refusal_for(json{{"id", "../escape"}, {"path", "x"}}).empty());
    check("a url with credentials in it is refused",
          !refusal_for(json{{"id", "a"}, {"url", "https://tok@example.org/m.json"}}).empty());
    check("a url that is not http is refused",
          !refusal_for(json{{"id", "a"}, {"url", "ftp://e/m.json"}}).empty());
    check("a plaintext url without the opt-in is refused",
          !refusal_for(json{{"id", "a"}, {"url", "http://e/m.json"}}).empty());
    check("a plaintext url with the opt-in is accepted",
          refusal_for(json{{"id", "a"},
                           {"url", "http://e/m.json"},
                           {"allow_insecure_http", true}})
              .empty());
    check("a setting a catalog does not have is refused",
          !refusal_for(json{{"id", "a"}, {"path", "x"}, {"auth_env", "T"}}).empty());
    check("a cache path a catalog did not ask for is refused",
          !refusal_for(json{{"id", "a"}, {"path", "x"}, {"cache_path", "/tmp/x"}}).empty());
    check("a demand that is not a boolean is refused",
          !refusal_for(json{{"id", "a"}, {"path", "x"}, {"required", "yes"}}).empty());

    const auto twice = parse_configured_catalogs(
        json::array({json{{"id", "a"}, {"path", "/x"}}, json{{"id", "a"}, {"path", "/y"}}}));
    check("one duplicate id costs the duplicate, not the set",
          twice.catalogs.size() == 1 && twice.refusals.size() == 1 &&
              twice.refusals[0].find("'a'") != std::string::npos);

    bool threw = false;
    try {
        parse_configured_catalogs(json{{"id", "a"}});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check("a value that is not a list of catalogs is refused whole", threw);
}

static void file_catalog_tests(const fs::path& scratch) {
    const fs::path document = scratch / "lab.json";
    {
        std::ofstream out(document, std::ios::binary);
        out << R"({"Lab-Model":{"recipe":"llamacpp","labels":["chat"]}})";
    }
    const auto catalog = make_configured_catalog(
        parse_one(json{{"id", "lab"}, {"path", document.string()}}), RegistryRowRules{},
        (scratch / "cache").string());
    check("a configured file contributes its models", enumerate(catalog) == 1);
    check("a configured catalog says which setting named it",
          catalog->descriptor().config_key == "model_catalogs" &&
          catalog->descriptor().rank == configured &&
          catalog->descriptor().claim == Conflict::KeepExisting &&
          catalog->descriptor().host_specific && !catalog->required());
}

static void http_catalog_tests(const fs::path& scratch) {
    CatalogHost host;
    host.start();
    if (host.port <= 0) {
        check("the catalog host bound a port", false);
        return;
    }

    const fs::path cache = scratch / "cache";
    const auto catalog = make_configured_catalog(
        parse_one(json{{"id", "fleet"}, {"url", host.url()}, {"allow_insecure_http", true}}),
        RegistryRowRules{}, cache.string());

    check("a configured url contributes its models", enumerate(catalog) == 1);
    check("a fetched body is cached under the catalog id",
          fs::exists(cache / "catalogs" / "fleet.json"));

    const auto clock = std::make_shared<CatalogFetchClock>();
    const auto record =
        parse_one(json{{"id", "fleet"}, {"url", host.url()}, {"allow_insecure_http", true}});

    CatalogSetDeps shared;
    shared.catalog_cache_dir = (scratch / "shared-clock").string();
    shared.configured_catalogs = {record};
    shared.catalog_fetch_clock = clock;

    const int before_build = host.reads.load();
    check("a configured catalog reaches the build through its deps",
          enumerate(pick(product_catalogs(shared), "fleet")) == 1 &&
          host.reads == before_build + 1);
    check("the next build with the same clock asks nothing",
          enumerate(pick(product_catalogs(shared), "fleet")) == 1 &&
          host.reads == before_build + 1);

    CatalogSetDeps unclocked = shared;
    unclocked.catalog_fetch_clock.reset();
    unclocked.catalog_cache_dir = (scratch / "no-clock").string();
    check("a build with no clock reads the document",
          enumerate(pick(product_catalogs(unclocked), "fleet")) == 1 &&
          host.reads == before_build + 2);

    const std::string unreachable = "http://127.0.0.1:9/models.json";
    const auto missing = make_configured_catalog(
        parse_one(json{{"id", "gone"}, {"url", unreachable}, {"allow_insecure_http", true}}),
        RegistryRowRules{}, (scratch / "empty-cache").string());
    check("an unreachable optional catalog contributes nothing", enumerate(missing) == 0);

    const auto demanded = make_configured_catalog(
        parse_one(json{{"id", "demanded"}, {"url", unreachable},
                       {"allow_insecure_http", true}, {"required", true}}),
        RegistryRowRules{}, (scratch / "empty-cache").string());
    bool threw = false;
    try {
        enumerate(demanded);
    } catch (const std::exception&) {
        threw = true;
    }
    check("an unreachable required catalog fails the build", threw);

    host.stop();
}

static void precedence_tests(const fs::path& scratch) {
    // The row values differ per document, so a contested name has a visible winner.
    const fs::path document = scratch / "contested.json";
    {
        std::ofstream out(document, std::ios::binary);
        out << R"({"Contested":{"recipe":"llamacpp","labels":["chat"],"size":2.5},
                   "Shared":{"recipe":"llamacpp","labels":["chat"],"size":3.5}})";
    }
    const auto from_config = make_configured_catalog(
        parse_one(json{{"id", "lab"}, {"path", document.string()}}), RegistryRowRules{},
        (scratch / "cache").string());
    const auto held = registration_catalog("Contested", 1.5);

    CatalogSet set;
    set.add(from_config);
    set.add(held);
    set.add(scan_catalog());

    const std::string order = [&set] {
        std::string names;
        for (const auto& catalog : set.snapshot()) {
            names += catalog->id() + " ";
        }
        return names;
    }();
    check("a configured catalog enumerates after registrations and before a scan",
          order == "user_models.json lab extra_models_dir ");
    check("and holds that place by rank, not by insertion order",
          from_config->descriptor().rank < set.find("extra_models_dir")->descriptor().rank);

    CatalogAccumulator accumulator;
    for (const auto& catalog : set.snapshot()) {
        catalog->enumerate(accumulator);
    }
    check("a registration keeps a name a configured document also claims",
          accumulator.models().count("Contested") == 1 &&
              accumulator.models().at("Contested").size == 1.5);
    check("a configured document keeps a name only it and a scan claim",
          accumulator.models().count("Shared") == 1 &&
              accumulator.models().at("Shared").size == 3.5);
    check("a scan still contributes what nobody claimed",
          accumulator.models().count("Scanned") == 1);
}

static void product_wiring_tests(const fs::path& scratch) {
    // The ranks this test reads are the ones product_catalogs() assigns.
    const fs::path document = scratch / "wiring.json";
    {
        std::ofstream out(document, std::ios::binary);
        out << R"({"Wired-Model":{"recipe":"llamacpp","labels":["chat"]}})";
    }
    CatalogSetDeps deps;
    deps.registry_rows = RegistryRowRules{};
    deps.configured_catalogs = {parse_one(json{{"id", "wired"}, {"path", document.string()}})};
    deps.catalog_cache_dir = (scratch / "cache").string();
    deps.scan_extra_models = [](const CatalogAccumulator&) {
        ModelInfo info;
        info.model_name = "Scanned-Only";
        return std::vector<ModelInfo>{info};
    };

    CatalogSet set;
    set.replace_with(product_catalogs(deps));

    std::string order;
    for (const auto& catalog : set.snapshot()) {
        order += catalog->id() + " ";
    }
    check("config.json's catalogs enumerate between registrations and a scan",
          order == "server_models.json user_models.json wired extra_models_dir ");
    check("a directory scan enumerates after a configured catalog",
          set.find("wired")->descriptor().rank <
              set.find("extra_models_dir")->descriptor().rank);
}


static void touch(const fs::path& path) {
    std::ofstream out(path, std::ios::binary);
    out << "{}";
}

static void prune_tests(const fs::path& scratch) {
    const fs::path cache = scratch / "prune";
    const fs::path dir = cache / "catalogs";
    fs::create_directories(dir);
    touch(dir / "keep.json");
    touch(dir / "keep.json.validators");
    touch(dir / "gone.json");
    touch(dir / "gone.json.validators");
    touch(dir / "notes.txt");
    fs::create_directories(dir / "shaped-like-a-body.json");

    prune_cached_bodies(cache.string(), {"keep"});

    check("a body for a catalog nobody configures is removed", !fs::exists(dir / "gone.json"));
    check("its validator sidecar goes too", !fs::exists(dir / "gone.json.validators"));
    check("a configured catalog keeps its body",
          fs::exists(dir / "keep.json") && fs::exists(dir / "keep.json.validators"));
    check("a file this cache does not write is left alone", fs::exists(dir / "notes.txt"));
    check("a directory is not a cached body", fs::is_directory(dir / "shaped-like-a-body.json"));

    prune_cached_bodies((scratch / "never-created").string(), {});
    check("a missing catalog cache is not an error", true);
}

int run() {
    const fs::path scratch = lemon::test::make_scratch_dir("configured_catalog");

    parse_tests();
    file_catalog_tests(scratch);
    http_catalog_tests(scratch);
    precedence_tests(scratch);
    product_wiring_tests(scratch);
    prune_tests(scratch);

    if (g_failures == 0) {
        std::printf("All configured catalog tests passed.\n");
    } else {
        std::printf("%d configured catalog test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}

}  // namespace configured

int main() {
    int failures = 0;
    std::printf("\n=== locator ===\n");
    failures |= locator::run();
    std::printf("\n=== configured ===\n");
    failures |= configured::run();
    return failures;
}
