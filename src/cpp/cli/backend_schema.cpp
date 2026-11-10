#include "lemon/backends/backend_option_contract.h"
#include "lemon/backends/configured/backend_definition_store.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/url_utils.h"
#include "lemon_cli/backend_schema.h"

#include <cmath>
#include <fstream>
#include <iostream>

namespace lemon_cli {
using nlohmann::json;
namespace {
struct FiniteJson : nlohmann::json_sax<json> {
    bool null() override { return true; }
    bool boolean(bool) override { return true; }
    bool number_integer(number_integer_t) override { return true; }
    bool number_unsigned(number_unsigned_t) override { return true; }
    bool number_float(number_float_t value, const string_t& token) override {
        return std::isfinite(value) && token.find_first_of(".eE") != std::string::npos;
    }
    bool string(string_t&) override { return true; }
    bool binary(binary_t&) override { return true; }
    bool start_object(std::size_t) override { return true; }
    bool key(string_t&) override { return true; }
    bool end_object() override { return true; }
    bool start_array(std::size_t) override { return true; }
    bool end_array() override { return true; }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }
};

void require_schema(const json& schema) {
    for (const auto* key : {"schema_version", "control_version", "definition_schema_version"}) {
        if (!schema.contains(key) || !schema[key].is_number_integer() || schema[key] != 1) throw std::runtime_error("Incompatible backend schema/control version");
    }
    if (!schema.contains("recipes") || !schema["recipes"].is_array() || !schema.contains("catalog_id") || !schema["catalog_id"].is_string()) throw std::runtime_error("Incompatible backend schema response");
}
int report_failure(const std::exception& error) {
    const auto* http = dynamic_cast<const lemonade::HttpError*>(&error);
    if (http && (http->status_code() == 401 || http->status_code() == 403)) std::cerr << "Authentication failure: " << lemonade::extract_server_error_message(*http) << '\n';
    else if (http && http->status_code() == 404) {
        const auto body = json::parse(http->response_body(), nullptr, false);
        if (body.is_object() && body.contains("error") && body["error"].is_object() && body["error"].contains("code") && body["error"]["code"] == "unknown_recipe") std::cerr << body.dump() << '\n';
        else std::cerr << "Server schema support required; backend schema/validation endpoint unavailable\n";
    } else std::cerr << "Backend schema request failed: " << error.what() << '\n';
    return 1;
}
json validation_result(const std::string& bytes) {
    const auto result = lemon::backends::BackendDefinitionStore().validate({"", bytes, lemon::backends::DefinitionOrigin::External});
    json output = {{"valid", result.definition != nullptr}, {"validation", "offline"},
        {"feature_manifest_id", lemon::backends::schema_digest(lemon::backends::installed_definition_features().dump())}, {"diagnostics", json::array()}};
    if (result.definition) {
        output["definition_id"] = result.definition->id;
        output["recipe"] = result.definition->recipe;
        output["definition_hash"] = result.definition->digest;
    }
    for (const auto& error : result.diagnostics) output["diagnostics"].push_back({{"code", error.code}, {"definition", error.definition}, {"pointer", error.pointer}, {"message", error.message}});
    return output;
}
int print_validation(const json& result) {
    if (!result.is_object() || !result.contains("valid") || !result["valid"].is_boolean() || result.value("validation", "") != "server" || !result.contains("diagnostics") || !result["diagnostics"].is_array()) throw std::runtime_error("Incompatible backend validation response");
    std::cout << result.dump(2) << '\n';
    return result["valid"].get<bool>() ? 0 : 2;
}
}

json parse_generic_options(const std::vector<std::string>& assignments, const std::set<std::string>& explicit_names) {
    json result = json::object();
    for (const auto& assignment : assignments) {
        const auto equals = assignment.find('=');
        if (equals == std::string::npos || equals == 0) throw std::invalid_argument("Expected --option KEY=JSON_VALUE with a nonempty exact key");
        const auto key = assignment.substr(0, equals);
        if (result.contains(key)) throw std::invalid_argument("Duplicate generic option: " + key);
        if (explicit_names.count(key)) throw std::invalid_argument("Generic option also explicitly assigned by named flag: " + key);
        const auto value = assignment.substr(equals + 1);
        FiniteJson finite;
        if (!json::sax_parse(value, &finite)) throw std::invalid_argument("Invalid, nonfinite or overflowing JSON value for option: " + key);
        result[key] = json::parse(value);
    }
    return result;
}

