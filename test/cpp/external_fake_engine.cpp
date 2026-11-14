#include <httplib.h>
#include <mbedtls/md.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

int main(int argc, char** argv) {
    using Json = nlohmann::json;
    std::vector<std::string> arguments(argv + 1, argv + argc);
    const auto value = [&](const std::string& name) {
        for (std::size_t i = 0; i + 1 < arguments.size(); ++i) if (arguments[i] == name) return arguments[i + 1];
        return std::string();
    };
    const auto path = value("--test-transcript");
    if (path.empty()) return 72;
#ifdef _WIN32
    const auto pid = _getpid();
#else
    const auto pid = getpid();
#endif
    mbedtls_md_context_t hash;
    mbedtls_md_init(&hash);
    const auto* algorithm = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!algorithm || mbedtls_md_setup(&hash, algorithm, 0) || mbedtls_md_starts(&hash)) return 75;
    std::ifstream executable(argv[0], std::ios::binary);
    char buffer[4096];
    while (executable.read(buffer, sizeof(buffer)) || executable.gcount())
        if (mbedtls_md_update(&hash, reinterpret_cast<const unsigned char*>(buffer), executable.gcount())) return 75;
    unsigned char digest[32];
    if (mbedtls_md_finish(&hash, digest)) return 75;
    mbedtls_md_free(&hash);
    std::string sha;
    for (const auto byte : digest) { sha += "0123456789abcdef"[byte >> 4]; sha += "0123456789abcdef"[byte & 15]; }
    Json transcript{{"sha256", sha}, {"pid", pid}, {"path", std::filesystem::canonical(argv[0]).string()}, {"argv", arguments},
        {"environment", {{"GGML_DISABLE_VULKAN", std::getenv("GGML_DISABLE_VULKAN") ? std::getenv("GGML_DISABLE_VULKAN") : ""}}},
        {"requests", Json::array()}, {"marker", "external-fork-cpu-4g-73f6"}};
    const auto save = [&] { std::ofstream(path) << transcript.dump(2) << '\n'; };
    save();
    if (value("--test-readiness") == "fail") return 73;
    httplib::Server server;
    server.new_task_queue = [] { return new httplib::ThreadPool(4); };
    server.set_keep_alive_max_count(1);
    server.set_read_timeout(2);
    server.set_write_timeout(2);
    server.Get("/health", [](const auto&, auto& response) { response.set_content("{}", "application/json"); });
    server.Post("/v1/chat/completions", [&](const auto& request, auto& response) {
        transcript["requests"].push_back({{"path", request.path}, {"body", Json::parse(request.body)}});
        save();
        response.set_content(Json{{"id", "external-fork-cpu-4g-73f6"}, {"model", "engine-internal"},
            {"choices", {{{"message", {{"role", "assistant"}, {"content", "external-fork-cpu-4g-73f6"}}}}}}}.dump(), "application/json");
    });
    return server.listen("127.0.0.1", std::stoi(value("--port"))) ? 0 : 74;
}
