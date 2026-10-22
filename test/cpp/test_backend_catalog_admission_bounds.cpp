#include "catalog_admission_fixture.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

using namespace admission_fixture;
int main() {
    DefinitionLimits limits;
    check(limits.snapshot_documents == 1024 && limits.snapshot_source_bytes == 64 * 1024 * 1024 && limits.retained_logical_bytes == 128 * 1024 * 1024, "documented aggregate defaults");
    const auto builtins = empty_builtins();
    std::vector<Json> values;
    for (int i = 0; i < 1024; ++i) values.push_back({{"schema_version", 2}, {"id", "node-" + std::to_string(i)}, {"recipe", "node-" + std::to_string(i)}});
    check(snapshot(values, builtins).snapshot != nullptr, "1024 definitions indexed");
    values.push_back({{"schema_version", 2}, {"id", "node-over"}, {"recipe", "node-over"}});
    const auto over = snapshot(values, builtins);
    check(!over.snapshot && has_code(over.diagnostics, "limit_exceeded"), "1025 definitions rejected before retention");
    values.resize(1008);
    const auto rows_over = admit(values);
    check(!rows_over.catalog && has_code(rows_over.diagnostics, "limit_exceeded"), "public row bound includes compiled-only recipes");
    for (int i = 1008; i < 1015; ++i) values.push_back({{"schema_version", 2}, {"id", "node-" + std::to_string(i)}, {"recipe", "node-" + std::to_string(i)}});
    check(snapshot(values).snapshot != nullptr, "builtin sources charged once in 1024 count");
    values.push_back({{"schema_version", 2}, {"id", "node-over"}, {"recipe", "node-over"}});
    check(!snapshot(values).snapshot, "builtin sources cannot bypass definition count");
    limits.retained_logical_bytes = 512 * 1024 * 1024;
    std::vector<std::string> padded;
    for (int i = 0; i < 64; ++i) {
        auto bytes = Json{{"schema_version", 2}, {"id", "padded-" + std::to_string(i)}, {"recipe", "padded-" + std::to_string(i)}}.dump();
        bytes.resize(1024 * 1024, ' ');
        padded.push_back(std::move(bytes));
    }
    std::vector<ExternalDefinitionInput> inputs;
    for (const auto& bytes : padded) inputs.push_back({bytes, "PRIVATE_PATH"});
    check(make_catalog_source_snapshot(builtins, inputs, limits).snapshot != nullptr, "64 MiB aggregate source boundary accepted");
    inputs.push_back({" ", "PRIVATE_PATH"});
    const auto bytes_over = make_catalog_source_snapshot(builtins, inputs, limits);
    check(!bytes_over.snapshot && has_code(bytes_over.diagnostics, "limit_exceeded"), "64 MiB plus one rejected before copy");
    values = {root(), child("one", "root"), child("two", "one")};
    const auto measured = admit(values, builtins);
    check(measured.catalog != nullptr, "retained graph measured");
    if (!measured.catalog) return finish();
    const auto used = measured.catalog->metrics().retained_logical_bytes;
    limits = {};
    limits.retained_logical_bytes = used;
    check(admit(values, builtins, limits).catalog != nullptr, "retained declaration provenance dependency authority exact boundary");
    --limits.retained_logical_bytes;
    const auto retained_over = admit(values, builtins, limits);
    check(!retained_over.catalog && has_code(retained_over.diagnostics, "limit_exceeded"), "retained aggregate one byte short rejects");
    const auto legacy_only = admit({legacy()}, builtins);
    check(legacy_only.catalog && legacy_only.catalog->metrics().retained_logical_bytes > legacy().dump().size() * 8, "v1 compilation allowance charged");
    const auto single = admit({root()}, builtins);
    check(used > single.catalog->metrics().retained_logical_bytes * 3, "dependent snapshots and replay charge aggregate ledger");
    const auto source = snapshot(values, builtins);
    limits.retained_logical_bytes = source.snapshot ? used / 2 : 0;
    check(!admit(values, builtins, limits).catalog, "resolution and authority share catalog budget");
    limits = {};
    limits.diagnostics = 0;
    const auto no_diagnostics = admit({child("child", "missing")}, builtins, limits);
    check(!no_diagnostics.catalog && no_diagnostics.diagnostics.entries.empty() && no_diagnostics.diagnostics.truncated, "aggregate diagnostic budget truncates fail closed");
    bool overflow = false;
    try { v2::checked_sum(std::numeric_limits<std::size_t>::max(), 1); } catch (const v2::LimitExceeded&) { overflow = true; }
    check(overflow, "aggregate ledger arithmetic rejects overflow");
    values = {root(), legacy()};
    limits = {};
    limits.snapshot_source_bytes = 1;
    check(!snapshot(values, builtins, limits).snapshot, "mixed source bytes checked before v1 compilation");
    auto many_options = root();
    for (int i = 0; i < 1017; ++i) many_options["merge"]["metadata"]["options"].push_back({{"id", "option-" + std::to_string(i)},
        {"name", "option_" + std::to_string(i)}, {"value_type", "integer"}, {"default_value", 1}});
    check(admit({many_options}, builtins).catalog != nullptr, "1024 projected options including common options");
    many_options["merge"]["metadata"]["options"].push_back({{"id", "option-over"}, {"name", "option_over"}, {"value_type", "integer"}, {"default_value", 1}});
    const auto projection_over = admit({many_options}, builtins);
    check(!projection_over.catalog && has_code(projection_over.diagnostics, "invalid_public_projection"), "projected option count includes implicit common fields");
    const auto directory = std::filesystem::temp_directory_path() / ("catalog-admission-discovery-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto definitions = directory / "backends.d";
    std::filesystem::create_directories(definitions);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{directory};
    for (int i = 0; i < 1017; ++i) std::ofstream(definitions / (std::to_string(i) + ".json")) << "{}";
    bool discovery_count = false;
    try { discover_backend_definitions(directory.string()); }
    catch (const DefinitionValidationError& error) { discovery_count = error.diagnostics.front().code == "limit_exceeded"; }
    check(discovery_count, "discovery bounds selected paths before accumulating files");
    std::filesystem::remove_all(definitions);
    std::filesystem::create_directories(definitions);
    const std::string padding(1024 * 1024, ' ');
    for (int i = 0; i < 65; ++i) std::ofstream(definitions / (std::to_string(i) + ".json")).write(padding.data(), static_cast<std::streamsize>(padding.size()));
    bool discovery_bytes = false;
    try { discover_backend_definitions(directory.string()); }
    catch (const DefinitionValidationError& error) { discovery_bytes = error.diagnostics.front().code == "limit_exceeded"; }
    check(discovery_bytes, "discovery bounds aggregate bytes before reading files");
    return finish();
}
