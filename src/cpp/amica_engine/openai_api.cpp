// httplib first: it includes winsock2.h, which must precede windows.h.
#include <httplib.h>

#include "openai_api.h"

#include "bundle.h"
#include "chat_frontend.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <thread>

namespace amica_engine {

namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

constexpr std::size_t kDefaultMaxNewTokens = 256;

void log(const std::string& message) {
    std::cerr << "[amica-engine] " << message << "\n";
    std::cerr.flush();
}

double milliseconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
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

std::string random_id(const char* prefix) {
    static std::mutex mutex;
    static std::mt19937_64 random(std::random_device{}());
    std::lock_guard<std::mutex> lock(mutex);
    char text[40];
    std::snprintf(text, sizeof(text), "%016llx%016llx", static_cast<unsigned long long>(random()),
                  static_cast<unsigned long long>(random()));
    return prefix + std::string(text);
}

// Turns token IDs into text deltas: each delta is the growth of the decoded
// text, held back while the text ends in an incomplete UTF-8 sequence so a
// multi-byte character is never split across deltas.
class TextStream {
public:
    explicit TextStream(const Tokenizer& tokenizer) : tokenizer_(&tokenizer) {}

    std::string push(std::int64_t token) {
        ids_.push_back(token);
        const std::string text = tokenizer_->decode(ids_);
        static const std::string kReplacement = "\xEF\xBF\xBD";
        if (text.size() >= kReplacement.size() &&
            text.compare(text.size() - kReplacement.size(), kReplacement.size(), kReplacement) == 0) {
            return "";
        }
        return advance(text);
    }

    std::string finish() { return ids_.empty() ? "" : advance(tokenizer_->decode(ids_)); }

private:
    std::string advance(const std::string& text) {
        std::string delta = text.size() > text_.size() ? text.substr(text_.size()) : "";
        text_ = text;
        return delta;
    }

    const Tokenizer* tokenizer_;
    std::vector<std::int64_t> ids_;
    std::string text_;
};

struct Delta {
    std::string field;  // "content" or "reasoning_content"
    std::string text;
};

class ReplyStream {
public:
    ReplyStream(const Tokenizer& tokenizer, const std::set<std::int64_t>& stop_ids)
        : tokenizer_(tokenizer), stop_ids_(stop_ids), text_(tokenizer) {}
    virtual ~ReplyStream() = default;

    virtual std::vector<Delta> push(std::int64_t token) {
        if (stop_ids_.count(token)) {
            stopped_ = true;
            return {};
        }
        return emit("content", text_.push(token));
    }

    virtual std::vector<Delta> finish() { return emit("content", text_.finish()); }

    bool stopped() const { return stopped_; }

protected:
    static std::vector<Delta> emit(const std::string& field, const std::string& text) {
        if (text.empty()) return {};
        return {{field, text}};
    }

    const Tokenizer& tokenizer_;
    std::set<std::int64_t> stop_ids_;
    TextStream text_;
    bool stopped_ = false;
};

// A Harmony reply is a series of messages, each a header naming its channel
// and a body: <|channel|>analysis<|message|>...<|end|><|start|>assistant
// <|channel|>final<|message|>... The final channel is the answer; every other
// channel is reasoning. Control tokens never reach the client.
class HarmonyReplyStream : public ReplyStream {
public:
    HarmonyReplyStream(const Tokenizer& tokenizer, const std::set<std::int64_t>& stop_ids)
        : ReplyStream(tokenizer, stop_ids) {
        for (const char* name : {"<|start|>", "<|channel|>", "<|constrain|>", "<|message|>", "<|end|>", "<|call|>",
                                 "<|return|>"}) {
            const auto ids = tokenizer.encode(name);
            if (ids.size() == 1) control_[ids[0]] = name;
        }
    }

    std::vector<Delta> push(std::int64_t token) override {
        if (stop_ids_.count(token)) {
            stopped_ = true;
            return {};
        }
        const auto control = control_.find(token);
        if (control == control_.end()) {
            if (field_.empty()) {
                header_.push_back(token);
                return {};
            }
            return emit(field_, text_.push(token));
        }

        std::vector<Delta> deltas = finish();
        field_.clear();
        if (control->second == "<|message|>") {
            const std::string header = tokenizer_.decode(header_);
            const std::size_t channel = header.rfind("<|channel|>");
            const std::string rest = channel == std::string::npos ? header : header.substr(channel + 11);
            const std::size_t begin = rest.find_first_not_of(" \t\r\n");
            const std::string name =
                begin == std::string::npos ? "" : rest.substr(begin, rest.find_first_of(" \t\r\n", begin) - begin);
            field_ = name == "final" ? "content" : "reasoning_content";
            text_ = TextStream(tokenizer_);
            header_.clear();
        } else {
            header_.push_back(token);
        }
        return deltas;
    }

