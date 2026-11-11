#include "lemon/backends/llamacpp/llamacpp_decision.h"
#include "lemon/models/model_info.h"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using namespace lemon;
using namespace lemon::backends::llamacpp;
using Json = nlohmann::json;
using Ordered = nlohmann::ordered_json;

namespace {
int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

struct Wire {
    std::vector<std::string> endpoints;
    Ordered answer = Ordered::object();
    DecisionTransport transport() {
        return {[this](const std::string& endpoint, const Json&) { endpoints.push_back(endpoint); return Json(answer); },
            [this](const std::string& endpoint, const Ordered&) { endpoints.push_back(endpoint); return answer; }};
    }
};

ModelInfo model(std::vector<std::string> labels) {
    ModelInfo info;
    info.model_name = "user.Decision";
    info.labels = std::move(labels);
    return info;
}

std::vector<std::string> keys(const Ordered& value) {
    std::vector<std::string> result;
    for (const auto& item : value.items()) result.push_back(item.key());
    return result;
}
}

int main() {
    {
        Wire wire;
        DecisionRuntime chat("user.Chat", model({"chat"}));
        check(chat.classify(wire.transport(), Json{{"input", "text"}, {"labels", {"a", "b"}}}).contains("error") &&
            chat.systemone(wire.transport(), Ordered{{"state", "text"}}).contains("error") && wire.endpoints.empty(),
            "a model without a decision label is refused before any request reaches llama-server");
    }
    {
        Wire wire;
        wire.answer = Ordered::parse(R"json({"model":"/models/decision.gguf","zeta":1,"alpha":2})json");
        DecisionRuntime decision("user.Decision", model({"classification", "systemone"}));
        const auto response = decision.systemone(wire.transport(), Ordered{{"state", "text"}});
        check(wire.endpoints == std::vector<std::string>{"/v1/systemone"}, "systemone posts to /v1/systemone once");
        check(keys(response) == std::vector<std::string>{"model", "zeta", "alpha"}, "the answer keeps llama-server's key order");
        check(response.at("model") == "user.Decision", "the answer names the client-facing model");
        const auto named = decision.systemone(wire.transport(), Ordered{{"model", "alias"}, {"state", "text"}});
        check(named.at("model") == "alias", "a request that names its model gets that name back");
    }
    {
        Wire wire;
        DecisionRuntime decision("user.Decision", model({"classification", "systemone"}));
        check(decision.classify(wire.transport(), Json{{"labels", {"a", "b"}}}).contains("error") &&
            decision.classify(wire.transport(), Json{{"input", "text"}}).contains("error") && wire.endpoints.empty(),
            "classify refuses a request without input or labels before any request reaches llama-server");
        wire.answer = Ordered{{"error", {{"message", "upstream failure"}}}};
        const auto failed = decision.classify(wire.transport(), Json{{"input", "text"}, {"labels", {"a", "b"}}});
        check(failed.contains("error") && wire.endpoints == std::vector<std::string>{"/v1/systemone"},
            "classify forwards one decision request and returns llama-server's error unchanged");
    }
    {
        check(!plan_decision_launch("user.Chat", model({"chat"}), 4096, "").has_value() &&
            !plan_decision_launch("user.Embed", model({"embeddings"}), 8192, "-b 64").has_value(),
            "a model that is not a decision model gets no launch changes");
        auto router = model({"classification", "zero-shot"});
        router.checkpoints["rulehead"] = "org/router:rulehead.bin";
        bool refused = false;
        try { plan_decision_launch("user.Router", router, 4096, ""); }
        catch (const std::runtime_error&) { refused = true; }
        check(refused, "a router whose rule head is not on disk is refused before launch");
        bool unread = false;
        try { DecisionRuntime missing("user.Router", router); }
        catch (const std::runtime_error&) { unread = true; }
        check(unread, "a router whose rule head is not on disk has no decision runtime");
    }
    if (failures) std::printf("FAILED: %d\n", failures);
    return failures ? 1 : 0;
}
