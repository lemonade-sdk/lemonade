// httplib first: it includes winsock2.h, which must precede windows.h.
#include <httplib.h>

#include "image_api.h"

#include "png.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>

namespace amica_engine {

namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

constexpr long long kMaxImagesPerRequest = 10;

void log(const std::string& message) {
    std::cerr << "[amica-engine] " << message << "\n";
    std::cerr.flush();
}

std::string dump(const ordered_json& value) {
    return value.dump(-1, ' ', false, ordered_json::error_handler_t::replace);
}

class RequestError : public std::runtime_error {
public:
    explicit RequestError(const std::string& message, int status = 400)
        : std::runtime_error(message), status(status) {}
    int status;
};

std::string base64(const std::string& bytes) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        std::uint32_t group = static_cast<std::uint8_t>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) group |= static_cast<std::uint8_t>(bytes[i + 1]) << 8;
        if (i + 2 < bytes.size()) group |= static_cast<std::uint8_t>(bytes[i + 2]);
        result += kAlphabet[(group >> 18) & 63];
        result += kAlphabet[(group >> 12) & 63];
        result += i + 1 < bytes.size() ? kAlphabet[(group >> 6) & 63] : '=';
        result += i + 2 < bytes.size() ? kAlphabet[group & 63] : '=';
    }
    return result;
}

long long integer(const json& body, const char* key, long long fallback) {
    const auto it = body.find(key);
    if (it == body.end() || it->is_null()) return fallback;
    if (!it->is_number_integer()) throw RequestError(std::string("'") + key + "' must be an integer");
    return it->get<long long>();
}

class ImageApi {
public:
    ImageApi(std::string model_name, ImageGenerator& generator)
        : model_name_(std::move(model_name)), generator_(generator) {}

    void health(const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok"})", "application/json");
    }

    void models(const httplib::Request&, httplib::Response& res) {
        const ordered_json body = {
            {"object", "list"},
            {"data", ordered_json::array({{{"id", model_name_}, {"object", "model"}, {"owned_by", "amica"}}})}};
        res.set_content(dump(body), "application/json");
    }

    void generations(const httplib::Request& req, httplib::Response& res) {
        try {
            const json body = parse_body(req);
            const auto prompt = body.value("prompt", json());
            if (!prompt.is_string() || prompt.get<std::string>().empty()) {
                throw RequestError("'prompt' must be a non-empty string");
            }
            const std::string format = body.value("response_format", json()).is_string()
                                           ? body.at("response_format").get<std::string>()
                                           : "b64_json";
            if (format != "b64_json") throw RequestError("response_format must be b64_json");
            check_size(body);
            const long long count = integer(body, "n", 1);
            if (count < 1 || count > kMaxImagesPerRequest) {
                throw RequestError("'n' must be between 1 and " + std::to_string(kMaxImagesPerRequest));
            }
            long long steps = integer(body, "steps", static_cast<long long>(generator_.default_steps()));
            steps = integer(body, "num_inference_steps", steps);
            if (steps < 1 || static_cast<std::size_t>(steps) > generator_.max_steps()) {
                throw RequestError("'steps' must be between 1 and " + std::to_string(generator_.max_steps()));
            }
            long long seed = integer(body, "seed", -1);
            if (seed < 0) seed = static_cast<long long>(std::random_device{}() >> 1);

            ordered_json data = ordered_json::array();
            double generate_ms = 0.0;
            for (long long i = 0; i < count; ++i) {
                const Image image = generate(prompt.get<std::string>(), static_cast<std::size_t>(steps),
                                             static_cast<std::uint64_t>(seed + i));
                generate_ms += image.generate_ms;
                data.push_back({{"b64_json", base64(encode_png(image.width, image.height, image.rgb))}});
            }
            log("generated " + std::to_string(count) + " image(s), " + std::to_string(steps) + " steps, seed " +
                std::to_string(seed) + ", in " + std::to_string(static_cast<long long>(generate_ms)) + " ms");
            const ordered_json response = {{"created", static_cast<long long>(std::time(nullptr))}, {"data", data}};
            res.set_content(dump(response), "application/json");
        } catch (const RequestError& error) {
            send_error(res, error.what(), error.status);
        } catch (const json::exception& error) {
            send_error(res, std::string("Invalid request: ") + error.what(), 400);
        } catch (const std::exception& error) {
            log(std::string("request failed: ") + error.what());
            send_error(res, error.what(), 500);
        }
    }

