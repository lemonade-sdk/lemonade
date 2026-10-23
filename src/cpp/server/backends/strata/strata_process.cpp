#include "lemon/backends/strata/strata_process.h"

#include "lemon/runtime_config.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/http_client.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chat.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <deque>
#include <fstream>
#include <functional>
#include <httplib.h>
#include <iterator>
#include <llama.h>
#include <map>
#include <mbedtls/md.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sheredom/subprocess.h>
#include <sstream>
#include <stdexcept>
#include <thread>

#ifndef _WIN32
#include <pthread.h>
#include <signal.h>
#ifdef __APPLE__
#include <crt_externs.h>
#else
#include <unistd.h>
#endif
#endif

namespace lemon::backends::strata {
namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

std::string new_id(const std::string& prefix) {
    static std::atomic<uint64_t> counter{0};
    return prefix + std::to_string(Clock::now().time_since_epoch().count()) + "_" + std::to_string(++counter);
}

int64_t now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

json error_body(const std::string& message, const std::string& type) {
    return {{"error", {{"message", message}, {"type", type}, {"code", type}}}};
}

json chat_request(json request) {
    if (request.value("store", false) || request.contains("previous_response_id")) {
        throw std::invalid_argument("Strata supports stateless Responses only");
    }
    json messages = json::array();
    if (request.contains("instructions") && !request["instructions"].is_null()) {
        messages.push_back({{"role", "system"}, {"content", request["instructions"]}});
    }
    const auto& input = request.at("input");
    if (input.is_string()) {
        messages.push_back({{"role", "user"}, {"content", input}});
    } else if (input.is_array()) {
        for (auto item : input) {
            const auto type = item.value("type", "message");
            if (type == "function_call_output") {
                messages.push_back({{"role", "tool"}, {"tool_call_id", item.at("call_id")},
                                    {"content", item.at("output")}});
            } else if (type == "function_call") {
                messages.push_back({{"role", "assistant"}, {"content", nullptr},
                    {"tool_calls", json::array({{{"id", item.at("call_id")}, {"type", "function"},
                        {"function", {{"name", item.at("name")}, {"arguments", item.at("arguments")}}}}})}});
            } else if (type == "message") {
                if (item.contains("content") && item["content"].is_array()) {
                    for (auto& part : item["content"]) {
                        const auto kind = part.value("type", "");
                        if (kind == "input_image") {
                            part = {{"type", "image_url"}, {"image_url", {{"url", part.at("image_url")}}}};
                        } else if (kind == "input_text" || kind == "output_text") part["type"] = "text";
                        else throw std::invalid_argument("Unsupported Strata Responses content part: " + kind);
                    }
                }
                messages.push_back(item);
            } else {
                throw std::invalid_argument("Unsupported Strata Responses input: " + type);
            }
        }
    } else {
        throw std::invalid_argument("Responses input must be text or an array");
    }
    request["messages"] = messages;
    if (request.contains("max_output_tokens")) request["max_completion_tokens"] = request["max_output_tokens"];
    if (request.contains("tools")) {
        for (auto& tool : request["tools"]) {
            if (tool.value("type", "") != "function") throw std::invalid_argument("Strata supports function tools only");
            tool = {{"type", "function"}, {"function", {
                {"name", tool.at("name")}, {"description", tool.value("description", "")},
                {"parameters", tool.value("parameters", json::object())}}}};
        }
    }
    if (request.contains("reasoning")) request["reasoning_effort"] = request["reasoning"].value("effort", "high");
    return request;
}

json response_output(const json& message, const std::string& id) {
    json output = json::array();
    if (message.contains("reasoning_content") && !message["reasoning_content"].get<std::string>().empty()) {
        output.push_back({{"type", "reasoning"}, {"id", "rs_" + id}, {"summary",
            json::array({{{"type", "summary_text"}, {"text", message["reasoning_content"]}}})}});
    }
    if (!message.value("content", json()).is_null()) {
        output.push_back({{"type", "message"}, {"id", "msg_" + id}, {"status", "completed"},
            {"role", "assistant"}, {"content", json::array({{{"type", "output_text"},
                {"text", message["content"]}, {"annotations", json::array()}}})}});
    }
    for (const auto& tool : message.value("tool_calls", json::array())) {
        output.push_back({{"type", "function_call"}, {"id", "fc_" + tool.at("id").get<std::string>()},
            {"call_id", tool.at("id")}, {"name", tool.at("function").at("name")},
            {"arguments", tool.at("function").at("arguments")}, {"status", "completed"}});
    }
    return output;
}

bool utf8_complete(const std::string& text) {
    if (text.empty()) return true;
    size_t i = text.size() - 1;
    while (i && (static_cast<unsigned char>(text[i]) & 0xc0) == 0x80) --i;
    const auto c = static_cast<unsigned char>(text[i]);
    const size_t expected = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : 4;
    return text.size() - i >= expected;
}

std::vector<std::string> child_environment(const ServerCommand& command) {
    std::vector<std::string> environment;
#ifdef _WIN32
    auto* block = GetEnvironmentStringsW();
    if (!block) throw std::runtime_error("Could not read the Strata child environment");
    for (const wchar_t* entry = block; *entry; entry += std::wcslen(entry) + 1) {
        environment.push_back(utils::path_to_utf8(std::filesystem::path(entry)));
    }
    FreeEnvironmentStringsW(block);
#else
#ifdef __APPLE__
    auto** inherited = *_NSGetEnviron();
#else
    auto** inherited = environ;
#endif
    for (auto** entry = inherited; *entry; ++entry) environment.emplace_back(*entry);
#endif
    const auto key_of = [](std::string s) {
        s.resize(s.find('='));
#ifdef _WIN32
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
        return s;
    };
    for (const auto& [key, value] : command.env) {
        const auto entry = key + "=" + value;
        const auto normalized = key_of(entry);
        environment.erase(std::remove_if(environment.begin(), environment.end(),
            [&](const auto& existing) { return key_of(existing) == normalized; }), environment.end());
        environment.push_back(entry);
    }
    return environment;
}

struct TemporaryFile {
    std::filesystem::path path;
    ~TemporaryFile() { cleanup(); }
    void cleanup() {
        if (path.empty()) return;
        std::error_code error;
        std::filesystem::remove(path, error);
        if (error) LOG(WARNING, "Strata") << "Could not clean temporary image file: " << error.message() << std::endl;
        path.clear();
    }
};

struct NativePipe {
    subprocess_s child{};
    bool created = false;
    mutable std::mutex lifecycle;
    std::atomic<bool> stopping{false};
    std::string pending;
    std::string stderr_pending;
    std::deque<std::string> lines;