std::string read_backend_definition_file(const std::string& path) {
    std::ifstream input(lemon::utils::path_from_utf8(path), std::ios::binary);
    if (!input) throw std::invalid_argument("Cannot read backend definition file");
    std::string bytes(1024 * 1024 + 1, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<size_t>(input.gcount()));
    if (input.bad() || bytes.size() > 1024 * 1024) throw std::invalid_argument("Backend definition cannot be read or exceeds 1 MiB");
    return bytes;
}
int validate_backend_definition_offline(const std::string& bytes) {
    std::cerr << "Offline validation only; server catalog collisions and executable bindings\n"
                 "have not been validated by the connected server.\n";
    const auto result = validation_result(bytes);
    std::cout << result.dump(2) << '\n';
    return result["valid"].get<bool>() ? 0 : 2;
}
int validate_backend_definition(lemonade::LemonadeClient& client, const std::string& bytes) {
    try { return print_validation(json::parse(client.make_request("/api/v1/backends/validate", "POST", bytes, "application/json"))); }
    catch (const lemonade::HttpError& error) {
        if (error.status_code() == 400 || error.status_code() == 413) {
            try { return print_validation(json::parse(error.response_body())); } catch (const std::exception& invalid) { return report_failure(invalid); }
        }
        return report_failure(error);
    } catch (const std::exception& error) { return report_failure(error); }
}
int print_backend_schema(lemonade::LemonadeClient& client, const std::optional<std::string>& recipe) {
    try {
        const auto schema = json::parse(client.make_request("/api/v1/backends/schema" + (recipe ? "?recipe=" + lemon::utils::url_encode(*recipe) : "")));
        require_schema(schema);
        std::cout << schema.dump(2) << '\n';
        return 0;
    } catch (const std::exception& error) { return report_failure(error); }
}
int preflight_generic_options(lemonade::LemonadeClient& client, const std::string& model, const json& options) {
    if (options.empty()) return 0;
    try {
        const auto info = json::parse(client.make_request("/api/v1/models/" + lemon::utils::url_encode(model)));
        if (!info.contains("recipe") || !info["recipe"].is_string() || info["recipe"].get<std::string>().empty()) throw std::runtime_error("Server model information has no recipe");
        const auto recipe = info["recipe"].get<std::string>();
        const auto schema = json::parse(client.make_request("/api/v1/backends/schema?recipe=" + lemon::utils::url_encode(recipe)));
        require_schema(schema);
        if (schema["recipes"].size() != 1 || schema["recipes"][0].value("recipe", "") != recipe || !schema["recipes"][0].contains("options") || !schema["recipes"][0]["options"].is_array()) throw std::runtime_error("Incompatible recipe option contract");
        std::map<std::string, json> contracts;
        for (const auto& contract : schema["recipes"][0]["options"]) {
            const auto type = contract.at("value_type").get<std::string>();
            if (type != "integer" && type != "number" && type != "string" && type != "boolean") throw std::runtime_error("Incompatible backend option type");
            for (const auto* bound : {"minimum", "maximum"}) if (contract.contains(bound) && !contract[bound].is_number()) throw std::runtime_error("Incompatible option bound");
            for (const auto* list : {"enum", "unset_values"}) if (contract.contains(list) && !contract[list].is_array()) throw std::runtime_error("Incompatible option value list");
            if (contract.contains("minimum") && contract.contains("maximum") && contract["minimum"] > contract["maximum"]) throw std::runtime_error("Incompatible option bounds");
            if (contract.contains("default_value") && !lemon::backends::validate_option_contract(contract, contract["default_value"]).empty()) throw std::runtime_error("Incompatible option default");
            if (!contract.contains("default_value") || !contracts.emplace(contract.at("name").get<std::string>(), contract).second) throw std::runtime_error("Incompatible backend option contract");
        }
        for (const auto& [key, value] : options.items()) {
            const auto found = contracts.find(key);
            const auto error = found == contracts.end() ? "Unknown option for server recipe" : value.is_null() ? "" : lemon::backends::validate_option_contract(found->second, value);
            if (!error.empty()) { std::cerr << "Invalid option " << key << ": " << error << '\n'; return 2; }
        }
        return 0;
    } catch (const std::exception& error) { return report_failure(error); }
}

}