private:
    static void send_error(httplib::Response& res, const std::string& message, int status) {
        const ordered_json body = {
            {"error", {{"message", message}, {"type", status < 500 ? "invalid_request_error" : "server_error"}}}};
        res.status = status;
        res.set_content(dump(body), "application/json");
    }

    static json parse_body(const httplib::Request& req) {
        json body;
        try {
            body = req.body.empty() ? json::object() : json::parse(req.body);
        } catch (const json::parse_error& error) {
            throw RequestError(std::string("Invalid JSON: ") + error.what());
        }
        if (!body.is_object()) throw RequestError("The request body must be a JSON object");
        return body;
    }

    // The model's image size is fixed when the bundle is prepared.
    void check_size(const json& body) const {
        const std::string expected = std::to_string(generator_.width()) + "x" + std::to_string(generator_.height());
        const auto size = body.value("size", json());
        if (size.is_string() && size.get<std::string>() != "auto" && size.get<std::string>() != expected) {
            throw RequestError("this AMICA model generates " + expected + " images; size " + size.get<std::string>() +
                               " is not available");
        }
        const long long width = integer(body, "width", static_cast<long long>(generator_.width()));
        const long long height = integer(body, "height", static_cast<long long>(generator_.height()));
        if (width != static_cast<long long>(generator_.width()) ||
            height != static_cast<long long>(generator_.height())) {
            throw RequestError("this AMICA model generates " + expected + " images; " + std::to_string(width) + "x" +
                               std::to_string(height) + " is not available");
        }
    }

    Image generate(const std::string& prompt, std::size_t steps, std::uint64_t seed) {
        std::lock_guard<std::mutex> lock(generation_mutex_);
        try {
            return generator_.generate(prompt, steps, seed);
        } catch (const std::exception& error) {
            log(std::string("generation failed: ") + error.what());
            if (generator_.device_removed()) {
                log("the GPU device was removed; exiting so Lemonade reloads the model");
                // Leave time for the error response to reach the client.
                std::thread([] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    std::_Exit(3);
                }).detach();
            }
            throw RequestError(std::string("amica-engine: ") + error.what(), 500);
        }
    }

    std::string model_name_;
    ImageGenerator& generator_;
    std::mutex generation_mutex_;
};

}  // namespace

int serve_images(const std::string& host, int port, const std::string& model_name, ImageGenerator& generator) {
    ImageApi api(model_name, generator);
    httplib::Server server;
    server.Get("/health", [&](const httplib::Request& req, httplib::Response& res) { api.health(req, res); });
    server.Get(R"(/(?:v1/)?models)",
               [&](const httplib::Request& req, httplib::Response& res) { api.models(req, res); });
    server.Post(R"(/(?:v1/)?images/generations)",
                [&](const httplib::Request& req, httplib::Response& res) { api.generations(req, res); });
    server.set_error_handler([](const httplib::Request&, httplib::Response& res) {
        if (res.status == 404 && res.body.empty()) {
            res.set_content(R"({"error":{"message":"Not found"}})", "application/json");
        }
    });
    server.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        if (req.path != "/health") log(req.method + " " + req.path + " " + std::to_string(res.status));
    });
    if (!server.bind_to_port(host, port)) {
        log("cannot listen on " + host + ":" + std::to_string(port));
        return 1;
    }
    log("listening on http://" + host + ":" + std::to_string(port));
    server.listen_after_bind();
    return 0;
}

}  // namespace amica_engine