    std::vector<Delta> finish() override {
        if (field_.empty()) return {};
        return emit(field_, text_.finish());
    }

private:
    std::map<std::int64_t, std::string> control_;
    std::vector<std::int64_t> header_;
    std::string field_;
};

std::string message_text(const json& content) {
    if (content.is_null()) return "";
    if (content.is_string()) return content.get<std::string>();
    if (content.is_array()) {
        std::string text;
        for (const auto& part : content) {
            if (part.is_object() && (part.value("type", "") == "text" || part.value("type", "") == "input_text")) {
                const json value = part.value("text", json(""));
                text += value.is_string() ? value.get<std::string>() : value.dump();
            } else if (part.is_string()) {
                text += part.get<std::string>();
            } else {
                throw RequestError("Only text message content is supported by AMICA");
            }
        }
        return text;
    }
    throw RequestError("Message content must be a string or a list of text parts");
}

std::vector<ChatMessage> chat_messages(const json& body) {
    const json messages = body.value("messages", json());
    if (!messages.is_array() || messages.empty()) {
        throw RequestError("'messages' must be a non-empty array");
    }
    std::vector<ChatMessage> result;
    bool has_user = false;
    for (const auto& message : messages) {
        if (!message.is_object()) throw RequestError("Each message must be an object");
        std::string role = message.value("role", json()).is_string() ? message.at("role").get<std::string>() : "";
        if (role == "developer") role = "system";
        if (role != "system" && role != "user" && role != "assistant") continue;
        has_user = has_user || role == "user";
        result.push_back({role, message_text(message.value("content", json()))});
    }
    if (!has_user) throw RequestError("The conversation must contain a user message");
    return result;
}

std::optional<long long> requested_max_tokens(const json& body) {
    for (const char* key : {"max_completion_tokens", "max_tokens"}) {
        const json value = body.value(key, json());
        if (value.is_null()) continue;
        if (!value.is_number_integer()) throw RequestError(std::string("'") + key + "' must be an integer");
        return value.get<long long>();
    }
    return std::nullopt;
}

bool truthy(const json& value) {
    if (value.is_null()) return false;
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_number()) return value.get<double>() != 0.0;
    if (value.is_string()) return !value.get<std::string>().empty();
    return !value.empty();
}

struct Request {
    std::vector<std::int64_t> prompt;
    std::size_t max_new_tokens;
    bool chat;
};

struct Outcome {
    std::string finish_reason;
    ordered_json usage;
    ordered_json timings;
};

class Api {
public:
    Api(std::string model_name, const Bundle& bundle, const ChatFrontend& frontend, Generator& generator)
        : model_name_(std::move(model_name)), bundle_(bundle), frontend_(frontend), generator_(generator) {}

