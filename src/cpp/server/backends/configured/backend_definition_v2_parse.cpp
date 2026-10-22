#include "backend_catalog_ledger_internal.h"
#include "backend_definition_v2_internal.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace lemon::backends {
namespace v2 {
std::string key_string(const DefinitionKey& key) {
    return (key.space == DefinitionNamespace::Builtin ? "builtin:" : "external:") + key.id + "@" + std::to_string(key.contract);
}
}
namespace {
using Json = nlohmann::json;

struct Preflight : nlohmann::json_sax<Json> {
    struct Frame {
        bool object;
        std::string key;
        std::set<std::string> keys;
        std::size_t count = 0;
        bool edits = false;
        bool parent = false;
        bool key_seen = false;
    };
    explicit Preflight(const DefinitionLimits& limits) : limits(limits) {}
    const DefinitionLimits& limits;
    std::vector<Frame> stack;
    std::vector<std::size_t> rationale_pointer_bytes;
    Json header = Json::object();
    std::size_t nodes = 0;
    std::string code;
    std::string pointer;
    bool root_object = false;

    std::string location(bool key = false) const {
        std::string result;
        for (std::size_t i = 0; i < stack.size(); ++i) {
            const auto& frame = stack[i];
            if (frame.object && !frame.key_seen && !key) continue;
            const auto token = frame.object ? v2::pointer_token(frame.key) : std::to_string(frame.count ? frame.count - 1 : 0);
            if (result.size() >= limits.pointer_bytes || token.size() > limits.pointer_bytes - result.size() - 1) return "";
            result += "/" + token;
        }
        return result;
    }
    bool fail(const std::string& error, bool key = false) {
        code = error;
        pointer = location(key);
        return false;
    }
    bool admit_node() {
        if (nodes >= limits.closure_source_nodes) return fail("limit_exceeded");
        if (!stack.empty()) {
            auto& frame = stack.back();
            if (frame.edits && frame.count >= limits.edits_per_source) return fail("limit_exceeded");
            ++frame.count;
        }
        ++nodes;
        return true;
    }
    bool scalar(Json value) {
        if (!admit_node()) return false;
        if (stack.empty() || (stack.size() == 2 && stack.back().edits)) return fail("invalid_type");
        const auto& frame = stack.back();
        if (stack.size() == 1 && (frame.key == "merge" || frame.key == "edits" || frame.key == "extends")) return fail("invalid_type");
        if (frame.object && stack.size() == 1 && (frame.key == "schema_version" || frame.key == "id" || frame.key == "recipe")) header[frame.key] = std::move(value);
        else if (frame.parent) header["extends"][frame.key] = std::move(value);
        else if (stack.size() == 3 && stack[1].edits) header["edits"].back()[frame.key] = std::move(value);
        else if (stack.size() == 4 && stack[1].edits && (stack[2].key == "match" || stack[2].key == "before" || stack[2].key == "after")) header["edits"].back()[stack[2].key][frame.key] = std::move(value);
        return true;
    }
    bool null() override { return scalar(nullptr); }
    bool boolean(bool value) override { return scalar(value); }
    bool number_integer(number_integer_t value) override { return scalar(value); }
    bool number_unsigned(number_unsigned_t value) override { return scalar(value); }
    bool number_float(number_float_t value, const string_t& token) override {
        if (!std::isfinite(value) || token.find_first_of(".eE") == std::string::npos) return fail("invalid_number");
        return scalar(value);
    }
    bool string(string_t& value) override {
        const auto& key = stack.empty() ? std::string{} : stack.back().key;
        const bool edit_pointer = stack.size() == 3 && stack[1].edits && (key == "path" || key == "at");
        const bool rationale_pointer = stack.size() == 4 && stack[0].key == "merge" && stack[1].key == "rationale" && !stack[2].object && (key == "subject" || key == "at");
        if ((edit_pointer || rationale_pointer) && value.size() > limits.pointer_bytes) return fail("limit_exceeded");
        if (stack.size() >= 4 && stack[1].edits && stack[2].key == "value" && (key == "subject" || key == "at")) rationale_pointer_bytes.back() = std::max(rationale_pointer_bytes.back(), value.size());
        return scalar(std::move(value));
    }
    bool binary(binary_t&) override { return fail("invalid_type"); }
    bool start(bool object) {
        if (!admit_node()) return false;
        if (stack.size() >= limits.source_depth) return fail("limit_exceeded");
        bool edits = false, parent = false;
        if (stack.empty()) { root_object = object; if (!object) return fail("invalid_type"); }
        else if (stack.size() == 1) {
            const auto& key = stack.back().key;
            if (key == "merge") { if (!object) return fail("invalid_type"); header["merge"] = Json::object(); }
            if (key == "edits") { if (object) return fail("invalid_type"); edits = true; header["edits"] = Json::array(); }
            if (key == "extends") { if (!object) return fail("invalid_type"); parent = true; header["extends"] = Json::object(); }
            if (key == "id" || key == "recipe" || key == "schema_version") return fail("invalid_type");
        } else if (stack.back().parent) return fail("invalid_type");
        else if (stack.size() == 2 && stack[1].edits) {
            if (!object) return fail("invalid_type");
            header["edits"].push_back(Json::object());
            rationale_pointer_bytes.push_back(0);
        } else if (stack.size() == 3 && stack[1].edits) header["edits"].back()[stack.back().key] = object ? Json::object() : Json::array();
        else if (stack.size() == 4 && stack[1].edits && (stack[2].key == "match" || stack[2].key == "before" || stack[2].key == "after")) header["edits"].back()[stack[2].key][stack.back().key] = object ? Json::object() : Json::array();
        stack.push_back({object, {}, {}, 0, edits, parent});
        return true;
    }
    bool start_object(std::size_t) override { return start(true); }
    bool start_array(std::size_t) override { return start(false); }
    bool key(string_t& value) override {
        auto& frame = stack.back();
        frame.key = value;
        frame.key_seen = true;
        if (!frame.keys.insert(std::move(value)).second) return fail("duplicate_key", true);
        if (stack.size() == 1) {
            static const std::set<std::string> keys = {"edits", "extends", "id", "merge", "recipe", "schema_version"};
            if (!keys.count(frame.key)) return fail("unknown_key", true);
        }
        if (stack.size() == 2 && stack[0].key == "merge") {
            static const std::set<std::string> fields = {"artifacts", "hooks", "install", "kind", "launch", "metadata", "operations", "rationale", "unsupported_messages"};
            if (!fields.count(frame.key)) return fail("unknown_key", true);
        }
        if (frame.parent) {
            static const std::set<std::string> fields = {"builtin", "contract", "expected_digest", "external"};
            if (!fields.count(frame.key)) return fail("unknown_key", true);
        }
        return true;
    }
    bool end_object() override {
        if (stack.size() == 3 && stack[1].edits && header["edits"].back().value("path", Json()) == "/rationale" && rationale_pointer_bytes.back() > limits.pointer_bytes) return fail("limit_exceeded");
        stack.pop_back();
        return true;
    }
    bool end_array() override { stack.pop_back(); return true; }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return fail("invalid_json"); }
};
}

