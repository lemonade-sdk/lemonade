#include <lemon/models/catalog.h>
#include <lemon/models/catalog_locator.h>
#include <lemon/models/json_catalog.h>
#include <nlohmann/json.hpp>

#include "support/test_state_isolation.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using lemon::json;
using lemon::ModelInfo;
using lemon::models::CatalogAccumulator;
using lemon::models::CatalogLocator;
using lemon::models::CatalogState;
using lemon::models::file_catalog_locator;
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

    if (g_failures == 0) {
        std::printf("All catalog locator tests passed.\n");
    } else {
        std::printf("%d catalog locator test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