    ~NativePipe() { terminate(); close(); }

    void launch(const ServerCommand& command, const std::filesystem::path& directory) {
        std::lock_guard<std::mutex> lock(lifecycle);
        if (stopping) throw std::runtime_error("Strata subprocess start cancelled");
        std::vector<std::string> arguments{command.program};
        arguments.insert(arguments.end(), command.args.begin(), command.args.end());
        std::vector<const char*> argv;
        for (const auto& arg : arguments) argv.push_back(arg.c_str());
        argv.push_back(nullptr);
        const auto environment = child_environment(command);
        std::vector<const char*> env;
        for (const auto& entry : environment) env.push_back(entry.c_str());
        env.push_back(nullptr);
        const auto cwd = utils::path_to_utf8(directory);
        const int options = subprocess_option_no_window |
            subprocess_option_enable_async | subprocess_option_enable_async_no_wait;
        if (subprocess_create_ex(argv.data(), options, env.data(), cwd.c_str(), &child)) {
            throw std::runtime_error("Could not start native Strata subprocess: " + command.program);
        }
        created = true;
    }

    void terminate() {
        stopping = true;
        std::lock_guard<std::mutex> lock(lifecycle);
        if (created) subprocess_terminate(&child);
    }

    void close() {
        std::lock_guard<std::mutex> lock(lifecycle);
        if (created) {
            int exit_code = 0;
            subprocess_join(&child, &exit_code);
            subprocess_destroy(&child);
            created = false;
        }
    }

    bool alive() const {
        std::lock_guard<std::mutex> lock(lifecycle);
        return created && !stopping.load() && subprocess_alive(const_cast<subprocess_s*>(&child));
    }

    void send(const std::string& text) {
#ifndef _WIN32
        sigset_t blocked;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
#endif
        auto* input = subprocess_stdin(&child);
        if (!input || std::fwrite(text.data(), 1, text.size(), input) != text.size() || std::fflush(input)) {
            throw std::runtime_error("Strata engine input pipe closed");
        }
    }

    void pump() {
        char buffer[8192];
        auto n = subprocess_read_stderr(&child, buffer, sizeof(buffer));
        if (n) {
            stderr_pending.append(buffer, n);
            size_t pos;
            while ((pos = stderr_pending.find('\n')) != std::string::npos) {
                LOG(INFO, "Strata") << stderr_pending.substr(0, pos) << std::endl;
                stderr_pending.erase(0, pos + 1);
            }
            if (stderr_pending.size() > 65536) throw std::runtime_error("Strata engine stderr line is too long");
        }
        n = subprocess_read_stdout(&child, buffer, sizeof(buffer));
        if (n) {
            pending.append(buffer, n);
            size_t pos;
            while ((pos = pending.find('\n')) != std::string::npos) {
                auto line = pending.substr(0, pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                lines.push_back(std::move(line));
                pending.erase(0, pos + 1);
            }
            if (pending.size() > 65536) throw std::runtime_error("Strata engine protocol line is too long");
        }
    }

    bool next(std::string& line) {
        pump();
        if (!lines.empty()) {
            line = std::move(lines.front());
            lines.pop_front();
            return true;
        }
        if (!alive()) throw std::runtime_error("Strata subprocess exited unexpectedly");
        return false;
    }
};

struct VisionEncoder : NativePipe {
    VisionCommand config;
    std::function<bool()> load_cancelled;
    std::filesystem::path directory;
    int dimensions = 0;
    struct Encoded {
        std::string key;
        int tokens;
    };
    std::map<std::string, int> cache;
    std::deque<std::string> lru;

    VisionEncoder(VisionCommand command, std::function<bool()> cancelled)
        : config(std::move(command)), load_cancelled(std::move(cancelled)) {
        directory = std::filesystem::temp_directory_path() / new_id("lemonade-strata-vision-");
        if (!std::filesystem::create_directory(directory)) throw std::runtime_error("Could not create Strata vision workspace");
        try {
            std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
        } catch (const std::filesystem::filesystem_error&) {
            std::error_code error;
            std::filesystem::remove(directory, error);
            if (error) LOG(WARNING, "Strata") << "Could not clean vision workspace: " << error.message() << std::endl;
            throw;
        }
    }

    ~VisionEncoder() {
        terminate();
        close();
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        if (error) LOG(WARNING, "Strata") << "Could not clean vision workspace: " << error.message() << std::endl;
    }

