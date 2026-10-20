#include "backend_definition_v2_internal.h"

#include <cstdio>
#include <fstream>
#include <limits>

using namespace lemon::backends;
using Json = nlohmann::json;
namespace {
int checks = 0;
int failures = 0;
void check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL %s\n", label.c_str()); }
}
const DefinitionKey key{DefinitionNamespace::Builtin, "fixture", 1};
const std::string bytes = R"({"schema_version":2,"id":"fixture","recipe":"fixture"})";
DefinitionSnapshotResult snapshot(const std::string& source, const DefinitionLimits& limits = {}) {
    return make_v2_source_snapshot({{key, source, "/private/secret.json"}}, limits);
}
void rejects(const std::string& source, const std::string& code, const std::string& label, const DefinitionLimits& limits = {}) {
    const auto result = snapshot(source, limits);
    check(!result.snapshot && result.diagnostics.entries.size() == 1 && result.diagnostics.entries[0].code == code, label);
}
}
int main() {
    auto limits = DefinitionLimits{};
    check(snapshot(bytes).snapshot != nullptr, "minimal source admitted");
    {
        std::string owned = bytes;
        const auto result = snapshot(owned);
        owned.assign(owned.size(), 'x');
        const auto& record = result.snapshot->sources.at(v2::key_string(key));
        check(record.bytes == bytes && record.nodes == 4, "snapshot owns immutable source bytes and preflight node count");
    }
    rejects("{", "invalid_json", "malformed source rejected");
    rejects("[]", "invalid_type", "root requires object");
    rejects(R"({"schema_version":2,"id":"fixture","recipe":"fixture","id":"other"})", "duplicate_key", "duplicate envelope key");
    const auto duplicate = snapshot(R"({"schema_version":2,"id":"fixture","recipe":"fixture","merge":{"metadata":{"config_extra":{"a":1,"\u0061":2}}}})");
    check(!duplicate.snapshot && duplicate.diagnostics.entries[0].code == "duplicate_key" && duplicate.diagnostics.entries[0].pointer == "/merge/metadata/config_extra/a", "decoded duplicate key has exact source pointer");
    const auto empty = snapshot(R"({"schema_version":2,"id":"fixture","recipe":"fixture","merge":{"metadata":{"config_extra":{"":{"":1,"":2}}}}})");
    check(!empty.snapshot && empty.diagnostics.entries[0].pointer == "/merge/metadata/config_extra//", "empty object keys preserved in diagnostics");
    rejects(R"({"schema_version":2,"id":"other","recipe":"fixture"})", "source_identity_mismatch", "loader identity cannot be changed");
    for (const auto& field : {"merge", "edits", "extends"}) {
        auto source = Json::parse(bytes);
        source[field] = nullptr;
        rejects(source.dump(), "invalid_type", std::string("wrong control type ") + field);
    }
    for (const auto& field : {"evil", "trusted", "grants"}) {
        auto source = Json::parse(bytes);
        source[field] = true;
        rejects(source.dump(), "unknown_key", std::string("closed source ") + field);
    }
    for (const auto& number : {"18446744073709551616", "-9223372036854775809", "1e9999"}) {
        rejects(std::string(R"({"schema_version":2,"id":"fixture","recipe":"fixture","merge":{"metadata":{"config_extra":{"number":)") + number + "}}}}", std::string(number) == "1e9999" ? "invalid_json" : "invalid_number", std::string("numeric overflow ") + number);
    }
    for (const auto& number : {"18446744073709551615", "-9223372036854775808", "-0.0", "1e2"}) {
        check(snapshot(std::string(R"({"schema_version":2,"id":"fixture","recipe":"fixture","merge":{"metadata":{"config_extra":{"number":)") + number + "}}}}").snapshot != nullptr, std::string("numeric boundary ") + number);
    }
    limits.source_bytes = bytes.size();
    check(snapshot(bytes, limits).snapshot != nullptr, "source byte boundary");
    rejects(bytes + " ", "limit_exceeded", "source bytes limit+1", limits);
    limits = {};
    limits.closure_source_bytes = bytes.size();
    check(snapshot(bytes, limits).snapshot != nullptr, "isolated closure byte boundary");
    rejects(bytes + " ", "limit_exceeded", "isolated closure bytes limit+1", limits);
    limits = {};
    limits.source_depth = 1;
    check(snapshot(bytes, limits).snapshot != nullptr, "source depth boundary");
    auto source = Json::parse(bytes);
    source["merge"] = Json::object();
    rejects(source.dump(), "limit_exceeded", "source depth rejected before next container frame", limits);
    limits = {};
    limits.closure_source_nodes = 4;
    check(snapshot(bytes, limits).snapshot != nullptr, "source node boundary");
    rejects(source.dump(), "limit_exceeded", "source nodes limit+1", limits);
    limits = {};
    limits.edits_per_source = 1;
    source = Json::parse(bytes);
    source["edits"] = Json::array({{{"op", "clear"}, {"path", "/hooks"}}});
    check(snapshot(source.dump(), limits).snapshot != nullptr, "edit count boundary");
    source["edits"].push_back(source["edits"][0]);
    rejects(source.dump(), "limit_exceeded", "edit count limit+1", limits);
    limits = {};
    limits.pointer_bytes = 32;
    source["edits"] = Json::array({{{"op", "clear"}, {"path", "/" + std::string(31, 'x')}}});
    check(snapshot(source.dump(), limits).snapshot != nullptr, "pointer bytes boundary before pointer decoding");
    source["edits"][0]["path"] = "/" + std::string(32, 'x');
    rejects(source.dump(), "limit_exceeded", "pointer bytes limit+1", limits);
    limits = {};
    limits.pointer_bytes = 32;
    const Json rationale{{"subject","/" + std::string(32,'x')},{"text","fixture"},{"source","fixture"},{"tests",Json::array({"fixture"})}};
    source = Json::parse(bytes);
    source["merge"]["rationale"] = Json::array({rationale});
    rejects(source.dump(),"limit_exceeded","rationale pointer rejected before payload DOM",limits);
    source.erase("merge");
    source["edits"] = Json::array({{{"op","replace"},{"path","/rationale"},{"value",Json::array({rationale})}}});
    rejects(source.dump(),"limit_exceeded","edit rationale pointer rejected before payload DOM",limits);
    limits = {};
    limits.snapshot_documents = 1;
    check(make_v2_source_snapshot({{key, bytes, ""}}, limits).snapshot != nullptr, "snapshot count boundary");
    check(!make_v2_source_snapshot({{key, bytes, ""}, {key, bytes, ""}}, limits).snapshot, "snapshot count limit+1 before indexing");
    limits = {};
    limits.snapshot_source_bytes = bytes.size();
    check(snapshot(bytes, limits).snapshot != nullptr, "snapshot source byte boundary");
    rejects(bytes + " ", "limit_exceeded", "snapshot source bytes limit+1", limits);
    limits = {};
    const auto retained = snapshot(bytes).snapshot->logical_bytes;
    limits.retained_logical_bytes = retained;
    check(snapshot(bytes, limits).snapshot != nullptr, "retained logical bytes boundary");
    limits.retained_logical_bytes = retained - 1;
    rejects(bytes, "limit_exceeded", "retained logical data limit+1 before index insertion", limits);
    limits = {};
    limits.source_bytes = std::numeric_limits<std::size_t>::max();
    limits.snapshot_source_bytes = std::numeric_limits<std::size_t>::max();
    limits.retained_logical_bytes = std::numeric_limits<std::size_t>::max();
    check(snapshot(bytes, limits).snapshot != nullptr, "maximum accounting limits do not overflow");
    limits = {};
    limits.diagnostics = 0;
    const auto capped = snapshot("{", limits);
    check(!capped.snapshot && capped.diagnostics.entries.empty() && capped.diagnostics.truncated, "zero diagnostic budget preserves failure");
    rejects(R"({"schema_version":2,"id":"fixture","recipe":"fixture","extends":{"external":"parent","unknown":1}})", "unknown_key", "closed preflight parent header");
    const auto duplicate_source = make_v2_source_snapshot({{key, bytes, ""}, {key, bytes, ""}});
    check(!duplicate_source.snapshot && duplicate_source.diagnostics.entries[0].code == "duplicate_source", "duplicate loader key rejected atomically");
    for (const auto& invalid_key : {DefinitionKey{DefinitionNamespace::Builtin,"fixture",0}, DefinitionKey{DefinitionNamespace::External,"fixture",1}}) {
        const auto result = make_v2_source_snapshot({{invalid_key, bytes, ""}});
        check(!result.snapshot && result.diagnostics.entries[0].code == "invalid_source_key", "loader namespace/contract invariants");
    }
    const auto external = make_v2_source_snapshot({{{DefinitionNamespace::External,"fixture",0}, bytes,""}});
    check(external.snapshot != nullptr, "external source has contract zero");
    {
        std::ifstream stream(std::string(LEMON_TEST_FIXTURES) + "/backend-v2/llamacpp.example.json");
        const auto base = Json::parse(stream);
        const auto data = base.dump();
        const DefinitionKey base_key{DefinitionNamespace::Builtin,"llamacpp",1};
        const auto frozen = make_v2_source_snapshot({{base_key,data,""}}).snapshot;
        DefinitionResolver reference(frozen);
        const auto resolved = reference.resolve(base_key).definition;
        check(resolved != nullptr,"bounded complete definition resolves");
        const auto attempt = [&](const DefinitionLimits& bounds) {
            DefinitionResolver resolver(frozen,bounds);
            return resolver.resolve(base_key);
        };
        const auto exact = [&](std::size_t DefinitionLimits::* member, std::size_t value, const std::string& name) {
            DefinitionLimits bounds;
            bounds.*member = value;
            check(attempt(bounds).definition != nullptr,name + " boundary accepted");
            bounds.*member = value - 1;
            const auto rejected = attempt(bounds);
            check(!rejected.definition && !rejected.diagnostics.entries.empty() && rejected.diagnostics.entries[0].code == "limit_exceeded",name + " limit+1 rejected");
        };
        exact(&DefinitionLimits::effective_bytes,resolved->metrics.effective_bytes,"effective bytes");
        exact(&DefinitionLimits::effective_nodes,resolved->metrics.effective_nodes,"effective nodes");
        exact(&DefinitionLimits::work_bytes,resolved->metrics.work_bytes,"cumulative work bytes");
        exact(&DefinitionLimits::work_nodes,resolved->metrics.work_nodes,"cumulative work visits");
        exact(&DefinitionLimits::retained_logical_bytes,resolved->metrics.retained_logical_bytes,"retained resolution data");
        std::size_t measurement_retained = 0;
        DefinitionLimits measurement_limits;
        v2::Budget measurement_budget{measurement_limits,{},measurement_retained};
        exact(&DefinitionLimits::effective_depth,v2::measure(resolved->declaration,measurement_budget).depth,"effective nesting");
        exact(&DefinitionLimits::closure_source_nodes,frozen->sources.begin()->second.nodes,"closure source nodes");
        exact(&DefinitionLimits::closure_source_bytes,data.size(),"closure source bytes");
        v2::admission_counters = {};
        auto bounds = DefinitionLimits{};
        bounds.closure_source_nodes = frozen->sources.begin()->second.nodes - 1;
        check(!attempt(bounds).definition && v2::admission_counters.dom == 0,"closure rejected before DOM allocation");
        DefinitionResolver cached(frozen);
        const auto once = cached.resolve(base_key).definition;
        const auto twice = cached.resolve(base_key).definition;
        check(once == twice,"memoized result reused");
        const Json child{{"schema_version",2},{"id","child"},{"recipe","child"},{"extends",{{"builtin","llamacpp"},{"contract",1}}}};
        const auto child_bytes = child.dump();
        const DefinitionKey child_key{DefinitionNamespace::Builtin,"child",1};
        const auto family = make_v2_source_snapshot({{base_key,data,""},{child_key,child_bytes,""}}).snapshot;
        DefinitionResolver family_reference(family);
        const auto family_result = family_reference.resolve(child_key).definition;
        bounds = {};
        bounds.work_bytes = family_result->metrics.work_bytes - 1;
        DefinitionResolver work_capped(family,bounds);
        check(work_capped.resolve(base_key).definition != nullptr && !work_capped.resolve(child_key).definition,"memoized parent retains inherited cumulative work charge");
        bounds = {};
        bounds.retained_logical_bytes = once->metrics.retained_logical_bytes;
        DefinitionResolver capped_cache(frozen,bounds);
        check(capped_cache.resolve(base_key).definition != nullptr && !capped_cache.resolve(base_key).definition,"cache hit still charged against transaction retention");
        bounds = {};
        bounds.effective_bytes = 10;
        v2::admission_counters = {};
        check(!attempt(bounds).definition && v2::admission_counters.clones == 1,"projected root growth rejected before cloning payload");
    }
    {
        auto bounds = DefinitionLimits{};
        std::size_t retained_data = 0;
        v2::Budget budget{bounds,{},retained_data};
        v2::LayerState state;
        v2::apply_layer(state,Json{{"merge",{{"metadata",{{"config_extra",{{"x",std::string(8192,'x')}}}}}}}},key,budget);
        const auto previous_work = budget.metrics.work_bytes;
        v2::apply_layer(state,Json{{"edits",Json::array({{{"op","remove"},{"path","/metadata/config_extra/x"}}})}},key,budget);
        check(budget.metrics.work_bytes >= previous_work,"removal does not refund cumulative work");
        const auto nodes_before = v2::admission_counters.dom;
        auto low = DefinitionLimits{};
        low.source_depth = 1;
        rejects(R"({"schema_version":2,"id":"fixture","recipe":"fixture","merge":{"metadata":{}}})","limit_exceeded","deep source rejected by preflight",low);
        check(v2::admission_counters.dom == nodes_before,"source preflight rejection never reaches DOM allocator");
        std::string padded = bytes;
        padded.insert(padded.size()-1,",\"merge\":{\"metadata\":{\"config_extra\":{\"value\":\"" + std::string(4096,'x') + "\"}}}");
        low.source_bytes = padded.size() - 1;
        check(!snapshot(padded,low).snapshot,"large lexer token refused at raw source admission");
    }
    {
        DefinitionLimits bounds;
        std::size_t retained_data = 0;
        v2::Budget budget{bounds,{},retained_data};
        v2::LayerState state;
        v2::apply_layer(state,Json{{"merge",{{"metadata",{{"config_extra",{{"x",1}}}}}}}},key,budget);
        const auto saved = state.root;
        const auto previous_clones = v2::admission_counters.clones;
        bounds.effective_nodes = state.root->metrics.nodes;
        bool rejected = false;
        try { v2::apply_layer(state,Json{{"edits",Json::array({{{"op","add"},{"path","/metadata/config_extra/new"},{"value",42}}})}},key,budget); }
        catch (const v2::LimitExceeded&) { rejected = true; }
        check(rejected && state.root == saved && previous_clones == v2::admission_counters.clones,"rejected edit projection never reaches clone allocator");
        const auto previous_pointers = v2::admission_counters.pointers;
        bounds.pointer_bytes = 4;
        rejected = false;
        try { v2::join_pointer("/xxx","é",bounds); } catch (const v2::LimitExceeded&) { rejected = true; }
        check(rejected && previous_pointers == v2::admission_counters.pointers,"pointer expansion rejected before allocation seam");
        bounds.pointer_bytes = 4096;
        bounds.retained_logical_bytes = retained_data;
        const auto previous_origins = v2::admission_counters.origins;
        rejected = false;
        try { v2::origin_child(state.root->origin,"new",budget); } catch (const v2::LimitExceeded&) { rejected = true; }
        check(rejected && previous_origins == v2::admission_counters.origins,"provenance rejected before origin allocator");
        std::size_t used = std::numeric_limits<std::size_t>::max() - 1;
        rejected = false;
        try { v2::charge(used,2,std::numeric_limits<std::size_t>::max()); } catch (const v2::LimitExceeded&) { rejected = true; }
        check(rejected && used == std::numeric_limits<std::size_t>::max() - 1,"overflow-sized increment rejected without wrap");
        rejected = false;
        try { v2::checked_product(std::numeric_limits<std::size_t>::max(),2); } catch (const v2::LimitExceeded&) { rejected = true; }
        check(rejected,"overflow-sized product rejected without wrap");
        DefinitionLimits intern_limits;
        std::size_t intern_retained = 0;
        v2::Budget intern_budget{intern_limits,{},intern_retained};
        const auto first = v2::origin_child(state.root->origin,"interned",intern_budget);
        const auto second = v2::origin_child(state.root->origin,"interned",intern_budget);
        check(first->token == second->token,"pointer segments use interned storage");
    }
    std::printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
