#include <lemon/models/catalog.h>
#include <lemon/models/catalog_set.h>
#include <lemon/models/configured_catalog.h>
#include <lemon/utils/path_utils.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "support/test_state_isolation.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using lemon::json;
using lemon::ModelInfo;
using lemon::models::CatalogAccumulator;
using lemon::models::CatalogSet;
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

static std::size_t enumerate(const std::shared_ptr<lemon::models::ModelCatalog>& catalog) {
    CatalogAccumulator accumulator;
    catalog->enumerate(accumulator);
    return accumulator.size();
}

struct CatalogHost {
    httplib::Server svr;
    std::thread thread;
    int port = 0;
    std::string body = R"({"Fetched-Model":{"recipe":"llamacpp","labels":["chat"]}})";

    void start() {
        port = svr.bind_to_any_port("127.0.0.1");
        if (port <= 0) {
            return;
        }
        svr.Get("/models.json", [this](const httplib::Request&, httplib::Response& res) {
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

int main() {
    const fs::path scratch = lemon::test::make_scratch_dir("configured_catalog");

    parse_tests();
    file_catalog_tests(scratch);
    http_catalog_tests(scratch);
    precedence_tests(scratch);
    product_wiring_tests(scratch);

    if (g_failures == 0) {
        std::printf("All configured catalog tests passed.\n");
    } else {
        std::printf("%d configured catalog test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
