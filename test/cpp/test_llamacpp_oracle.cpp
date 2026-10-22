#include "backends/llamacpp/llamacpp_effects.h"
#include "lemon/backends/llamacpp/llamacpp.h"
#include "lemon/backends/llamacpp/llamacpp_gguf.h"
#include "lemon/backends/llamacpp/llamacpp_server.h"
#include "lemon/model_manager.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/recipe_arg_resolver.h"
#include "llamacpp_oracle_descriptor.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>

using namespace lemon;
using namespace lemon::backends;
namespace fs = std::filesystem;

namespace {
json read(const fs::path& path) {
    std::ifstream input(path);
    return json::parse(input);
}

struct Fixture : llamacpp::oracle::Effects {
    json facts = json::object();
    RuntimeConfig config;
    json events = json::array();
    json command = nullptr;
    json forwarded = nullptr;
    json reply = {{"model", "backend-model"}, {"witness", true}};
    std::map<std::string, std::string> env;
    std::set<std::string> files, regular_files, executables;

    explicit Fixture(const json& input = json::object()) : facts(input), config({{"rocm_channel", input.value("channel", "stable")}}) {
        const json environment_values = input.value("env", json::object());
        for (const auto& [key, value] : environment_values.items()) {
            env[key] = value.get<std::string>();
        }
        for (const auto& value : input.value("exists", json::array())) files.insert(value);
        for (const auto& value : input.value("regular", json::array())) regular_files.insert(value);
        for (const auto& value : input.value("executable", json::array())) executables.insert(value);
        llamacpp::oracle::current_effects = this;
    }
    ~Fixture() override { llamacpp::oracle::current_effects = nullptr; }
    RuntimeConfig* runtime_config() override {
        events.push_back("runtime-config");
        return facts.value("no_config", false) ? nullptr : &config;
    }
    json versions() override {
        events.push_back("versions:resources/backend_versions.json");
        return facts.value("versions", read(fs::path(LEMON_ORACLE_FIXTURES) / "pins.json"));
    }
    std::string rocm_arch() override {
        events.push_back("rocm-arch");
        return facts.value("rocm_arch", "gfx1151");
    }
    std::string cuda_arch() override {
        events.push_back("cuda-arch");
        return facts.value("cuda_arch", "sm_120");
    }
    std::string unsupported(const std::string& backend) override {
        events.push_back("unsupported:" + backend);
        return "injected unsupported backend: " + backend;
    }
    std::string os() override {
        events.push_back("os");
        return LLAMA_WINDOWS ? "windows" : LLAMA_MACOS ? "macos" : "linux";
    }
    SystemInfo::SupportedBackendsResult supported_backends(const std::string& recipe) override {
        events.push_back("supported:" + recipe);
        if (LLAMA_MACOS) return {{"metal"}, ""};
        return {{"vulkan", "rocm", "cuda", "cpu"}, ""};
    }
    void install(const std::string& recipe, const std::string& backend) override {
        events.push_back("install:" + recipe + ":" + backend);
        if (facts.contains("install_error")) throw std::runtime_error(facts["install_error"]);
    }
    int port() override {
        events.push_back("port");
        return 12345;
    }
    std::string path_executable(const std::string& name) override {
        events.push_back("path-executable:" + name);
        return facts.value("path_executable", "/oracle/system/" + name);
    }
    std::string external_binary(const std::string& recipe, const std::string& backend) override {
        events.push_back("external-binary:" + recipe + ":" + backend);
        if (facts.contains("binary_error")) throw std::runtime_error(facts["binary_error"]);
        return facts.value("external_binary", "");
    }
    std::string install_directory(const std::string& recipe, const std::string& backend) override {
        events.push_back("install-directory:" + recipe + ":" + backend);
        return "/oracle/bundle/" + backend;
    }
    std::string installed_binary(const std::string& dir, const std::string& name) override {
        events.push_back("installed-binary:" + dir + ":" + name);
        return facts.value("installed_binary", dir + "/" + name + (LLAMA_WINDOWS ? ".exe" : ""));
    }
    std::vector<std::string> runtime_paths(const std::string& arch) override {
        events.push_back("runtime-paths:" + arch);
        return facts.value("runtime_paths", std::vector<std::string>{"/oracle/therock/core", "/oracle/therock/blas"});
    }
    void stage(const std::string& arch, const fs::path& dir) override {
        events.push_back("stage:" + arch + ":" + dir.generic_string());
    }
    const char* environment(const std::string& name) override {
        events.push_back("env:" + name);
        const auto it = env.find(name);
        return it == env.end() ? nullptr : it->second.c_str();
    }
    std::string cuda_visible_devices(const std::string& arch) override {
        events.push_back("cuda-visible:" + arch);
        return facts.value("cuda_visible_devices", "0,2");
    }
    std::string home() override {
        events.push_back("passwd-home");
        return facts.value("home", "/oracle/passwd/home");
    }
    void start(const ServerCommand& cmd, ProcessOutput output, const std::string& cwd) override {
        events.push_back("start");
        command = {{"program", cmd.program}, {"argv", cmd.args}, {"env", cmd.env},
                   {"port", cmd.port}, {"ready_endpoint", cmd.ready_endpoint}, {"cwd", cwd},
                   {"inherit_output", output.inherit}, {"filter_health_logs", output.filter_health_logs}};
    }
    json post(const std::string& endpoint, const json& request) override {
        events.push_back("POST:" + endpoint);
        forwarded = {{"method", "POST"}, {"path", endpoint}, {"body", request}};
        return reply;
    }
    json get(const std::string& endpoint) override {
        events.push_back("GET:" + endpoint);
        forwarded = {{"method", "GET"}, {"path", endpoint}};
        return reply;
    }
    void stream(const std::string& endpoint, const std::string& body, httplib::DataSink& sink,
                bool sse, long timeout, WrappedServer::TelemetryCallback callback) override {
        events.push_back("stream:" + endpoint);
        forwarded = {{"method", "POST"}, {"path", endpoint}, {"body", body}, {"sse", sse},
                     {"timeout", timeout}, {"telemetry", static_cast<bool>(callback)}};
        const std::string bytes = "data: {\"model\":\"backend-model\"}\n\ndata: [DONE]\n\n";
        sink.write(bytes.data(), bytes.size());
    }
    int version_command(const std::string& command, std::string& output) override {
        events.push_back("probe:" + command);
        output = facts.value("version_output", "");
        return facts.value("probe_status", 0);
    }
    FILE* version_pipe(const std::string& command) override {
        events.push_back("probe:" + command);
        if (facts.value("probe_failure", false)) return nullptr;
        FILE* pipe = std::tmpfile();
        if (!pipe) throw std::runtime_error("oracle version pipe failed");
        const std::string output = facts.value("version_output", "");
        std::fwrite(output.data(), 1, output.size(), pipe);
        std::rewind(pipe);
        return pipe;
    }
    int close_version_pipe(FILE* pipe) override {
        events.push_back("close-version-pipe");
        std::fclose(pipe);
        return facts.value("probe_status", 0);
    }
    bool exists(const fs::path& path) override {
        events.push_back("exists:" + path.generic_string());
        return files.count(path.generic_string());
    }
    bool regular(const fs::path& path) override {
        events.push_back("regular:" + path.generic_string());
        return regular_files.count(path.generic_string());
    }
    bool executable(const fs::path& path) override {
        events.push_back("executable:" + path.generic_string());
        return executables.count(path.generic_string());
    }
    void observe(const std::string& hook) override { events.push_back(hook); }
};

ModelInfo model(const json& input) {
    ModelInfo info;
    info.checkpoints["main"] = input.value("checkpoint", "fixture/repo:Q4_0");
    info.checkpoints["draft"] = input.value("draft_checkpoint", "");
    info.resolved_paths["main"] = input.value("main", "/oracle/models/main.gguf");
    info.resolved_paths["mmproj"] = input.value("mmproj", "");
    info.resolved_paths["draft"] = input.value("draft", "");
    info.labels = input.value("labels", std::vector<std::string>{});
    info.extras["hf_load"] = input.value("hf_load", false);
    const std::string type = input.value("type", "llm");
    info.type = type == "embedding" ? ModelType::EMBEDDING : type == "reranking" ? ModelType::RERANKING : ModelType::LLM;
    return info;
}

json normalize(json value, const std::string& temp = "");

struct TempDirectory {
    fs::path path;
    TempDirectory() {
        char pattern[] = "/tmp/lemonade-llamacpp-oracle-XXXXXX";
        char* result = mkdtemp(pattern);
        if (!result) throw std::runtime_error("oracle temporary directory failed");
        path = result;
    }
    ~TempDirectory() { fs::remove_all(path); }
};

template <typename T>
void write_number(std::ostream& output, T value) {
    for (size_t i = 0; i < sizeof(T); ++i) output.put(static_cast<char>((value >> (8 * i)) & 255));
}

void write_string(std::ostream& output, const std::string& value) {
    write_number<std::uint64_t>(output, value.size());
    output.write(value.data(), value.size());
}

json gguf(const json& input) {
    TempDirectory temp;
    const fs::path cache = temp.path / "cache";
    if (input.value("create_cache", true)) fs::create_directory(cache);
    const json file_values = input.value("files", json::object());
    for (const auto& [name, content] : file_values.items()) {
        const auto path = cache / name;
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        if (content.is_object()) {
            file.write("GGUF", 4);
            write_number<std::uint32_t>(file, 3);
            write_number<std::uint64_t>(file, 0);
            write_number<std::uint64_t>(file, content.size());
            for (const auto& [key, value] : content.items()) {
                write_string(file, key);
                write_number<std::uint32_t>(file, value.is_string() ? 8 : 4);
                if (value.is_string()) write_string(file, value.get<std::string>());
                else write_number<std::uint32_t>(file, value.get<std::uint32_t>());
            }
        } else file << content.get<std::string>();
    }
    json result;
    const std::string variant = input.value("variant", "");
    result["resolved"] = llamacpp::resolve_gguf_path(cache.string(), variant);
    result["imported"] = llamacpp::ops()->find_imported_checkpoint(cache.string());
    CheckpointResolveContext context;
    context.model_cache_path = cache.string();
    context.variant = variant;
    context.type = "main";
    auto info = model(input.value("model", json::object()));
    result["checkpoint"] = llamacpp::ops()->resolve_checkpoint_path(info, context);
    result["files"] = json::object();
    for (const auto& [name, content] : file_values.items()) {
        (void)content;
        result["files"][name] = llamacpp::ops()->validate_checkpoint_file((cache / name).string());
    }
    result["directory"] = llamacpp::ops()->validate_checkpoint_file(cache.string());
    result["registration"] = llamacpp::ops()->validate_registration_checkpoint(input.value("registration", "repo/model-GGUF"));
    info.resolved_paths["main"] = (cache / input.value("metadata_file", "missing.gguf")).string();
    llamacpp::ops()->populate_metadata(info, {});
    result["metadata"] = {{"architecture", info.gguf.architecture}, {"context", info.max_context_window},
        {"vision", info.gguf.caps.vision}, {"tool_calling", info.gguf.caps.tool_calling}, {"mtp", info.gguf.caps.mtp}, {"labels", info.labels}};
    return normalize(result, temp.path.string());
}

json run(const json& input) {
    Fixture fixture(input.value("facts", json::object()));
    json result = json::object();
    const std::string kind = input["kind"];
    try {
        if (kind == "load" || kind == "runtime") {
            RecipeOptions options("llamacpp", {{"ctx_size", 4096}, {"llamacpp_backend", LLAMA_MACOS ? "metal" : "cpu"}});
            options.set_option("llamacpp_device", "");
            options.set_option("llamacpp_args", "");
            const json option_values = input.value("options", json::object());
            for (const auto& [key, value] : option_values.items()) options.set_option(key, value);
            const auto info = model(input.value("model", json::object()));
            LlamaCppServer server(input.value("level", "error"), nullptr, nullptr);
            server.set_model_metadata("fixture", info.checkpoint(), info.type, DEVICE_NPU, options);
            result["requested_options"] = options.to_json();
            llamacpp::ops()->resolve_runtime_options(info, options);
            result["resolved_options"] = options.to_json();
            if (kind == "load") {
                try { server.load("fixture", info, options, false); }
                catch (...) {
                    result["device"] = static_cast<int>(server.get_device_type());
                    throw;
                }
                result["device"] = static_cast<int>(server.get_device_type());
                result["options_after_load"] = options.to_json();
            }
        } else if (kind == "install") {
            const auto* spec = llamacpp::spec();
            const auto phase = input["phase"] == "repository" ? InstallPhase::Repository : InstallPhase::Asset;
            const InstallContext context{*spec, nullptr, "llamacpp", "llamacpp", input["backend"], "", "b1111", "b2222",
                {LLAMA_WINDOWS ? "windows" : LLAMA_MACOS ? "macos" : "linux", LLAMA_ARM64 ? "aarch64" : "x86_64", ""}, phase};
            const auto params = spec->install_params_fn(context);
            result["params"] = {{"repo", params.repo}, {"filename", params.filename},
                                {"version_override", params.version_override}, {"sha256", params.sha256}};
            result["spec"] = {{"recipe", spec->recipe}, {"binary", spec->binary}, {"pin_recipe", spec->pin_recipe},
                              {"public_recipe", spec->public_recipe}, {"split_archive", spec->supports_split_archive}};
        } else if (kind == "gguf") {
            result["gguf"] = gguf(input);
        } else if (kind == "scoped") {
            utils::ScopedCustomArgs args;
            args.backend = input.value("backend_args", "--threads 2 --batch 16");
            args.architecture = input.value("arch_args", "--threads 3 --ubatch 8");
            args.model_defaults = input.value("model_defaults", "--threads 4");
            args.model = input.value("model_args", "--threads 5");
            args.request = input.value("request_args", "--threads 6");
            const std::string state = input.value("request_state", "omitted");
            args.request_state = state == "value" ? utils::CustomArgsRequestState::Value : state == "tombstone" ? utils::CustomArgsRequestState::Tombstone : utils::CustomArgsRequestState::Omitted;
            args.merge_args = input.value("merge_args", true);
            result["args"] = utils::resolve_scoped_custom_args(args);
        } else if (kind == "system-version") {
            result["system"] = llamacpp::ops()->resolve_version("system", "file-version");
            result["managed"] = llamacpp::ops()->resolve_version("cpu", "file-version");
        } else if (kind == "system-install") {
            const auto checked = llamacpp::ops()->check_install("system", input["binary_found"]);
            result["installed"] = checked.installed;
            result["message"] = checked.error;
        } else {
            throw std::logic_error("unknown oracle case kind");
        }
        result["error"] = "";
    } catch (const std::invalid_argument& error) {
        result["error"] = error.what();
        result["exception"] = "invalid_argument";
    } catch (const std::exception& error) {
        result["error"] = error.what();
        result["exception"] = "exception";
    }
    result["events"] = fixture.events;
    result["command"] = fixture.command;
    return result;
}


json protocol() {
    Fixture fixture;
    LlamaCppServer server("error", nullptr, nullptr);
    RecipeOptions options("llamacpp", {});
    server.set_model_metadata("loaded-model", "fixture/repo", ModelType::LLM, DEVICE_NPU, options);
    json output = json::object();
    json cells = json::array();
    for (const auto& entry : server.operation_bindings()->descriptions()) {
        cells.push_back({{"operation", static_cast<int>(entry.operation)}, {"delivery", static_cast<int>(entry.delivery)},
                         {"models", entry.model_types}, {"provenance", static_cast<int>(entry.provenance)}});
    }
    output["bindings"] = cells;
    json matrix = json::array();
    for (size_t op = 0; op < k_backend_operation_count; ++op) {
        for (size_t delivery = 0; delivery < k_operation_delivery_count; ++delivery) {
            for (const auto mode : k_operation_model_types) {
                matrix.push_back(server.supports_operation(static_cast<BackendOperation>(op), static_cast<OperationDelivery>(delivery), mode));
            }
        }
    }
    output["matrix"] = matrix;
    const json schema = {{"type", "object"}, {"maxLength", 2000}, {"minLength", 1999}, {"maxItems", 2001},
        {"minItems", 2000}, {"properties", {{"x", {{"maxLength", 2000.0}, {"minItems", 2000.5}}}}},
        {"description", "maxLength 9999"}};
    const json base = {{"model", "client-model"}, {"max_completion_tokens", 7},
        {"tools", {{{"function", {{"parameters", schema}}}}, {{"parameters", schema}}}}};
    for (int op : {0, 1, 2, 3, 4, 15, 16, 17}) {
        for (int variant = 0; variant < 4; ++variant) {
            fixture.events = json::array();
            fixture.forwarded = nullptr;
            fixture.reply = variant == 3 ? json::array({1}) : json{{"model", "backend-model"}, {"witness", true}};
            json request = base;
            if (variant == 1) request.erase("model");
            if (variant == 2) request["max_tokens"] = 11;
            json response;
            if (op <= 2) response = server.dispatch_completion_operation(static_cast<BackendOperation>(op), request);
            else if (op == 3) response = server.embeddings(request);
            else if (op == 4) response = server.reranking(request);
            else if (op == 15) response = server.get_slots();
            else if (op == 16) response = server.slots_action(2, "save", request);
            else response = server.tokenize(request);
            output["buffered"][std::to_string(op) + ":" + std::to_string(variant)] = {
                {"input", request}, {"forwarded", fixture.forwarded}, {"response", response}, {"events", fixture.events}};
        }
    }
    for (const std::string endpoint : {"/v1/chat/completions", "/v1/completions", "/v1/responses", "/unknown"}) {
        std::vector<std::string> inputs = {base.dump(), "{ \"max_completion_tokens\" : 7, \"stream\" : true }", "{broken", "[1]", "null"};
        for (size_t index = 0; index < inputs.size(); ++index) {
            fixture.events = json::array();
            std::string bytes;
            httplib::DataSink sink;
            sink.write = [&](const char* data, size_t size) { bytes.append(data, size); return true; };
            sink.is_writable = [] { return true; };
            sink.done = [] {};
            for (const bool sse : {false, true}) {
                fixture.events = json::array();
                bytes.clear();
                WrappedServer::TelemetryCallback callback = sse ? WrappedServer::TelemetryCallback([](const auto&) {}) : nullptr;
                server.forward_streaming_request(endpoint, inputs[index], sink, sse, sse ? 0 : 13, callback);
                output["streaming"][endpoint + ":" + std::to_string(index) + (sse ? ":sse" : ":bytes")] = {
                    {"input", inputs[index]}, {"forwarded", fixture.forwarded}, {"bytes", bytes}, {"events", fixture.events}};
            }
        }
    }
    return output;
}

json normalize(json value, const std::string& temp) {
    if (value.is_string()) {
        std::string text = value.get<std::string>();
        for (const auto& pair : std::vector<std::pair<std::string, std::string>>{{"/oracle", "<ROOT>"}, {temp, "<TEMP>"}}) {
            if (pair.first.empty()) continue;
            size_t pos = 0;
            while ((pos = text.find(pair.first, pos)) != std::string::npos) {
                text.replace(pos, pair.first.size(), pair.second);
                pos += pair.second.size();
            }
        }
        return text;
    }
    if (value.is_structured()) for (auto& entry : value) entry = normalize(std::move(entry), temp);
    return value;
}
}

int main(int argc, char** argv) {
    try {
        AixLog::Log::init({});
        json output = {{"descriptor", dump_llamacpp_descriptor(llamacpp::descriptor)}, {"protocol", protocol()}, {"cases", json::object()}};
        for (const auto& input : read(fs::path(LEMON_ORACLE_FIXTURES) / "cases.json")) {
            const std::string id = input["id"];
            if (output["cases"].contains(id)) throw std::logic_error("duplicate oracle case: " + id);
            output["cases"][id] = run(input);
        }
        output = normalize(std::move(output));
        if (argc == 3 && std::string(argv[1]) == "--capture") {
            std::ofstream(argv[2]) << output.dump(2) << '\n';
            return 0;
        }
        const auto expected = read(fs::path(LEMON_ORACLE_FIXTURES) / (std::string(LEMON_ORACLE_VIEW) + ".json"));
        const auto difference = json::diff(expected, output);
        if (!difference.empty()) {
            std::cerr << difference.dump(2).substr(0, 12000) << '\n';
            return 1;
        }
        std::cout << output["cases"].size() << " cases; descriptor; 11 operation cells; 32 buffered and 40 streaming transcripts match " << LEMON_ORACLE_VIEW << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
