#pragma once

#include "configured_fixture.h"
#include "lemon/backends/configured/configured_server.h"
#include "lemon/config_file.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"

#include <fstream>
#include <future>
#include <new>

namespace llm_oracle {
using namespace thinksound_oracle;
using namespace lemon::backends;

inline thread_local bool track_allocations = false;
inline thread_local size_t allocations = 0;

struct Fixture : EffectsFixture {
    std::string executable = "/bundle/ds4-server";
    std::set<std::string> paths{u8"/models/Deep Seek/雪.gguf"};
    bool install_fails = false, lookup_fails = false;
    std::function<void()> on_install;
    explicit Fixture(int port = 8123) : EffectsFixture(port) {}
    BackendEffects effects() {
        auto value = EffectsFixture::effects();
        value.exists = [this](const auto& path) { events.push_back("exists:" + path); return paths.count(path) != 0; };
        value.binary.install = [this](const auto& recipe, const auto& backend) {
            events.push_back("install:" + recipe + ":" + backend);
            if (on_install) on_install();
            if (install_fails) throw std::logic_error("install witness");
        };
        value.binary.managed_path = [this](const auto& spec, const auto& backend) {
            events.push_back("managed:" + spec.recipe + ":" + backend);
            if (lookup_fails) throw std::logic_error("lookup witness");
            return executable;
        };
        value.inherited_posix_loader = [this] { events.push_back("posix-loader"); return inherited; };
        return value;
    }
};
class Services : public EffectLaunchServices {
public:
    Services(Fixture& fixture, bool posix = false) : EffectLaunchServices(fixture.effects(), posix) {}
    PlatformInfo host{"linux", "x86_64", ""};
    PlatformInfo platform() const override { return host; }
};
inline ModelInfo ds4_model() {
    ModelInfo model;
    model.checkpoints["main"] = "fixture/repo";
    model.resolved_paths["main"] = u8"/models/Deep Seek/雪.gguf";
    return model;
}
inline std::shared_ptr<const ConfiguredBackendEntry> definition_entry(const std::string& text, BuiltinLaunchCompatibility compatibility = {}) {
    auto result = BackendDefinitionStore().validate({"oracle.json", text, DefinitionOrigin::Builtin});
    for (const auto& diagnostic : result.diagnostics) std::printf("%s %s\n", diagnostic.code.c_str(), diagnostic.pointer.c_str());
    if (!result.definition) throw std::runtime_error("Oracle definition validation failed");
    return ConfiguredBackendEntry::make(result.definition, TrustedCompatibility::None, compatibility);
}
class Access : public WrappedServer {
public:
    Access() : WrappedServer("access", "error") {}
    void load(const std::string&, const ModelInfo&, const RecipeOptions&, bool) override {}
    static void locks(WrappedServer& server, const std::function<void()>& body) {
        std::scoped_lock lock(server.*(&Access::state_mutex_), server.*(&Access::process_mutex_)); body();
    }
    static void start(WrappedServer& server, std::unique_ptr<ServerProcess> process, const ServerCommand& command, long timeout) {
        (server.*(&Access::start_server))(std::move(process), command, timeout);
    }
};
class Http {
public:
    httplib::Server http;
    int port;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable condition;
    std::string body, path, reply = R"({"choices":[],"usage":{"prompt_tokens":3,"completion_tokens":2}})";
    std::string stream = "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\ndata: {\"choices\":[],\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":2,\"total_tokens\":5}}\n\ndata: [DONE]\n\n";
    std::atomic<int> health{200}, models{200}, health_calls{0}, model_calls{0};
    int status = 200, disconnect = -1;
    bool hold = false, entered = false, released = false;
    std::function<void()> on_disconnect;
    Http() {
        http.Get("/health", [this](const auto&, auto& response) { ++health_calls; response.status = health; response.set_content("healthy", "text/plain"); });
        http.Get("/v1/models", [this](const auto&, auto& response) { ++model_calls; response.status = models; response.set_content("ready, no JSON", "text/plain"); });
        for (const char* endpoint : {"/v1/chat/completions", "/v1/completions", "/v1/responses"}) http.Post(endpoint, [this](const auto& request, auto& response) {
            std::unique_lock<std::mutex> lock(mutex);
            body = request.body; path = request.path; entered = true; condition.notify_all();
            if (hold) { condition.wait(lock, [&] { return released; }); hold = false; }
            response.status = status;
            if (disconnect >= 0) {
                const auto callback = on_disconnect;
                const auto mode = disconnect;
                response.set_content_provider(10000, "text/event-stream", [mode, callback](size_t offset, size_t, auto& sink) {
                    if (mode && offset == 0) { sink.write("data: {\"partial\":true}\n\n", 24); return true; }
                    if (callback) callback();
                    return false;
                });
            } else if (request.body.find("stream") != std::string::npos || request.body == "malformed") {
                const auto bytes = stream;
                response.set_content_provider(bytes.size(), "text/event-stream", [bytes](size_t offset, size_t length, auto& sink) {
                    const size_t marker = bytes.find("data: [DONE]");
                    const size_t count = offset < marker ? std::min<size_t>({length, 7, marker - offset}) : bytes.size() - offset;
                    return sink.write(bytes.data() + offset, count);
                });
            } else response.set_content(reply, "application/json");
        });
        port = http.bind_to_any_port("127.0.0.1");
        if (port <= 0) throw std::runtime_error("Fake localhost bind failed");
        thread = std::thread([this] { http.listen_after_bind(); }); http.wait_until_ready();
    }
    ~Http() { unblock(); http.stop(); thread.join(); }
    void delay() { std::lock_guard<std::mutex> lock(mutex); hold = true; entered = false; released = false; }
    bool wait() { std::unique_lock<std::mutex> lock(mutex); return condition.wait_for(lock, std::chrono::seconds(3), [&] { return entered; }); }
    void unblock() { std::lock_guard<std::mutex> lock(mutex); released = true; condition.notify_all(); }
};
inline void bindings(WrappedServer& server, Fixture& fixture) {
    const auto& descriptions = server.operation_bindings()->descriptions();
    check(descriptions.size() == 6, "six completion delivery descriptions");
    for (size_t i = 0; i < descriptions.size(); ++i) check(static_cast<size_t>(descriptions[i].operation) == i % 3 &&
        static_cast<size_t>(descriptions[i].delivery) == i / 3 && descriptions[i].model_types == 511 &&
        descriptions[i].provenance == OperationProvenance::Implemented, "literal binding order mask provenance");
    const auto events = fixture.events;
    bool correct = true, completed = false;
    size_t count = 1;
    std::thread reader;
    std::mutex mutex;
    std::condition_variable finished;
    Access::locks(server, [&] {
        reader = std::thread([&] {
            allocations = 0; track_allocations = true;
            for (size_t op = 0; op < k_backend_operation_count; ++op) for (size_t delivery = 0; delivery < k_operation_delivery_count; ++delivery)
                for (auto mode : k_operation_model_types) correct &= server.supports_operation(static_cast<BackendOperation>(op), static_cast<OperationDelivery>(delivery), mode) == (op < 3 && delivery < 2);
            track_allocations = false; count = allocations;
            std::lock_guard<std::mutex> lock(mutex); completed = true; finished.notify_one();
        });
        std::unique_lock<std::mutex> lock(mutex);
        check(finished.wait_for(lock, std::chrono::seconds(3), [&] { return completed; }), "support queries complete with state/process locks held");
    });
    reader.join();
    check(correct && count == 0 && fixture.events == events, "all nine modes full matrix inert allocation-free support");
}
inline void descriptor(const BackendDescriptor& value, const BackendDescriptor& compiled, bool gated = false) {
    check(value.recipe == compiled.recipe && value.display_name == compiled.display_name && value.binary == compiled.binary && value.config_section == compiled.config_section, "compiled descriptor identities");
    check(value.default_device == compiled.default_device && value.slot_policy == compiled.slot_policy && value.selectable_backend == compiled.selectable_backend && value.uses_ctx_size == compiled.uses_ctx_size && value.dynamic_models == compiled.dynamic_models, "compiled descriptor policy");
    check(value.supported_modes == compiled.supported_modes && value.required_checkpoints == compiled.required_checkpoints && value.default_capabilities == compiled.default_capabilities && value.web_display_name == compiled.web_display_name && value.rocm_channels == compiled.rocm_channels, "compiled descriptor lists");
    check(value.exposes_prometheus_metrics == compiled.exposes_prometheus_metrics && value.rocm_requires_cwsr_fix == compiled.rocm_requires_cwsr_fix && value.version_policy == compiled.version_policy && value.self_manages_downloads == compiled.self_manages_downloads && value.takes_args == compiled.takes_args && value.arg_variants == compiled.arg_variants && value.bin_variants == compiled.bin_variants && value.config_extra == compiled.config_extra && value.streams_model_from_storage == compiled.streams_model_from_storage, "compiled descriptor flags");
    check(value.config_defaults() == compiled.config_defaults() && value.options.size() == compiled.options.size() && value.support.size() == compiled.support.size(), "compiled descriptor defaults/options/support counts");
    for (size_t i = 0; i < value.options.size(); ++i) {
        const auto& a = value.options[i]; const auto& b = compiled.options[i];
        check(a.name == b.name && a.cli_flag == b.cli_flag && a.default_value == b.default_value && a.type_name == b.type_name && a.help == b.help && a.group == b.group, "every compiled option field");
    }
    for (size_t i = 0; i < value.support.size(); ++i) {
        const auto& a = value.support[i]; const auto& b = compiled.support[i];
        bool gates = a.arch_gates.size() == b.arch_gates.size() && (gated ? !a.arch_gates.empty() : a.arch_gates.empty());
        for (const auto& [arch, gate] : a.arch_gates) gates = gates && b.arch_gates.count(arch) && b.arch_gates.at(arch).os == gate.os && b.arch_gates.at(arch).channels == gate.channels;
        check(a.backend == b.backend && a.tier == b.tier && a.format == b.format && a.supported_os == b.supported_os && a.devices == b.devices && a.device_summary == b.device_summary && gates, "every compiled support field");
    }
}
}
