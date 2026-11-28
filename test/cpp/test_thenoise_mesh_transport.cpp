#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <httplib.h>
#include <lemon/backends/thenoise/thenoise_server.h>

namespace {
using lemon::json;
int failures = 0;
void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

// Track the actual stop_server() call without launching a GPU process.
class FakeMeshProcess : public lemon::ServerProcess {
public:
    explicit FakeMeshProcess(int& stops) : ServerProcess({}), stops_(stops) {}
    std::string start(const lemon::ServerCommand&) override { return "127.0.0.1"; }
    bool running() const override { return true; }
    void stop() override { ++stops_; }
private:
    int& stops_;
};

class FakeMeshModel : public lemon::backends::TheNoiseServer {
public:
    FakeMeshModel(int port, int& stops) : TheNoiseServer("error", nullptr, nullptr) {
        port_ = port;
        process_ = std::make_unique<FakeMeshProcess>(stops);
        model_type_ = lemon::ModelType::MESH;
        recipe_options_ = lemon::RecipeOptions("thenoise", json::object());
        set_state(lemon::ModelState::READY);
    }
    void options(const json& values) {
        recipe_options_ = lemon::RecipeOptions("thenoise", values);
    }
};

std::string generate(FakeMeshModel& model, json request = json::object()) {
    request["image"] = "aW1hZ2U=";
    std::string result;
    httplib::DataSink sink;
    sink.write = [&](const char* data, size_t len) { result.append(data, len); return true; };
    sink.done = []() {};
    sink.is_writable = []() { return true; };
    model.model_3d_generations(request, sink);
    return result;
}

int status(const std::string& result) {
    const auto parsed = json::parse(result, nullptr, false);
    return parsed.is_object() && parsed.contains("error")
        ? parsed["error"].value("status_code", 0) : 0;
}

// Small GLB 2.0 with a padded JSON chunk.
const std::string glb = std::string("glTF\x02\0\0\0\x30\0\0\0\x1c\0\0\0JSON", 20)
    + R"({"asset":{"version":"2.0"}} )";
const std::string stl = std::string(80, '\0') + std::string("\x01\0\0\0", 4) + std::string(50, '\0');
const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
} // namespace

int main() {
    httplib::Server backend;
    std::atomic<int> calls{0};
    std::atomic<int> mode{0};
    std::mutex body_mutex;
    json last_body;
    const auto body = [&] {
        std::lock_guard<std::mutex> lock(body_mutex);
        return last_body;
    };
    backend.Post("/mesh", [&](const httplib::Request& req, httplib::Response& res) {
        ++calls;
        const auto payload = json::parse(req.body);
        {
            std::lock_guard<std::mutex> lock(body_mutex);
            last_body = payload;
        }
        if (mode == 1) {
            res.status = 400;
            res.set_content("invalid mesh input", "text/plain");
        } else if (mode == 2) {
            res.set_content("not a GLB", "model/gltf-binary");
        } else if (mode == 3) {
            // Advertise a body, then close without delivering it. libcurl must
            // report a transport error rather than a complete HTTP response.
            res.set_header("Connection", "close");
            res.set_content_provider(128, "model/gltf-binary",
                [](size_t, size_t, httplib::DataSink&) { return false; });
        } else if (mode == 4) {
            res.set_content(stl.substr(0, stl.size() - 1), "model/stl");
        } else if (payload.value("output_format", "") == "stl") {
            res.set_content(stl, "model/stl");
        } else if (payload.value("output_format", "") == "obj") {
            res.set_content(obj, "model/obj");
        } else {
            res.set_content(glb, "model/gltf-binary");
        }
    });
    const int port = backend.bind_to_any_port("127.0.0.1");
    if (port <= 0) return 1;
    std::thread thread([&] { backend.listen_after_bind(); });
    backend.wait_until_ready();

    int stops = 0;
    FakeMeshModel model(port, stops);
    check("successful mesh keeps backend warm", generate(model) == glb && model.is_backend_alive() && stops == 0);
    mode = 1;
    check("complete HTTP error keeps backend warm", status(generate(model)) == 400 && model.is_backend_alive() && stops == 0);
    mode = 2;
    check("complete invalid GLB keeps backend warm", status(generate(model)) == 502 && model.is_backend_alive() && stops == 0);
    {
        std::atomic<bool> cancel{true};
        lemon::WrappedServer::RequestCancelScope scope(lemon::utils::RequestCancelToken{&cancel});
        check("pre-cancel does not submit or stop backend", status(generate(model)) == 499 && calls == 3 && stops == 0);
    }
    mode = 0;
    model.options({{"triangles", 0.2}, {"octree_resolution", 192}, {"steps", 12},
                   {"cfg_scale", 5.0}, {"sampler", "euler"}});
    check("stored mesh settings and GLB default", generate(model) == glb && body() == json({
        {"image", "aW1hZ2U="}, {"output_format", "glb"}, {"triangles", 0.2},
        {"octree_resolution", 192}, {"steps", 12}, {"guidance_scale", 5.0}, {"sampler", "euler"}}));
    const json request = {{"model", "alias"}, {"ctx_size", 2048}, {"resolution", 512},
        {"uv", "box"}, {"bg_removal", "threshold"}, {"response_format", "stl"},
        {"triangles", 0.5}, {"octree_resolution", 96}, {"steps", 4}, {"smooth", 0.3},
        {"floaters", 0.1}, {"latents", 2048}, {"cfg_scale", 2.0}, {"sampler", "heun"}};
    check("native STL bytes and request precedence", generate(model, request) == stl && body() == json({
        {"image", "aW1hZ2U="}, {"output_format", "stl"}, {"triangles", 0.5},
        {"octree_resolution", 96}, {"steps", 4}, {"smooth", 0.3}, {"floaters", 0.1},
        {"latents", 2048}, {"guidance_scale", 2.0}, {"sampler", "heun"}}));
    check("native OBJ bytes", generate(model, {{"response_format", "obj"}}) == obj);
    mode = 4;
    check("truncated STL keeps backend warm", status(generate(model, {{"response_format", "stl"}})) == 502 &&
          model.is_backend_alive() && stops == 0);
    mode = 3;
    const int before_transport_failure = calls.load();
    check("incomplete transport stops backend", status(generate(model)) == 502 && !model.is_backend_alive() && stops == 1);
    check("stopped instance cannot submit another mesh", status(generate(model)) == 502 && calls == before_transport_failure + 1 && stops == 1);

    backend.stop();
    thread.join();
    return failures ? 1 : 0;
}