    void health(const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok"})", "application/json");
    }

    void models(const httplib::Request&, httplib::Response& res) {
        const ordered_json body = {
            {"object", "list"},
            {"data", ordered_json::array({{{"id", model_name_}, {"object", "model"}, {"owned_by", "amica"}}})}};
        res.set_content(dump(body), "application/json");
    }

    void chat_completion(const httplib::Request& req, httplib::Response& res) {
        handle(res, [&] {
            const json body = parse_body(req);
            const auto messages = chat_messages(body);
            const auto prompt = fit_messages(messages);
            const Request request{prompt, token_budget(prompt.size(), requested_max_tokens(body)), true};
            const std::string id = random_id("chatcmpl-");
            const auto created = static_cast<long long>(std::time(nullptr));
            const auto chunk = [this, id, created](const ordered_json& delta, const ordered_json& finish_reason) {
                return ordered_json{{"id", id},
                                    {"object", "chat.completion.chunk"},
                                    {"created", created},
                                    {"model", model_name_},
                                    {"choices", ordered_json::array({{{"index", 0},
                                                                      {"delta", delta},
                                                                      {"finish_reason", finish_reason}}})}};
            };
            if (truthy(body.value("stream", json()))) {
                stream(res, request, [chunk, first = true](const Delta* delta, const ordered_json& finish) mutable {
                    ordered_json fields = ordered_json::object();
                    if (delta) {
                        fields[delta->field] = delta->text;
                        if (first) {
                            fields["role"] = "assistant";
                            first = false;
                        }
                    }
                    return chunk(fields, finish);
                });
                return;
            }
            std::map<std::string, std::string> fields;
            const Outcome outcome = run(request, [&](const Delta& delta) {
                fields[delta.field] += delta.text;
                return true;
            });
            ordered_json message = {{"role", "assistant"}, {"content", fields["content"]}};
            if (!fields["reasoning_content"].empty()) message["reasoning_content"] = fields["reasoning_content"];
            ordered_json response = {{"id", id},
                                     {"object", "chat.completion"},
                                     {"created", created},
                                     {"model", model_name_},
                                     {"choices", ordered_json::array({{{"index", 0},
                                                                       {"message", message},
                                                                       {"finish_reason", outcome.finish_reason}}})}};
            response["usage"] = outcome.usage;
            response["timings"] = outcome.timings;
            res.set_content(dump(response), "application/json");
        });
    }

    void completion(const httplib::Request& req, httplib::Response& res) {
        handle(res, [&] {
            const json body = parse_body(req);
            json prompt_value = body.value("prompt", json());
            if (prompt_value.is_array() && prompt_value.size() == 1) prompt_value = prompt_value[0];
            if (!prompt_value.is_string() || prompt_value.get<std::string>().empty()) {
                throw RequestError("'prompt' must be a non-empty string");
            }
            const auto prompt = frontend_.encode_text(prompt_value.get<std::string>());
            if (prompt.size() > bundle_.max_prompt_tokens()) {
                throw RequestError(prompt_too_long(prompt.size()));
            }
            const Request request{prompt, token_budget(prompt.size(), requested_max_tokens(body)), false};
            const std::string id = random_id("cmpl-");
            const auto created = static_cast<long long>(std::time(nullptr));
            const auto chunk = [this, id, created](const std::string& text, const ordered_json& finish_reason) {
                return ordered_json{
                    {"id", id},
                    {"object", "text_completion"},
                    {"created", created},
                    {"model", model_name_},
                    {"choices",
                     ordered_json::array({{{"index", 0}, {"text", text}, {"finish_reason", finish_reason}}})}};
            };
            if (truthy(body.value("stream", json()))) {
                stream(res, request, [chunk](const Delta* delta, const ordered_json& finish) {
                    return chunk(delta ? delta->text : "", finish);
                });
                return;
            }
            std::string text;
            const Outcome outcome = run(request, [&](const Delta& delta) {
                text += delta.text;
                return true;
            });
            ordered_json response = chunk(text, outcome.finish_reason);
            response["usage"] = outcome.usage;
            response["timings"] = outcome.timings;
            res.set_content(dump(response), "application/json");
        });
    }

