#include "backend_definition_v2_internal.h"

#include <algorithm>
#include <limits>

#include <mbedtls/md.h>

namespace lemon::backends::v2 {

thread_local AdmissionCounters admission_counters;

void charge(std::size_t& used, std::size_t increment, std::size_t limit) {
    if (used > limit || increment > limit - used) throw LimitExceeded();
    used += increment;
}

std::size_t checked_sum(std::size_t left, std::size_t right) {
    charge(left, right, std::numeric_limits<std::size_t>::max());
    return left;
}

std::size_t checked_product(std::size_t left, std::size_t right) {
    if (right && left > std::numeric_limits<std::size_t>::max() / right) throw LimitExceeded();
    return left * right;
}

std::size_t string_bytes(std::string_view value) {
    std::size_t result = 2;
    for (unsigned char c : value) {
        charge(result, c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t' ? 2 : c < 0x20 ? 6 : 1,
            std::numeric_limits<std::size_t>::max());
    }
    return result;
}

void Budget::work(std::size_t bytes, std::size_t nodes) {
    charge(metrics.work_bytes, bytes, limits.work_bytes);
    charge(metrics.work_nodes, nodes, limits.work_nodes);
}

void Budget::keep(std::size_t bytes) {
    charge(retained, bytes, limits.retained_logical_bytes);
    metrics.retained_logical_bytes = retained;
}

void Budget::admit(const Metrics& value) {
    if (value.bytes > limits.effective_bytes || value.nodes > limits.effective_nodes || value.depth > limits.effective_depth) throw LimitExceeded();
}

Metrics measure(const Json& value, Budget& budget) {
    budget.work(0, 1);
    Metrics result{0, 1, value.is_structured() ? 1u : 0u};
    if (value.is_structured()) {
        result.bytes = 2;
        for (auto it = value.begin(); it != value.end(); ++it) {
            const auto child = measure(it.value(), budget);
            auto overhead = it == value.begin() ? 0u : 1u;
            result.bytes = checked_sum(result.bytes, overhead);
            if (value.is_object()) result.bytes = checked_sum(result.bytes, checked_sum(string_bytes(it.key()), 1));
            result.bytes = checked_sum(result.bytes, child.bytes);
            result.nodes = checked_sum(result.nodes, child.nodes);
            result.depth = std::max(result.depth, checked_sum(child.depth, 1));
        }
    } else if (value.is_string()) result.bytes = string_bytes(value.get_ref<const std::string&>());
    else result.bytes = value.dump().size();
    budget.work(value.is_structured() ? 2 : result.bytes, 0);
    return result;
}

NodePtr authored(const Json& value, const OriginPtr& origin, Budget& budget) {
    const auto metrics = measure(value, budget);
    budget.admit(metrics);
    std::size_t logical = sizeof(Node);
    if (value.is_object()) for (const auto& [key, child] : value.items()) logical = checked_sum(logical, checked_sum(key.size(), sizeof(NodePtr)));
    else if (value.is_array()) for (std::size_t i = 0; i < value.size(); ++i) logical = checked_sum(logical, sizeof(NodePtr));
    else logical = checked_sum(logical, metrics.bytes);
    budget.keep(logical);
    budget.work(metrics.bytes, 1);
    ++admission_counters.clones;
    auto node = std::make_shared<Node>();
    node->origin = origin;
    node->metrics = metrics;
    if (value.is_object()) {
        node->kind = Node::Kind::Object;
        for (const auto& [key, child] : value.items()) node->object.emplace(key, authored(child, origin_child(origin, key, budget), budget));
    } else if (value.is_array()) {
        node->kind = Node::Kind::Array;
        node->array.reserve(value.size());
        for (std::size_t i = 0; i < value.size(); ++i) node->array.push_back(authored(value[i], origin_child(origin, std::to_string(i), budget), budget));
    } else node->scalar = value;
    return node;
}

Json materialize(const NodePtr& node, Budget& budget) {
    budget.work(node->metrics.bytes, node->metrics.nodes);
    budget.keep(checked_sum(node->metrics.bytes, checked_sum(checked_product(node->metrics.nodes, sizeof(Json)), sizeof(Json))));
    const auto build = [&](const auto& self, const NodePtr& current) -> Json {
        if (current->kind == Node::Kind::Scalar) return current->scalar;
        Json result = current->kind == Node::Kind::Object ? Json::object() : Json::array();
        if (current->kind == Node::Kind::Object) for (const auto& [key, child] : current->object) result[key] = self(self, child);
        else for (const auto& child : current->array) result.push_back(self(self, child));
        return result;
    };
    ++admission_counters.dom;
    return build(build, node);
}

std::string digest(std::string_view domain, const Json& value, Budget& budget) {
    const auto metrics = measure(value, budget);
    budget.work(checked_sum(domain.size(), metrics.bytes), metrics.nodes);
    const auto bytes = value.dump();
    mbedtls_md_context_t context;
    mbedtls_md_init(&context);
    const auto* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    unsigned char hash[32];
    const bool ok = info && mbedtls_md_setup(&context, info, 0) == 0 && mbedtls_md_starts(&context) == 0 &&
        mbedtls_md_update(&context, reinterpret_cast<const unsigned char*>(domain.data()), domain.size()) == 0 &&
        mbedtls_md_update(&context, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()) == 0 &&
        mbedtls_md_finish(&context, hash) == 0;
    mbedtls_md_free(&context);
    if (!ok) throw std::runtime_error("SHA-256 unavailable");
    std::string result = "sha256:";
    for (auto byte : hash) { result += "0123456789abcdef"[byte >> 4]; result += "0123456789abcdef"[byte & 15]; }
    return result;
}

}
