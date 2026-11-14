#pragma once

#include "backend_catalog_admission_internal.h"
#include "backend_definition_export_fixture.h"
#include "catalog_fixture.h"
#include "lemon/backends/configured/backend_catalog.h"

#include <cstdio>

namespace admission_fixture {
using namespace export_fixture;
inline int checks = 0, failures = 0;
inline void check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL %s\n", label.c_str()); }
}
inline bool has_code(const CompositionDiagnostics& errors, const std::string& code) {
    for (const auto& error : errors.entries) if (error.code == code) return true;
    return false;
}
inline Json root(const std::string& id = "root") {
    auto value = Json::parse(read_bytes("export.synthetic.json"));
    value["id"] = id;
    value["recipe"] = id;
    auto& merge = value["merge"];
    merge.erase("hooks");
    merge.erase("artifacts");
    merge["launch"]["prepare_order"] = {"backend", "artifacts", "program", "ports"};
    merge["operations"][0]["request"].erase("hooks");
    merge["operations"][0].erase("response");
    return value;
}
inline Json child(const std::string& id, const std::string& parent) {
    return {{"schema_version", 2}, {"id", id}, {"recipe", id}, {"extends", {{"external", parent}}}};
}
inline Json legacy(const std::string& id = "legacy") {
    auto value = catalog_fixture::definition(id);
    value["metadata"]["options"] = Json::array();
    value["launch"]["argv"] = Json::array();
    return value;
}
inline std::shared_ptr<const FrozenBuiltinCatalog> empty_builtins() {
    return CatalogAdmissionBuilder::test_builtins(BuiltinCatalogLoader::test_freeze({}, export_fixture::empty_exports()).snapshot, {});
}
inline CatalogSnapshotResult snapshot(const std::vector<Json>& values, std::shared_ptr<const FrozenBuiltinCatalog> builtins = {}, DefinitionLimits limits = {}) {
    std::vector<std::string> bytes;
    for (const auto& value : values) bytes.push_back(value.dump());
    std::vector<ExternalDefinitionInput> sources;
    for (const auto& value : bytes) sources.push_back({value, "/PRIVATE_SERVER_PATH"});
    return make_catalog_source_snapshot(builtins ? builtins : freeze_builtin_catalog(), sources, limits);
}
inline CatalogAdmissionResult admit(const std::vector<Json>& values, std::shared_ptr<const FrozenBuiltinCatalog> builtins = {}, DefinitionLimits limits = {}) {
    auto frozen = snapshot(values, std::move(builtins), limits);
    if (!frozen.snapshot) return {nullptr, frozen.diagnostics};
    return admit_catalog(std::move(frozen.snapshot));
}
inline void reject(const std::vector<Json>& values, const std::string& code, const std::string& label) {
    const auto result = admit(values);
    check(!result.catalog && has_code(result.diagnostics, code), label);
    if (!has_code(result.diagnostics, code)) for (const auto& error : result.diagnostics.entries) std::printf("  %s %s\n", error.code.c_str(), error.pointer.c_str());
}
inline int finish() {
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
}
