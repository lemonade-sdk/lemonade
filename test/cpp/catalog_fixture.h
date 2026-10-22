#pragma once

#include "lemon/backends/configured/backend_definition_store.h"

namespace catalog_fixture {
using lemon::json;
inline json definition(const std::string& identity = "catalog-alpha") {
    auto doc = json::parse(R"({
        "schema_version":1,"id":"catalog-alpha","recipe":"catalog-alpha","kind":"local",
        "metadata":{
            "display_name":"Catalog Alpha","server_name":"catalog-fixture","binary":"catalog-engine",
            "default_device":"cpu","bin_variants":["cpu"],
            "options":[
                {"name":"alpha_batch","value_type":"integer","default_value":2,"minimum":1,"maximum":8,"help":"Batch size","group":"Alpha","ui":{"label":"Batch size","control":"number","order":10}},
                {"name":"alpha_enabled","value_type":"boolean","default_value":false},
                {"name":"alpha_text","value_type":"string","default_value":""},
                {"name":"alpha_bias","value_type":"integer","default_value":-1}
            ]
        },
        "install":{"strategy":"external_only"},
        "launch":{
            "prepare_order":["artifacts","backend","program","ports"],
            "backend":{"fixed":"cpu"},"program":{"source":"external_or_managed","install_policy":"always"},
            "ports":[{"name":"http","reject_zero":true,"error":"No port"}],
            "argv":[{"args":["--port",{"string":{"ref":"/launch/ports/http"}},"--batch",{"string":{"ref":"/options/alpha_batch"}}]}],
            "environment":{"preset":"inherit","set":[]},"working_directory":"inherit",
            "readiness":{"port":"http","path":"/health","timeout":5},
            "watchdog":{"port":"http","path":"/health","policy":"standard"},
            "output":{"inherit_levels":["debug"],"filter":false}
        },
        "operations":[{
            "operation":"chat_completion","delivery":"buffered","model_modes":["chat"],
            "transport":{"kind":"http_json","method":"POST","path":"/chat","port":"http","timeout":5,"response_codec":"json.v1"},
            "request":{"base":"identity"}
        }]
    })");
    doc["id"] = identity;
    doc["recipe"] = identity;
    return doc;
}
inline json beta_definition() {
    auto doc = definition("catalog-beta");
    doc["metadata"]["display_name"] = "Catalog Beta";
    doc["metadata"]["options"] = {{{"name", "beta_scale"}, {"value_type", "number"}, {"default_value", 0.0}, {"enum", {0, 0.5, 1}}}};
    doc["launch"]["argv"] = {{{"args", {"--beta"}}}};
    doc["install"] = json::parse(R"({"strategy":"github_release","install_recipe":"beta-installer","pin_recipe":"beta-pin","default_versions":{"cpu":"v7"},"cases":[{"backend":["cpu"],"repo":"fixture/beta","filename":{"concat":["beta-",{"ref":"/install/version"},".zip"]}}]})");
    return doc;
}
}