private:
    using ChunkBuilder = std::function<ordered_json(const Delta*, const ordered_json&)>;

    template <typename Body>
    void handle(httplib::Response& res, Body body) {
        try {
            body();
        } catch (const RequestError& error) {
            send_error(res, error.what(), error.status);
        } catch (const json::exception& error) {
            send_error(res, std::string("Invalid request: ") + error.what(), 400);
        } catch (const std::exception& error) {
            log(std::string("request failed: ") + error.what());
            send_error(res, error.what(), 500);
        }
    }

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

    std::string prompt_too_long(std::size_t tokens) const {
        return "The prompt is " + std::to_string(tokens) + " tokens, but this AMICA bundle accepts at most " +
               std::to_string(bundle_.max_prompt_tokens()) + " prompt tokens";
    }

    std::vector<std::int64_t> encode_messages(const std::vector<ChatMessage>& messages) const {
        if (bundle_.chat_template()) return frontend_.encode_messages(messages);
        std::string text;
        for (std::size_t i = 0; i < messages.size(); ++i) {
            text += (i ? "\n" : "") + messages[i].content;
        }
        return frontend_.encode_text(text);
    }

    // Tokenizes the conversation, dropping the oldest turns until it fits prefill.
    std::vector<std::int64_t> fit_messages(std::vector<ChatMessage> messages) const {
        while (true) {
            auto ids = encode_messages(messages);
            if (ids.size() <= bundle_.max_prompt_tokens()) return ids;
            std::vector<std::size_t> droppable;
            for (std::size_t i = 0; i < messages.size(); ++i) {
                if (messages[i].role != "system") droppable.push_back(i);
            }
            if (droppable.size() <= 1) throw RequestError(prompt_too_long(ids.size()));
            messages.erase(messages.begin() + static_cast<std::ptrdiff_t>(droppable[0]));
        }
    }

    std::size_t token_budget(std::size_t prompt_tokens, std::optional<long long> requested) const {
        const auto capacity = bundle_.context_capacity();
        if (capacity && *capacity <= prompt_tokens) {
            throw RequestError("The prompt fills this AMICA bundle's " + std::to_string(*capacity) +
                               "-token context");
        }
        const std::size_t limit = capacity ? *capacity - prompt_tokens : kDefaultMaxNewTokens;
        if (!requested) return limit;
        if (*requested < 1) throw RequestError("max_tokens must be at least 1");
        return std::min(static_cast<std::size_t>(*requested), limit);
    }

    Outcome run(const Request& request, const std::function<bool(const Delta&)>& on_delta) {
        const std::set<std::int64_t>& stop_ids = request.chat ? frontend_.chat_stop_ids() : frontend_.eos_ids();
        std::unique_ptr<ReplyStream> reply =
            request.chat && frontend_.harmony()
                ? std::make_unique<HarmonyReplyStream>(frontend_.tokenizer(), stop_ids)
                : std::make_unique<ReplyStream>(frontend_.tokenizer(), stop_ids);
        const auto started = std::chrono::steady_clock::now();
        Generation generation;
        {
            std::lock_guard<std::mutex> lock(generation_mutex_);
            try {
                generation = generator_.generate(request.prompt, request.max_new_tokens, stop_ids,
                                                 [&](std::int64_t token) {
                                                     bool keep = true;
                                                     for (const auto& delta : reply->push(token)) {
                                                         keep = on_delta(delta) && keep;
                                                     }
                                                     return keep;
                                                 });
            } catch (const std::exception& error) {
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
        for (const auto& delta : reply->finish()) on_delta(delta);

        Outcome outcome;
        outcome.finish_reason = reply->stopped() ? "stop" : "length";
        const std::size_t completion_tokens = generation.ids.size();
        outcome.usage = {{"prompt_tokens", request.prompt.size()},
                         {"completion_tokens", completion_tokens},
                         {"total_tokens", request.prompt.size() + completion_tokens}};
        outcome.timings = {{"prompt_n", request.prompt.size()},
                           {"prompt_ms", generation.ttft_ms},
                           {"predicted_n", completion_tokens},
                           {"predicted_ms", generation.decode_ms}};
        if (generation.tokens_per_second > 0.0) {
            outcome.timings["predicted_per_second"] = generation.tokens_per_second;
        }
        char line[256];
        std::snprintf(line, sizeof(line),
                      "prompt_tokens=%zu generated_tokens=%zu finish=%s ttft_ms=%.1f tps=%.1f request_ms=%.0f",
                      request.prompt.size(), completion_tokens, outcome.finish_reason.c_str(), generation.ttft_ms,
                      generation.tokens_per_second, milliseconds_since(started));
        log(line);
        return outcome;
    }

    void stream(httplib::Response& res, const Request& request, ChunkBuilder chunk) {
        res.set_header("Cache-Control", "no-cache");
        res.set_chunked_content_provider(
            "text/event-stream", [this, request, chunk](std::size_t, httplib::DataSink& sink) mutable {
                const auto send = [&](const std::string& data) {
                    const std::string event = "data: " + data + "\n\n";
                    return sink.is_writable() && sink.write(event.data(), event.size());
                };
                try {
                    const Outcome outcome = run(request, [&](const Delta& delta) {
                        return send(dump(chunk(&delta, ordered_json())));
                    });
                    ordered_json final = chunk(nullptr, outcome.finish_reason);
                    final["usage"] = outcome.usage;
                    final["timings"] = outcome.timings;
                    send(dump(final));
                    send("[DONE]");
                } catch (const std::exception& error) {
                    send(dump(ordered_json{{"error", {{"message", error.what()}}}}));
                }
                sink.done();
                return true;
            });
    }

    std::string model_name_;
    const Bundle& bundle_;
    const ChatFrontend& frontend_;
    Generator& generator_;
    std::mutex generation_mutex_;
};

}  // namespace

int serve(const std::string& host, int port, const std::string& model_name, const Bundle& bundle,
          const ChatFrontend& frontend, Generator& generator) {
    Api api(model_name, bundle, frontend, generator);
    httplib::Server server;
    server.Get("/health", [&](const httplib::Request& req, httplib::Response& res) { api.health(req, res); });
    server.Get(R"(/(?:v1/)?models)",
               [&](const httplib::Request& req, httplib::Response& res) { api.models(req, res); });
    server.Post(R"(/(?:v1/)?chat/completions)",
                [&](const httplib::Request& req, httplib::Response& res) { api.chat_completion(req, res); });
    server.Post(R"(/(?:v1/)?completions)",
                [&](const httplib::Request& req, httplib::Response& res) { api.completion(req, res); });
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
