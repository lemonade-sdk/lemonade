#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

#include <curl/curl.h>
#include <httplib.h>

#include <lemon/backends/cloud/cloud_server.h>
#include <lemon/cloud_provider_registry.h>

using lemon::json;
using lemon::backends::CloudServer;

int main() {
    int failed = 0;
    auto check = [&](bool ok, const char* name) {
        printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failed;
    };
    curl_global_init(CURL_GLOBAL_DEFAULT);
    httplib::Server mock;
    const int port = mock.bind_to_any_port("127.0.0.1");
    if (port <= 0) return 1;
    const std::string base = "http://127.0.0.1:" + std::to_string(port) + "/v1";
    std::atomic<int> image_requests{0};
    std::atomic<int> chat_requests{0};
    const json catalog = json::array({
        {{"id", "Flux-2-Klein-9B-GGUF"}},
        {{"id", "SD-Turbo"}},
        {{"id", "plain-chat"}},
        {{"id", "named-image"}, {"type", "image"}},
        {{"id", "image-endpoint"}, {"capabilities", {"images.generations"}}},
        {{"id", "image-kind"}, {"type", "image"}, {"kind", "MODEL"}},
        {{"id", "image-architecture"}, {"type", "image"},
         {"architecture", {{"modality", "text->image"}}}},
        {{"id", "endpoint-kind"}, {"capabilities", {"images.generations"}}, {"kind", "MODEL"}},
        {{"id", "endpoint-architecture"}, {"capabilities", {"images.generations"}},
         {"architecture", {{"family", "diffusion"}}}},
        {{"id", "Flux-extra-kind"}, {"kind", "MODEL"}},
        {{"id", "Flux-extra-architecture"}, {"architecture", {{"family", "diffusion"}}}},
        {{"id", "image-and-chat"}, {"type", "image"}, {"supports_chat", true}},
        {{"id", "Flux-chat-shaped"}, {"architecture", {{"modality", "text->image"}}}},
        {{"id", "Flux-chat"}, {"type", "chat"}},
        {{"id", "Flux-vision"}, {"supports_chat", true}},
        {{"id", "Flux-edit"}, {"kind", "FLUMINA_BASE_MODEL"}, {"supports_chat", true}},
        {{"id", "multimodal"}, {"architecture", {{"modality", "text->image"}}}},
        {{"id", "vision-chat"}, {"architecture", {{"modality", "text+image->text"}}}},
        {{"id", "Sora-video"}},
        {{"id", "BGE-Embedding"}},
        {{"id", 42}}
    });
    mock.Get("/v1/models", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.get_header_value("Authorization") != "Bearer test-cloud-image-key") {
            res.status = 401;
            return;
        }
        res.set_content(json({{"data", catalog}}).dump(), "application/json");
    });
    mock.Post("/v1/images/generations", [&](const httplib::Request& req, httplib::Response& res) {
        ++image_requests;
        const auto body = json::parse(req.body);
        if (req.get_header_value("Authorization") != "Bearer test-cloud-image-key") {
            res.status = 401;
            return;
        }
        if (body.value("prompt", "") == "fail") {
            res.status = 429;
            res.set_content(R"({"error":{"message":"rate limited"}})", "application/json");
            return;
        }
        res.set_content(json({{"data", {{{"b64_json", "test-image"}}}}, {"received", body}}).dump(),
                        "application/json");
    });
    mock.Post("/v1/chat/completions", [&](const httplib::Request& req, httplib::Response& res) {
        ++chat_requests;
        res.set_content(json({{"received", json::parse(req.body)}}).dump(), "application/json");
    });
    std::thread listener([&] { mock.listen_after_bind(); });
    mock.wait_until_ready();

    const auto models = CloudServer::discover_models("image-test", "test-cloud-image-key", base, true);
    auto find = [&](const std::string& id) {
        return std::find_if(models.begin(), models.end(), [&](const auto& m) {
            return m.checkpoint() == id;
        });
    };
    check(models.size() == 15, "discover only supported chat and image models");
    for (const auto* id : {"Flux-2-Klein-9B-GGUF", "SD-Turbo", "named-image", "image-endpoint",
                           "image-kind", "image-architecture", "endpoint-kind", "endpoint-architecture",
                           "Flux-extra-kind", "Flux-extra-architecture", "image-and-chat"}) {
        const auto it = find(id);
        check(it != models.end() && it->type == lemon::ModelType::IMAGE
              && it->labels == std::vector<std::string>({"cloud", "image"}), id);
    }
    for (const auto* id : {"plain-chat", "Flux-chat", "Flux-vision", "vision-chat"}) {
        const auto it = find(id);
        check(it != models.end() && it->type == lemon::ModelType::LLM, id);
    }
    check(find("Sora-video") == models.end() && find("BGE-Embedding") == models.end()
          && find("Flux-edit") == models.end() && find("multimodal") == models.end()
          && find("Flux-chat-shaped") == models.end(),
          "unsupported output formats are not advertised as images or chat");

    const auto anthropic_models = CloudServer::discover_models(
        "image-test", "test-cloud-image-key", base, true, {}, "anthropic");
    check(anthropic_models.size() == 4, "Anthropic discovery excludes every image model");
    for (const auto* id : {"plain-chat", "Flux-chat", "Flux-vision", "vision-chat"}) {
        check(std::any_of(anthropic_models.begin(), anthropic_models.end(), [&](const auto& model) {
            return model.checkpoint() == id && model.type == lemon::ModelType::LLM;
        }), "Anthropic discovery retains supported chat models");
    }

    lemon::CloudProviderRegistry registry;
    registry.install("image-test", base);
    registry.set_allow_insecure_http("image-test", true);
    registry.set_runtime_key("image-test", "test-cloud-image-key");
    CloudServer server("image-test", "error", nullptr, nullptr, &registry);
    lemon::ModelInfo info;
    info.model_name = "image-test.Flux-2-Klein-9B-GGUF";
    info.checkpoints["main"] = "Flux-2-Klein-9B-GGUF";
    info.cloud_provider = "image-test";
    info.recipe = "cloud";
    info.type = lemon::ModelType::IMAGE;
    server.load(info.model_name, info, lemon::RecipeOptions("cloud", json::object()));
    const json request = {{"model", info.model_name}, {"prompt", "a cat"}, {"size", "256x256"},
                          {"response_format", "b64_json"}, {"steps", 4}, {"cfg_scale", 1}};
    const auto response = server.image_generations(request);
    json expected = request;
    expected["model"] = info.checkpoint();
    check(response.value("received", json()) == expected, "image endpoint receives original upstream id and generation options");
    check(response.contains("data") && response["data"][0]["b64_json"] == "test-image",
          "image data reaches the client unchanged");
    check(server.chat_completion(request).contains("error") && chat_requests == 0,
          "image model cannot be sent through chat");
    check(server.image_edits(request).contains("error") && server.image_variations(request).contains("error"),
          "unsupported image operations return errors");
    json failing_request = request;
    failing_request["prompt"] = "fail";
    const auto failure = server.image_generations(failing_request);
    check(failure.contains("error") && failure.dump().find("429") != std::string::npos,
          "upstream error status is preserved");

    registry.clear_runtime_key("image-test");
    check(server.image_generations(request).contains("error") && image_requests == 2,
          "missing credentials do not send an upstream request");
    registry.set_runtime_key("image-test", "test-cloud-image-key");
    registry.set_allow_insecure_http("image-test", false);
    check(server.image_generations(request).contains("error") && image_requests == 2,
          "plaintext credentials still require explicit opt-in");
    registry.set_allow_insecure_http("image-test", true);
    info.type = lemon::ModelType::LLM;
    info.checkpoints["main"] = "plain-chat";
    server.load("image-test.plain-chat", info, lemon::RecipeOptions("cloud", json::object()));
    check(server.image_generations(request).contains("error") && image_requests == 2,
          "chat model cannot be sent through image generation");
    const auto chat = server.chat_completion({{"model", "image-test.plain-chat"}, {"messages", json::array()}});
    check(chat.contains("received") && chat["received"]["model"] == "plain-chat" && chat_requests == 1,
          "existing chat routing still works");
    server.unload();
    mock.stop();
    listener.join();
    curl_global_cleanup();
    return failed == 0 ? 0 : 1;
}