    std::string wait(const std::function<bool()>& cancelled) {
        const auto deadline = Clock::now() + std::chrono::minutes(5);
        try {
            for (;;) {
                if (cancelled() || (load_cancelled && load_cancelled())) {
                    throw std::runtime_error("Strata vision cancelled");
                }
                if (Clock::now() >= deadline) throw std::runtime_error("Strata vision encoder timed out");
                std::string line;
                if (next(line)) return line;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        } catch (const std::exception&) {
            terminate();
            close();
            dimensions = 0;
            throw;
        }
    }

    void start(const std::function<bool()>& cancelled) {
        stopping = false;
        if (cancelled()) throw std::runtime_error("Strata vision start cancelled");
        launch(config.command, directory);
        const auto line = wait(cancelled);
        std::istringstream input(line);
        std::string tag;
        input >> tag >> dimensions;
        if (!input || tag != "READY" || dimensions <= 0 || dimensions > 16384) {
            throw std::runtime_error("Strata vision startup: " + line);
        }
        LOG(INFO, "Strata") << "Native picture reader ready, embedding dimensions=" << dimensions << std::endl;
    }

    static constexpr size_t max_image_bytes = 20 * 1024 * 1024;

    std::string load_image(const std::string& source, const std::function<bool()>& cancelled) {
        if (source.rfind("data:image/", 0) == 0) {
            const auto comma = source.find(',');
            if (comma == std::string::npos || comma < 7 || source.substr(comma - 7, 7) != ";base64") {
                throw std::invalid_argument("Images must use a base64 data URL");
            }
            const auto data = source.substr(comma + 1);
            const auto first_pad = data.find('=');
            const size_t padding = first_pad == std::string::npos ? 0 : data.size() - first_pad;
            if (data.empty() || data.size() > (max_image_bytes + 2) / 3 * 4 || data.size() % 4 || padding > 2 ||
                data.substr(0, data.size() - padding).find_first_not_of(
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/") != std::string::npos ||
                (padding && data.substr(first_pad).find_first_not_of('=') != std::string::npos)) {
                throw std::invalid_argument("Invalid or oversized base64 image (maximum 20 MiB)");
            }
            auto decoded = utils::JsonUtils::base64_decode(data);
            if (decoded.size() > max_image_bytes) throw std::invalid_argument("Image exceeds 20 MiB");
            return decoded;
        }
        if (source.rfind("https://", 0) != 0) {
            throw std::invalid_argument("Images must be base64 data URLs or HTTPS URLs; local files are not read");
        }
        if (auto* cfg = RuntimeConfig::global(); cfg && cfg->offline()) {
            throw std::invalid_argument("Offline mode requires base64 image data rather than remote image URLs");
        }
        const auto path = directory / "download.img";
        const TemporaryFile cleanup{path};
        utils::DownloadOptions options;
        options.max_retries = 0;
        options.resume_partial = false;
        options.connect_timeout = 10;
        options.no_progress_timeout = 15;
        const auto deadline = Clock::now() + std::chrono::seconds(60);
        const auto result = utils::HttpClient::download_file(source, utils::path_to_utf8(path),
            [&](size_t downloaded, size_t total) {
                return !stopping && !cancelled() && Clock::now() < deadline &&
                    downloaded <= max_image_bytes && total <= max_image_bytes;
            }, {}, options);
        if (!result.success) throw std::invalid_argument("Strata image download failed: " + result.error_message);
        if (std::filesystem::file_size(path) > max_image_bytes) throw std::invalid_argument("Image exceeds 20 MiB");
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("Could not read downloaded image");
        std::string data{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (input.bad()) throw std::runtime_error("Could not read downloaded image");
        return data;
    }

    Encoded encode(const std::string& source, const std::function<bool()>& cancelled) {
        if (stopping || cancelled()) throw std::runtime_error("Strata vision cancelled");
        const auto data = load_image(source, cancelled);
        std::array<unsigned char, 32> digest{};
        if (mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                       reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest.data())) {
            throw std::runtime_error("Could not hash Strata image");
        }
        std::string key;
        for (const auto byte : digest) {
            key += "0123456789abcdef"[byte >> 4];
            key += "0123456789abcdef"[byte & 15];
        }
        if (cache.count(key) && std::filesystem::is_regular_file(directory / (key + ".sve"))) {
            lru.erase(std::remove(lru.begin(), lru.end(), key), lru.end());
            lru.push_back(key);
            return {key, cache.at(key)};
        }
        const auto image = directory / "input.img";
        const TemporaryFile cleanup{image};
        TemporaryFile features{directory / (key + ".sve")};
        {
            std::ofstream output(image, std::ios::binary);
            output.write(data.data(), static_cast<std::streamsize>(data.size()));
            output.close();
            if (!output) throw std::runtime_error("Could not write Strata image");
        }
        send("ENC input.img " + key + ".sve\n");
        const auto line = wait(cancelled);
        if (line.rfind("ERR ", 0) == 0) throw std::invalid_argument("Strata picture reader: " + line.substr(4));
        std::istringstream reply(line);
        std::string tag;
        int tokens = 0;
        reply >> tag >> tokens;
        if (!reply || tag != "OK" || tokens <= 0 || tokens > config.max_tokens * 4) {
            throw std::runtime_error("Invalid Strata vision reply: " + line);
        }
        cache[key] = tokens;
        features.path.clear();
        lru.erase(std::remove(lru.begin(), lru.end(), key), lru.end());
        lru.push_back(key);
        while (lru.size() > 64) {
            const auto old = lru.front();
            std::filesystem::remove(directory / (old + ".sve"));
            cache.erase(old);
            lru.pop_front();
        }
        return {key, tokens};
    }

    void combine(const std::vector<Encoded>& images) {
        std::ofstream output(directory / "request.sve", std::ios::binary);
        uint64_t total = 0;
        for (const auto& image : images) {
            const auto path = directory / (image.key + ".sve");
            std::ifstream input(path, std::ios::binary);
            std::array<int32_t, 5> header{};
            input.read(reinterpret_cast<char*>(header.data()), sizeof(header));
            const auto bytes = sizeof(header) + static_cast<uint64_t>(image.tokens) * dimensions * sizeof(float);
            total += bytes;
            if (!input || header[0] != 0x31455653 || header[1] != image.tokens || header[2] <= 0 ||
                header[3] <= 0 || header[4] != dimensions || std::filesystem::file_size(path) != bytes ||
                total > 256 * 1024 * 1024) {
                input.close();
                cache.erase(image.key);
                std::filesystem::remove(path);
                throw std::runtime_error("Invalid or oversized Strata vision embeddings");
            }
            input.seekg(0);
            output << input.rdbuf();
            if (!output || input.bad()) throw std::runtime_error("Could not combine Strata vision embeddings");
        }
        output.close();
        if (!output) throw std::runtime_error("Could not write combined Strata vision embeddings");
    }
};

}

struct StrataProcess::Impl : NativePipe {
    std::filesystem::path vocabulary;
    std::string model_name;
    std::function<bool()> load_cancelled;
    std::unique_ptr<llama_model, decltype(&llama_model_free)> model{nullptr, llama_model_free};
    common_chat_templates_ptr templates;
    std::unique_ptr<VisionEncoder> vision;
    std::timed_mutex generation;
    std::atomic<bool> ready{false};
    int context = 0;

