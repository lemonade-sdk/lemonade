// The two name tables a store build derives from its model set: which name wins,
// what a client is answered with, and what a shadowed entry falls back to.

#include "lemon/model_manager.h"
#include "lemon/models/entry_identity.h"

#include <map>
#include <string>

using lemon::ModelInfo;
using lemon::models::ModelAliasMaps;
using lemon::models::compute_model_alias_maps;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

static ModelInfo model(const std::string& key, const std::string& recipe = "llamacpp") {
    ModelInfo info;
    info.model_name = key;
    info.recipe = recipe;
    return info;
}

static void test_precedence() {
    ModelAliasMaps against_builtin = compute_model_alias_maps(
        {{"Foo", model("Foo")}, {"user.Foo", model("user.Foo")}});
    check("a registration takes the bare name from a built-in",
          against_builtin.public_model_aliases["Foo"] == "user.Foo");
    check("the registration is listed under the bare name",
          against_builtin.canonical_public_names["user.Foo"] == "Foo");
    check("the shadowed built-in is listed under its canonical id",
          against_builtin.canonical_public_names["Foo"] == "builtin.Foo" &&
              against_builtin.public_model_aliases["builtin.Foo"] == "Foo");

    ModelAliasMaps against_scan = compute_model_alias_maps(
        {{"extra.Foo", model("extra.Foo")}, {"user.Foo", model("user.Foo")}});
    check("a registration outranks a scanned file",
          against_scan.public_model_aliases["Foo"] == "user.Foo");
    check("the shadowed scan keeps the id it was minted with",
          against_scan.canonical_public_names["extra.Foo"] == "extra.Foo");
}

static void test_input_aliases() {
    ModelInfo claiming = model("Foo");
    claiming.input_aliases = {"bar"};
    ModelAliasMaps demotion =
        compute_model_alias_maps({{"bar", model("bar")}, {"Foo", claiming}});
    check("an unqualified built-in yields its bare name to an input alias",
          demotion.public_model_aliases["bar"] == "Foo");
    check("and is reachable under the id it is pushed to",
          demotion.canonical_public_names["bar"] == "builtin.bar" &&
              demotion.public_model_aliases["builtin.bar"] == "bar");

    ModelInfo cloud = model("fireworks.x");
    cloud.cloud_provider = "fireworks";
    ModelInfo claims_cloud = model("Foo");
    claims_cloud.input_aliases = {"fireworks.x"};
    ModelAliasMaps self_qualified = compute_model_alias_maps(
        {{"fireworks.x", cloud}, {"Foo", claims_cloud}});
    check("a provider-qualified id is not pushed aside by an input alias",
          self_qualified.public_model_aliases["fireworks.x"] == "fireworks.x");
    check("and is never re-prefixed as a built-in",
          self_qualified.public_model_aliases.count("builtin.fireworks.x") == 0 &&
              self_qualified.canonical_public_names.count("builtin.fireworks.x") == 0);

    ModelInfo aliased = model("Foo");
    aliased.input_aliases = {"user.Other"};
    ModelAliasMaps canonical_alias = compute_model_alias_maps({{"Foo", aliased}});
    check("an input alias naming a canonical id resolves to its target",
          canonical_alias.public_model_aliases["user.Other"] == "Foo");
}

static void test_collections() {
    // A registered collection must be listed under the id its user registered,
    // fetched and chatted it by, not under a bare name that names nothing else.
    ModelAliasMaps registered_collection = compute_model_alias_maps(
        {{"user.MyColl", model("user.MyColl", "collection.omni")}});
    check("a registered collection lists under its canonical id",
          registered_collection.canonical_public_names["user.MyColl"] == "user.MyColl");

    ModelAliasMaps builtin_collection = compute_model_alias_maps(
        {{"TheirColl", model("TheirColl", "collection.omni")}});
    check("a built-in collection lists under its bare name",
          builtin_collection.canonical_public_names["TheirColl"] == "TheirColl");
}

int main() {
    test_precedence();
    test_input_aliases();
    test_collections();

    if (g_failures == 0) {
        std::printf("All alias map tests passed.\n");
    } else {
        std::printf("%d alias map test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