namespace v2 {
CompositionDiagnostics preflight_source(const SourceRecord& source, const DefinitionLimits& limits) {
    CompositionDiagnostics result;
    Preflight preflight(limits);
    if (source.bytes.size() > limits.source_bytes || !Json::sax_parse(source.bytes.begin(), source.bytes.end(), &preflight)) {
        if (limits.diagnostics) result.entries.push_back({source.bytes.size() > limits.source_bytes ? "limit_exceeded" : preflight.code, source.key.id, preflight.pointer, "Source admission failed", {}, {}, {}});
        else result.truncated = true;
    }
    return result;
}
}

DefinitionSnapshotResult make_v2_source_snapshot(const std::vector<DefinitionSourceView>& sources, const DefinitionLimits& limits) {
    return make_v2_source_snapshot_accounted(sources, limits, nullptr);
}

DefinitionSnapshotResult make_v2_source_snapshot_accounted(const std::vector<DefinitionSourceView>& sources,
    const DefinitionLimits& limits, std::size_t* retained) {
    DefinitionSnapshotResult result;
    accounting::owned(sizeof(DefinitionSourceSnapshot) + 64);
    auto snapshot = std::make_shared<DefinitionSourceSnapshot>();
    std::size_t source_bytes = 0;
    const auto error = [&](const DefinitionKey& key, const std::string& code, const std::string& pointer) {
        if (result.diagnostics.entries.size() < limits.diagnostics) result.diagnostics.entries.push_back({code, key.id, pointer, "Source snapshot admission failed", {}, {}, {}});
        else result.diagnostics.truncated = true;
    };
    if (sources.size() > limits.snapshot_documents) { error({}, "limit_exceeded", ""); return result; }
    for (const auto& source : sources) {
        if (source.bytes.size() > limits.source_bytes || source.bytes.size() > limits.closure_source_bytes || source.bytes.size() > limits.snapshot_source_bytes - source_bytes ||
            source.bytes.size() > limits.retained_logical_bytes - snapshot->logical_bytes) { error(source.key, "limit_exceeded", ""); return result; }
        if ((source.key.space == DefinitionNamespace::Builtin && source.key.contract == 0) ||
            (source.key.space == DefinitionNamespace::External && source.key.contract != 0)) { error(source.key, "invalid_source_key", ""); return result; }
        accounting::Temporary temporary(v2::checked_sum(v2::checked_product(source.bytes.size(), 16), 4096));
        accounting::work(source.bytes.size(), source.bytes.size());
        Preflight preflight(limits);
        if (!Json::sax_parse(source.bytes.begin(), source.bytes.end(), &preflight)) {
            error(source.key, preflight.code.empty() ? "invalid_json" : preflight.code, preflight.pointer);
            return result;
        }
        const auto structural = validate_v2_structure(preflight.header, DefinitionV2SchemaRoot::Envelope, limits);
        if (!structural.entries.empty() || structural.truncated) {
            result.diagnostics = structural;
            for (auto& diagnostic : result.diagnostics.entries) diagnostic.definition = source.key.id;
            return result;
        }
        preflight.header.erase("merge");
        preflight.header.erase("edits");
        if (preflight.header["id"] != source.key.id) { error(source.key, "source_identity_mismatch", "/id"); return result; }
        std::size_t logical = source.bytes.size();
        std::size_t header_bytes = 0;
        const auto measure = [&](const auto& self, const Json& value) -> void {
            header_bytes += sizeof(Json);
            if (value.is_string()) header_bytes += value.get_ref<const std::string&>().size();
            if (value.is_object()) for (const auto& [name, field] : value.items()) { header_bytes += name.size(); self(self, field); }
        };
        measure(measure, preflight.header);
        for (const auto size : {source.key.id.size(), source.key.id.size(), std::size_t(32), header_bytes, sizeof(v2::SourceRecord)}) {
            if (size > limits.retained_logical_bytes - snapshot->logical_bytes - logical) { error(source.key, "limit_exceeded", ""); return result; }
            logical += size;
        }
        const auto key = v2::key_string(source.key);
        if (snapshot->sources.count(key)) { error(source.key, "duplicate_source", "/id"); return result; }
        if (retained) {
            try { accounting::owned(logical); v2::charge(*retained, logical, limits.retained_logical_bytes); }
            catch (const v2::LimitExceeded&) { error(source.key, "limit_exceeded", ""); return result; }
        }
        source_bytes += source.bytes.size();
        snapshot->logical_bytes += logical;
        snapshot->sources.emplace(key, v2::SourceRecord{source.key, std::string(source.bytes), std::move(preflight.header), preflight.nodes});
    }
    result.snapshot = std::move(snapshot);
    return result;
}

}