    struct Request {
        json body;
        std::vector<llama_token> tokens;
        common_chat_parser_params parser;
        int max_new;
        std::string command;
        std::vector<std::string> stops;
    };

    Request prepare(json body, const std::function<bool()>& cancelled) {
        if (!body.is_object() || !body.contains("messages") || !body["messages"].is_array() || body["messages"].empty()) {
            throw std::invalid_argument("messages must be a non-empty array");
        }
        std::vector<std::pair<std::string, std::string>> images;
        for (auto& message : body["messages"]) {
            if (!message.is_object()) throw std::invalid_argument("Each message must be an object");
            const auto role = message.value("role", "");
            if (role != "system" && role != "developer" && role != "user" && role != "assistant" && role != "tool") {
                throw std::invalid_argument("Unsupported message role");
            }
            auto& content = message["content"];
            if (content.is_array()) {
                for (auto& part : content) {
                    const auto type = part.value("type", "");
                    if (type == "image_url") {
                        if (!vision) throw std::invalid_argument("Strata vision is disabled for this model");
                        if (images.size() >= 64) throw std::invalid_argument("Strata supports at most 64 images per request");
                        const auto marker = new_id("lemonade_image_");
                        images.emplace_back(marker, part.at("image_url").at("url").get<std::string>());
                        part = {{"type", "media_marker"}, {"text", marker}};
                    } else if (type != "text") throw std::invalid_argument("Unsupported Strata message content: " + type);
                }
            } else if (!content.is_string() && !content.is_null()) {
                throw std::invalid_argument("Message content must be text");
            }
        }
        if (body.value("n", 1) != 1) throw std::invalid_argument("Strata supports n=1 only");
        if (body.contains("response_format") || body.contains("grammar") || body.contains("logit_bias") ||
            body.value("logprobs", false)) {
            throw std::invalid_argument("Strata native adapter does not support constrained decoding or logprobs");
        }
        common_chat_templates_inputs input;
        input.messages = common_chat_msgs_parse_oaicompat(common_json::parse(body["messages"].dump()));
        input.enable_thinking = true;
        input.reasoning_format = COMMON_REASONING_FORMAT_DEEPSEEK;
        if (body.contains("chat_template_kwargs")) {
            for (const auto& [key, value] : body["chat_template_kwargs"].items()) input.chat_template_kwargs[key] = value.dump();
            input.enable_thinking = body["chat_template_kwargs"].value("enable_thinking", true);
        }
        if (body.contains("reasoning_effort")) {
            const auto effort = body["reasoning_effort"].get<std::string>();
            if (effort == "none") input.enable_thinking = false;
            else input.chat_template_kwargs["reasoning_effort"] = json(effort).dump();
        }
        if (body.contains("tools")) input.tools = common_chat_tools_parse_oaicompat(common_json::parse(body["tools"].dump()));
        if (body.contains("tool_choice")) {
            if (!body["tool_choice"].is_string() || (body["tool_choice"] != "auto" && body["tool_choice"] != "none")) {
                throw std::invalid_argument("Strata native adapter supports auto/none tool_choice only");
            }
            if (body["tool_choice"] == "none") input.tools.clear();
        }
        input.parallel_tool_calls = body.value("parallel_tool_calls", true);
        const auto formatted = common_chat_templates_apply(templates.get(), input);
        const auto* vocab = llama_model_get_vocab(model.get());
        Request result;
        result.body = std::move(body);
        const auto tokenize = [&](const std::string& text, bool special) {
            const auto length = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()),
                                               nullptr, 0, false, special);
            std::vector<llama_token> tokens(static_cast<size_t>(length < 0 ? -length : 0));
            if (llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()),
                               tokens.data(), static_cast<int32_t>(tokens.size()), false, special) !=
                static_cast<int32_t>(tokens.size())) throw std::runtime_error("Strata tokenization failed");
            return tokens;
        };
        std::vector<VisionEncoder::Encoded> encoded;
        if (images.empty()) result.tokens = tokenize(formatted.prompt, true);
        else {
            const auto start = tokenize("<|vision_start|>", true);
            const auto pad = tokenize("<|image_pad|>", true);
            const auto end = tokenize("<|vision_end|>", true);
            if (start.size() != 1 || pad.size() != 1 || end.size() != 1) {
                throw std::runtime_error("Strata tokenizer has no native vision markers");
            }
            if (!vision->alive()) {
                vision->terminate();
                vision->close();
                vision->pending.clear();
                vision->lines.clear();
                vision->start([&] { return stopping || cancelled(); });
            }
            std::sort(images.begin(), images.end(), [&](const auto& a, const auto& b) {
                return formatted.prompt.find(a.first) < formatted.prompt.find(b.first);
            });
            const auto literal_pad = tokenize("<|image_pad|>", false);
            const auto append_text = [&](const std::string& text) {
                for (const auto token : tokenize(text, true)) {
                    if (token == pad[0]) result.tokens.insert(result.tokens.end(), literal_pad.begin(), literal_pad.end());
                    else result.tokens.push_back(token);
                }
            };
            size_t offset = 0;
            for (const auto& [marker, source] : images) {
                const auto position = formatted.prompt.find(marker, offset);
                if (position == std::string::npos ||
                    formatted.prompt.find(marker, position + marker.size()) != std::string::npos) {
                    throw std::invalid_argument("Strata chat template did not preserve the image content");
                }
                append_text(formatted.prompt.substr(offset, position - offset));
                const auto image = vision->encode(source, [&] { return stopping || cancelled(); });
                if (result.tokens.size() + image.tokens + 2 > static_cast<size_t>(context - 8)) {
                    throw std::invalid_argument("Image prompt exceeds the Strata context");
                }
                result.tokens.push_back(start[0]);
                result.tokens.insert(result.tokens.end(), image.tokens, pad[0]);
                result.tokens.push_back(end[0]);
                encoded.push_back(image);
                offset = position + marker.size();
            }
            append_text(formatted.prompt.substr(offset));
        }
        if (result.tokens.empty()) throw std::invalid_argument("Strata chat template produced an empty prompt");
        result.parser = common_chat_parser_params(formatted);
        result.parser.reasoning_format = COMMON_REASONING_FORMAT_DEEPSEEK;
        result.parser.parse_tool_calls = !input.tools.empty();
        if (!formatted.parser.empty()) result.parser.parser.load(formatted.parser);
        const auto available = context - static_cast<int>(result.tokens.size()) - 8;
        result.max_new = result.body.value("max_completion_tokens", result.body.value("max_tokens", std::min(2048, available)));
        if (result.max_new <= 0 || result.max_new > available) {
            throw std::invalid_argument("Prompt and requested output exceed the Strata context");
        }
        std::ostringstream command;
        command << (encoded.empty() ? "GEN " : "GENI ") << result.max_new;
        const auto number = [&](const char* api, const char* engine, double low, double high, bool integer = false) {
            if (!result.body.contains(api)) return;
            const auto& v = result.body[api];
            if (!v.is_number() || (integer && !v.is_number_integer())) throw std::invalid_argument(std::string(api) + " has invalid type");
            const double value = v.get<double>();
            if (!std::isfinite(value) || value < low || value > high) throw std::invalid_argument(std::string(api) + " is out of range");
            command << " " << engine << "=" << v.dump();
        };
        number("temperature", "temperature", 0, 2);
        number("top_p", "top_p", 0, 1);
        number("top_k", "top_k", 1, 64, true);
        number("min_p", "min_p", 0, 1);
        number("repeat_penalty", "penalty_repeat", 0.01, 100);
        number("frequency_penalty", "penalty_freq", -2, 2);
        number("presence_penalty", "penalty_present", -2, 2);
        if (result.body.contains("frequency_penalty") || result.body.contains("presence_penalty") ||
            result.body.contains("repeat_penalty")) command << " penalty_last_n=64";
        number("seed", "seed", 0, 9007199254740991.0, true);
        if (!encoded.empty()) {
            vision->combine(encoded);
            command << " img_keys=";
            for (size_t i = 0; i < encoded.size(); ++i) {
                if (i) command << ",";
                command << encoded[i].key.substr(0, 16);
            }
            command << " request.sve";
        }
        command << " ";
        for (size_t i = 0; i < result.tokens.size(); ++i) {
            if (i) command << ",";
            command << result.tokens[i];
        }
        command << "\n";
        result.command = command.str();
        result.stops = formatted.additional_stops;
        if (result.body.contains("stop") && !result.body["stop"].is_null()) {
            if (result.body["stop"].is_string()) result.stops.push_back(result.body["stop"]);
            else {
                const auto stops = result.body["stop"].get<std::vector<std::string>>();
                result.stops.insert(result.stops.end(), stops.begin(), stops.end());
            }
        }
        for (const auto& stop : result.stops) if (stop.empty()) throw std::invalid_argument("Stop strings must not be empty");
        return result;
    }

    json generate(const Request& request, const std::function<bool()>& cancelled,
                  const std::function<bool(const json&)>& delta = {}) {
        if (!alive()) throw std::runtime_error("Strata engine is unavailable");
        bool sent_stop = false, matched_stop = false, done_received = false;
        std::string output;
        common_chat_msg previous;
        std::vector<std::string> tool_ids;
        auto heard = Clock::now();
        auto heartbeat = heard;
        int prompt_tokens = static_cast<int>(request.tokens.size()), completion_tokens = 0, cached_tokens = 0;
        double prompt_ms = 0.0, decode_ms = 0.0;
        int64_t accepted = 0, offered = 0;
        std::string finish;
        const auto stop = [&] {
            if (!sent_stop) {
                send("STOP\n");
                sent_stop = true;
            }
        };
        const auto parse = [&](bool partial) {
            auto visible = output;
            if (partial && !matched_stop) {
                size_t hold = 0;
                for (const auto& s : request.stops) {
                    for (size_t n = 1; n < s.size() && n <= visible.size(); ++n) {
                        if (visible.compare(visible.size() - n, n, s, 0, n) == 0) hold = std::max(hold, n);
                    }
                }
                visible.resize(visible.size() - hold);
            }
            if (partial && !utf8_complete(visible)) return previous;
            visible = json::parse(json(visible).dump(-1, ' ', false, json::error_handler_t::replace)).get<std::string>();
            auto message = common_chat_parse(visible, partial, request.parser);
            message.set_tool_call_ids(tool_ids, [] { return new_id("call_"); });
            return message;
        };
        const auto emit = [&](const common_chat_msg& message) {
            if (!delta) return true;
            for (const auto& diff : common_chat_msg_diff::compute_diffs(previous, message)) {
                json d = json::object();
                if (!diff.content_delta.empty()) d["content"] = diff.content_delta;
                if (!diff.reasoning_content_delta.empty()) d["reasoning_content"] = diff.reasoning_content_delta;
                if (diff.tool_call_index != std::string::npos) {
                    json tool = {{"index", diff.tool_call_index}, {"function", json::object()}};
                    if (!diff.tool_call_delta.id.empty()) { tool["id"] = diff.tool_call_delta.id; tool["type"] = "function"; }
                    if (!diff.tool_call_delta.name.empty()) tool["function"]["name"] = diff.tool_call_delta.name;
                    if (!diff.tool_call_delta.arguments.empty()) tool["function"]["arguments"] = diff.tool_call_delta.arguments;
                    d["tool_calls"] = json::array({tool});
                }
                if (!d.empty() && !delta(d)) return false;
            }
            return true;
        };
        try {
            send(request.command);
            for (;;) {
                if (stopping) throw std::runtime_error("Strata backend is stopping");
                if (cancelled()) stop();
                std::string line;
                if (!next(line)) {
                    if (Clock::now() - heard > std::chrono::minutes(5)) throw std::runtime_error("Strata engine protocol timed out");
                    if (delta && !sent_stop && Clock::now() - heartbeat > std::chrono::seconds(5)) {
                        if (!delta(json::object())) stop();
                        heartbeat = Clock::now();
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                heard = Clock::now();
                if (line.rfind("ERR", 0) == 0) throw std::runtime_error("Strata engine: " + line);
                if (line.rfind("DONE ", 0) == 0) {
                    std::istringstream done(line);
                    std::string tag;
                    if (!(done >> tag >> completion_tokens >> prompt_tokens >> prompt_ms >> decode_ms >> finish
                               >> accepted >> offered >> cached_tokens)) throw std::runtime_error("Malformed Strata DONE line");
                    if (!std::isfinite(prompt_ms) || !std::isfinite(decode_ms) || prompt_ms < 0 || decode_ms < 0 ||
                        accepted < 0 || offered < accepted ||
                        completion_tokens < 0 || prompt_tokens < 0 || cached_tokens < 0 ||
                        cached_tokens > prompt_tokens || completion_tokens > request.max_new ||
                        (finish != "stop" && finish != "eos" && finish != "length" && finish != "cancel" && finish != "nonfinite")) {
                        throw std::runtime_error("Invalid Strata completion accounting or finish reason");
                    }
                    if (finish == "nonfinite") throw std::runtime_error("Strata engine produced non-finite logits");
                    done_received = true;
                    break;
                }
                if (line.rfind("T ", 0) != 0 || sent_stop) continue;
                size_t used = 0;
                const auto token = std::stoi(line.substr(2), &used);
                const auto* vocab = llama_model_get_vocab(model.get());
                if (used != line.size() - 2 || token < 0 || token >= llama_vocab_n_tokens(vocab)) {
                    throw std::runtime_error("Invalid Strata token ID");
                }
                if (llama_vocab_is_eog(vocab, token)) continue;
                std::vector<char> piece(256);
                int n = llama_token_to_piece(vocab, token, piece.data(), static_cast<int>(piece.size()), 0, true);
                if (n < 0) {
                    piece.resize(static_cast<size_t>(-n));
                    n = llama_token_to_piece(vocab, token, piece.data(), static_cast<int>(piece.size()), 0, true);
                }
                if (n < 0) throw std::runtime_error("Strata detokenization failed");
                output.append(piece.data(), static_cast<size_t>(n));
                for (const auto& s : request.stops) {
                    const auto at = output.find(s);
                    if (at != std::string::npos) { output.resize(at); matched_stop = true; stop(); break; }
                }
                if (delta) {
                    const auto message = parse(true);
                    if (!emit(message)) stop();
                    previous = message;
                }
            }
            const auto message = parse(false);
            if (!sent_stop || matched_stop) emit(message);
            const auto content = json::parse(message.to_json_oaicompat().dump());
            const double decode_tps = decode_ms > 0 ? completion_tokens * 1000.0 / decode_ms : 0.0;
            return {{"message", content}, {"finish_reason", !message.tool_calls.empty() ? "tool_calls" :
                finish == "length" && !matched_stop ? "length" : "stop"},
                {"timings", {{"prompt_n", prompt_tokens - cached_tokens}, {"cache_n", cached_tokens}, {"prompt_ms", prompt_ms},
                    {"prompt_per_second", prompt_ms > 0 ? (prompt_tokens - cached_tokens) * 1000.0 / prompt_ms : 0.0},
                    {"predicted_n", completion_tokens}, {"predicted_ms", decode_ms}, {"predicted_per_second", decode_tps},
                    {"draft_n", offered}, {"draft_n_accepted", accepted}}},
                {"usage", {{"prompt_tokens", prompt_tokens}, {"completion_tokens", completion_tokens},
                    {"total_tokens", prompt_tokens + completion_tokens}, {"prompt_tokens_details", {{"cached_tokens", cached_tokens}}},
                    {"prefill_duration_ttft", prompt_ms / 1000.0}, {"decoding_speed_tps", decode_tps}}}};
        } catch (const std::exception&) {
            if (!done_received) {
                ready = false;
                std::lock_guard<std::mutex> guard(lifecycle);
                if (created) subprocess_terminate(&child);
            }
            throw;
        }
    }

    json handle(const json& body, bool responses, const std::function<bool()>& cancelled,
                httplib::DataSink* target = nullptr,
                const std::function<void(const json&)>& complete = {}) {
            std::unique_lock<std::timed_mutex> lock(generation, std::defer_lock);
            while (!lock.try_lock_for(std::chrono::milliseconds(25))) {
                if (stopping || cancelled()) throw std::runtime_error("Strata request cancelled while queued");
            }
            if (!alive() || cancelled()) throw std::runtime_error("Strata request cancelled or engine unavailable");
            TemporaryFile embeddings{vision ? vision->directory / "request.sve" : std::filesystem::path{}};
            const auto release = [&] {
                embeddings.cleanup();
                if (lock.owns_lock()) lock.unlock();
            };
            Request request;
            try {
                request = prepare(responses ? chat_request(body) : body, cancelled);
            } catch (const common_json_error& e) {
                throw std::invalid_argument(e.what());
            } catch (const json::exception& e) {
                throw std::invalid_argument(e.what());
            }
            const auto id = new_id(responses ? "resp_" : "chatcmpl_");
            const auto created_at = now();
            const auto envelope = [this, id, created_at](const json& result) {
                return json{{"id", id}, {"object", "chat.completion"}, {"created", created_at}, {"model", model_name},
                    {"choices", json::array({{{"index", 0}, {"message", result.at("message")},
                        {"finish_reason", result.at("finish_reason")}}})}, {"usage", result.at("usage")},
                    {"timings", result.at("timings")}};
            };
            const auto response = [this, id, created_at](const json& result) {
                const auto& u = result.at("usage");
                const bool incomplete = result.at("finish_reason") == "length";
                auto output = response_output(result.at("message"), id);
                if (incomplete) {
                    for (auto& item : output) if (item.contains("status")) item["status"] = "incomplete";
                }
                return json{{"id", id}, {"object", "response"}, {"created_at", created_at}, {"model", model_name},
                    {"status", incomplete ? "incomplete" : "completed"},
                    {"incomplete_details", incomplete ? json{{"reason", "max_output_tokens"}} : json(nullptr)},
                    {"output", output}, {"timings", result.at("timings")},
                    {"usage", {{"input_tokens", u.at("prompt_tokens")}, {"output_tokens", u.at("completion_tokens")},
                        {"total_tokens", u.at("total_tokens")}}}};
            };
            if (!target) {
                const auto result = generate(request, cancelled);
                release();
                return responses ? response(result) : envelope(result);
            }
            [this, request = std::move(request), id, created_at, cancelled, responses, response, complete, &release]
                (httplib::DataSink& sink) {
                    bool writable = true;
                    const auto write = [&](const json& event, const std::string& name = "") {
                        const auto data = (name.empty() ? "" : "event: " + name + "\n") + "data: " + event.dump() + "\n\n";
                        writable = writable && sink.write(data.data(), data.size());
                        return writable;
                    };
                    const auto chunk = [&](const json& d, const json& finish = nullptr) {
                        return json{{"id", id}, {"object", "chat.completion.chunk"}, {"created", created_at},
                            {"model", model_name}, {"choices", json::array({{{"index", 0}, {"delta", d}, {"finish_reason", finish}}})}};
                    };
                    int sequence = 0;
                    bool stream_open = true;
                    const auto event = [&](const std::string& type, json data) {
                        data["type"] = type;
                        data["sequence_number"] = sequence++;
                        stream_open = stream_open && write(data, type);
                        return stream_open;
                    };
                    json items = json::array();
                    size_t text_index = std::string::npos, reasoning_index = std::string::npos;
                    std::map<size_t, size_t> tool_indexes;
                    const auto add_item = [&](json item) {
                        const auto index = items.size();
                        items.push_back(std::move(item));
                        event("response.output_item.added", {{"output_index", index}, {"item", items[index]}});
                        return index;
                    };
                    try {
                        if (responses) {
                            const json initial = {{"id", id}, {"object", "response"}, {"created_at", created_at},
                                {"model", model_name}, {"status", "in_progress"}, {"output", json::array()}};
                            event("response.created", {{"response", initial}});
                            event("response.in_progress", {{"response", initial}});
                        }
                        else write(chunk({{"role", "assistant"}, {"content", ""}}));
                        const auto result = generate(request, [&] {
                            return cancelled() || !writable || (sink.is_writable && !sink.is_writable());
                        }, [&](const json& d) {
                            if (d.empty()) return sink.write(": keep-alive\n\n", 14);
                            if (!responses) return write(chunk(d));
                            if (d.contains("reasoning_content")) {
                                if (reasoning_index == std::string::npos) {
                                    reasoning_index = add_item({{"type", "reasoning"}, {"id", "rs_" + id}, {"summary", json::array()}});
                                    items[reasoning_index]["summary"].push_back({{"type", "summary_text"}, {"text", ""}});
                                    event("response.reasoning_summary_part.added", {{"item_id", "rs_" + id},
                                        {"output_index", reasoning_index}, {"summary_index", 0}, {"part", items[reasoning_index]["summary"][0]}});
                                }
                                auto& text = items[reasoning_index]["summary"][0]["text"].get_ref<std::string&>();
                                text += d["reasoning_content"].get<std::string>();
                                event("response.reasoning_summary_text.delta", {{"item_id", "rs_" + id},
                                    {"output_index", reasoning_index}, {"summary_index", 0}, {"delta", d["reasoning_content"]}});
                            }
                            if (d.contains("content")) {
                                if (text_index == std::string::npos) {
                                    text_index = add_item({{"type", "message"}, {"id", "msg_" + id}, {"status", "in_progress"},
                                        {"role", "assistant"}, {"content", json::array()}});
                                    items[text_index]["content"].push_back({{"type", "output_text"}, {"text", ""}, {"annotations", json::array()}});
                                    event("response.content_part.added", {{"item_id", "msg_" + id}, {"output_index", text_index},
                                        {"content_index", 0}, {"part", items[text_index]["content"][0]}});
                                }
                                auto& text = items[text_index]["content"][0]["text"].get_ref<std::string&>();
                                text += d["content"].get<std::string>();
                                event("response.output_text.delta", {{"item_id", "msg_" + id}, {"output_index", text_index},
                                    {"content_index", 0}, {"delta", d["content"]}, {"logprobs", json::array()}});
                            }
                            for (const auto& tool : d.value("tool_calls", json::array())) {
                                const auto index = tool.at("index").get<size_t>();
                                const auto& function = tool.at("function");
                                auto found = tool_indexes.find(index);
                                if (found == tool_indexes.end()) {
                                    const auto call_id = tool.at("id").get<std::string>();
                                    const auto output_index = add_item({{"type", "function_call"}, {"id", "fc_" + call_id},
                                        {"call_id", call_id}, {"name", function.value("name", "")},
                                        {"arguments", ""}, {"status", "in_progress"}});
                                    found = tool_indexes.emplace(index, output_index).first;
                                } else if (function.contains("name")) {
                                    items[found->second]["name"].get_ref<std::string&>() += function["name"].get<std::string>();
                                }
                                if (function.contains("arguments")) {
                                    auto& item = items[found->second];
                                    item["arguments"].get_ref<std::string&>() += function["arguments"].get<std::string>();
                                    event("response.function_call_arguments.delta", {{"item_id", item["id"]},
                                        {"output_index", found->second}, {"delta", function["arguments"]}});
                                }
                            }
                            return stream_open;
                        });
                        release();
                        if (responses) {
                            auto final = response(result);
                            for (const auto& completed : final["output"]) {
                                auto found = std::find_if(items.begin(), items.end(), [&](const auto& item) {
                                    return item.at("id") == completed.at("id");
                                });
                                if (found == items.end()) {
                                    auto initial = completed;
                                    if (initial.contains("status")) initial["status"] = "in_progress";
                                    if (initial.contains("content")) initial["content"] = json::array();
                                    const auto index = add_item(initial);
                                    if (completed["type"] == "message") {
                                        event("response.content_part.added", {{"item_id", completed["id"]}, {"output_index", index},
                                            {"content_index", 0}, {"part", completed["content"][0]}});
                                    }
                                    items[index] = completed;
                                } else *found = completed;
                            }
                            for (size_t i = 0; i < items.size(); ++i) {
                                const auto& item = items[i];
                                if (item["type"] == "message") {
                                    const auto& part = item["content"][0];
                                    event("response.output_text.done", {{"item_id", item["id"]}, {"output_index", i},
                                        {"content_index", 0}, {"text", part["text"]}, {"logprobs", json::array()}});
                                    event("response.content_part.done", {{"item_id", item["id"]}, {"output_index", i},
                                        {"content_index", 0}, {"part", part}});
                                } else if (item["type"] == "reasoning") {
                                    const auto& part = item["summary"][0];
                                    event("response.reasoning_summary_text.done", {{"item_id", item["id"]}, {"output_index", i},
                                        {"summary_index", 0}, {"text", part["text"]}});
                                    event("response.reasoning_summary_part.done", {{"item_id", item["id"]}, {"output_index", i},
                                        {"summary_index", 0}, {"part", part}});
                                } else if (item["type"] == "function_call") {
                                    event("response.function_call_arguments.done", {{"item_id", item["id"]}, {"output_index", i},
                                        {"name", item["name"]}, {"arguments", item["arguments"]}});
                                }
                                event("response.output_item.done", {{"output_index", i}, {"item", item}});
                            }
                            final["output"] = items;
                            event(final["status"] == "incomplete" ? "response.incomplete" : "response.completed", {{"response", final}});
                        } else {
                            auto final_chunk = chunk(json::object(), result.at("finish_reason"));
                            final_chunk["timings"] = result.at("timings");
                            write(final_chunk);
                            if (request.body.value("stream_options", json::object()).value("include_usage", true)) {
                                write({{"id", id}, {"object", "chat.completion.chunk"}, {"created", created_at},
                                    {"model", model_name}, {"choices", json::array()}, {"usage", result.at("usage")},
                                    {"timings", result.at("timings")}});
                            }
                            if (writable) sink.write("data: [DONE]\n\n", 14);
                        }
                        if (complete) complete(result);
                    } catch (const std::exception& e) {
                        release();
                        LOG(ERROR, "Strata") << "Streaming inference failed: " << e.what() << std::endl;
                        const auto error = error_body(e.what(), "backend_error");
                        write(error, responses ? "error" : "");
                        if (complete) complete(error);
                    }
                    if (sink.done) sink.done();
                }(*target);
            return nullptr;
    }
};

StrataProcess::StrataProcess(std::filesystem::path vocabulary, std::string model_name,
                             std::optional<VisionCommand> vision, std::function<bool()> load_cancelled)
    : ServerProcess({false, false}), impl_(std::make_unique<Impl>()) {
    impl_->vocabulary = std::move(vocabulary);
    impl_->model_name = std::move(model_name);
    impl_->load_cancelled = std::move(load_cancelled);
    if (vision) impl_->vision = std::make_unique<VisionEncoder>(std::move(*vision), impl_->load_cancelled);
}

StrataProcess::~StrataProcess() { stop(); }

std::string StrataProcess::start(const ServerCommand& command) {
    auto params = llama_model_default_params();
    params.vocab_only = true;
    impl_->model.reset(llama_model_load_from_file(utils::path_to_utf8(impl_->vocabulary).c_str(), params));
    if (!impl_->model) throw std::runtime_error("Could not load Strata native tokenizer");
    impl_->templates = common_chat_templates_init(impl_->model.get(), "");
    command_line_ = {command.program};
    command_line_.insert(command_line_.end(), command.args.begin(), command.args.end());
    if (impl_->vision) {
        impl_->vision->start([&] { return impl_->stopping.load(); });
    }
    // Both pipe protocols use unquoted filenames; a shared cwd keeps them valid even under a path with spaces.
    impl_->launch(command, impl_->vision ? impl_->vision->directory : utils::path_from_utf8(command.program).parent_path());
#ifdef _WIN32
    handle_ = {impl_->child.hProcess, static_cast<int>(GetProcessId(impl_->child.hProcess))};
#else
    handle_ = {nullptr, impl_->child.child};
#endif
    LOG(INFO, "Strata") << "Started native engine, PID " << handle_.pid << std::endl;
    try {
        const auto deadline = Clock::now() + std::chrono::minutes(15);
        while (!impl_->ready) {
            if (impl_->load_cancelled && impl_->load_cancelled()) throw std::runtime_error("Strata load cancelled");
            std::string line;
            if (impl_->next(line)) {
                if (line.rfind("ERR", 0) == 0) throw std::runtime_error("Strata startup: " + line);
                if (line.rfind("READY ", 0) == 0) {
                    std::istringstream input(line);
                    std::string tag, stop_support;
                    input >> tag >> impl_->context >> stop_support;
                    if (!input || impl_->context <= 8 || stop_support != "stop") {
                        throw std::runtime_error("Strata engine lacks the required cancellable pipe protocol");
                    }
                    impl_->ready = true;
                }
            } else std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (Clock::now() >= deadline) throw std::runtime_error("Strata engine startup timed out");
        }
    } catch (const std::exception&) {
        stop();
        throw;
    }
    return "";
}

void StrataProcess::stop() {
    impl_->stopping = true;
    impl_->ready = false;
    impl_->terminate();
    if (impl_->vision) impl_->vision->terminate();
    std::lock_guard<std::timed_mutex> generation(impl_->generation);
    impl_->close();
    if (impl_->vision) impl_->vision->close();
    handle_ = {nullptr, 0};
}

bool StrataProcess::running() const { return impl_->ready && impl_->alive(); }

json StrataProcess::request(const json& body, bool responses, const std::function<bool()>& cancelled) {
    return impl_->handle(body, responses, cancelled);
}

void StrataProcess::stream(const json& body, bool responses, httplib::DataSink& sink,
                          const std::function<bool()>& cancelled,
                          const std::function<void(const json&)>& complete) {
    impl_->handle(body, responses, cancelled, &sink, complete);
}

}
