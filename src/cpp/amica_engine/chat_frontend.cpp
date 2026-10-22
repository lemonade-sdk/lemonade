#include "chat_frontend.h"

#include <minja/minja.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace amica_engine {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

std::string read_text(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open " + path.u8string());
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

json load_json(const fs::path& dir, const char* name) {
    const fs::path path = dir / name;
    if (!fs::is_regular_file(path)) return json::object();
    return json::parse(read_text(path));
}

// Token fields hold either the token text or an object with its "content".
std::string token_text(const json& value) {
    if (value.is_object()) return value.value("content", "");
    if (value.is_string()) return value.get<std::string>();
    return "";
}

// Like Python's dict.get: a key that is present wins even when it is null.
json get(const json& object, const char* key, const json& fallback) {
    const auto it = object.find(key);
    return it == object.end() ? fallback : *it;
}

void add_ids(const json& value, std::set<std::int64_t>& ids) {
    if (value.is_number_integer()) {
        ids.insert(value.get<std::int64_t>());
    } else if (value.is_array()) {
        for (const auto& id : value) {
            if (id.is_number_integer()) ids.insert(id.get<std::int64_t>());
        }
    }
}

}  // namespace

ChatFrontend::ChatFrontend(const std::string& model_dir)
    : tokenizer_((fs::u8path(model_dir) / "tokenizer.json").u8string()) {
    const fs::path dir = fs::u8path(model_dir);
    tokenizer_config_ = load_json(dir, "tokenizer_config.json");
    generation_config_ = load_json(dir, "generation_config.json");
    model_config_ = load_json(dir, "config.json");

    const json added_tokens = tokenizer_config_.value("added_tokens_decoder", json::object());
    for (const auto& [id, value] : added_tokens.items()) {
        special_tokens_.insert(token_text(value));
    }
    for (const auto& value : tokenizer_config_.value("extra_special_tokens", json::array())) {
        special_tokens_.insert(token_text(value));
    }
    for (const auto& token : tokenizer_.special_tokens()) {
        special_tokens_.insert(token);
    }
    harmony_ = special_tokens_.count("<|channel|>") && special_tokens_.count("<|message|>") &&
               special_tokens_.count("<|return|>");

    const json configured = get(tokenizer_config_, "chat_template", json());
    if (configured.is_string()) {
        template_text_ = configured.get<std::string>();
    }
    if (template_text_.empty() && (configured.is_null() || configured.is_string()) &&
        fs::is_regular_file(dir / "chat_template.jinja")) {
        template_text_ = read_text(dir / "chat_template.jinja");
    }
    if (!template_text_.empty()) {
        // ChatApp templates mark the assistant reply with {% generation %} tags
        // for training masks; rendering drops them.
        static const std::regex generation_tag(R"(\{%-?\s*(?:end)?generation\s*-?%\})");
        const std::string text = std::regex_replace(template_text_, generation_tag, "");
        template_ = minja::Parser::parse(text, minja::Options{false, false, false});
    }
    resolve_stop_ids();
}

ChatFrontend::~ChatFrontend() = default;

void ChatFrontend::resolve_stop_ids() {
    const json none = json::array();
    add_ids(get(generation_config_, "eos_token_id",
                get(tokenizer_config_, "eos_token_id", get(model_config_, "eos_token_id", none))),
            eos_ids_);
    // Some bundles keep a stale numeric EOS in config.json while
    // tokenizer_config.json names the real token.
    const json added_tokens = tokenizer_config_.value("added_tokens_decoder", json::object());
    const auto add_named = [&](const std::string& name, std::set<std::int64_t>& ids) {
        for (const auto& [id, value] : added_tokens.items()) {
            if (token_text(value) == name) ids.insert(std::stoll(id));
        }
        if (const auto id = tokenizer_.token_to_id(name)) ids.insert(*id);
    };
    const std::string eos_token = token_text(tokenizer_config_.value("eos_token", json()));
    if (!eos_token.empty()) {
        add_named(eos_token, eos_ids_);
    }

    chat_stop_ids_ = eos_ids_;
    std::vector<std::string> terminators;
    if (harmony_) {
        // A Harmony turn can close its analysis message with <|end|> and go on
        // to the final answer, so only <|return|> ends it.
        terminators = {"<|return|>"};
    } else if (!template_text_.empty()) {
        for (const char* token : {"<|end|>", "<|im_end|>"}) {
            if (template_text_.find(token) != std::string::npos) terminators.push_back(token);
        }
    } else if (special_tokens_.count("<|im_start|>") && special_tokens_.count("<|im_end|>")) {
        terminators = {"<|im_end|>"};
    } else {
        terminators = {"<|end|>"};
    }
    for (const auto& terminator : terminators) {
        add_named(terminator, chat_stop_ids_);
    }
}

std::string ChatFrontend::render(const std::vector<ChatMessage>& messages) const {
    if (!template_) {
        std::string system;
        std::string user;
        for (const auto& message : messages) {
            if (message.role == "system" && system.empty()) system = message.content;
            if (message.role == "user") user = message.content;
        }
        if (special_tokens_.count("<|im_start|>") && special_tokens_.count("<|im_end|>")) {
            const std::string head = system.empty() ? "" : "<|im_start|>system\n" + system + "<|im_end|>\n";
            return head + "<|im_start|>user\n" + user + "<|im_end|>\n<|im_start|>assistant\n";
        }
        const std::string head = system.empty() ? "" : "<|system|>\n" + system + "<|end|>\n";
        return head + "<|user|>\n" + user + "<|end|>\n<|assistant|>\n";
    }

    nlohmann::ordered_json list = nlohmann::ordered_json::array();
    for (const auto& message : messages) {
        list.push_back({{"role", message.role}, {"content", message.content}});
    }
    nlohmann::ordered_json variables = {
        {"messages", list},
        {"add_generation_prompt", true},
        {"bos_token", token_text(tokenizer_config_.value("bos_token", json()))},
        {"eos_token", token_text(tokenizer_config_.value("eos_token", json()))},
    };
    auto context = minja::Context::make(minja::Value(variables));
    // Hugging Face gives templates this helper; gpt-oss puts today's date in
    // its system preamble with it.
    context->set("strftime_now", minja::Value::callable([](const std::shared_ptr<minja::Context>&,
                                                           minja::ArgumentsValue& args) {
                     args.expectArgs("strftime_now", {1, 1}, {0, 0});
                     const std::string format = args.args[0].get<std::string>();
                     const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                     std::tm local{};
                     localtime_s(&local, &now);
                     char text[256];
                     const std::size_t size = std::strftime(text, sizeof(text), format.c_str(), &local);
                     return minja::Value(std::string(text, size));
                 }));
    return template_->render(context);
}

std::vector<std::int64_t> ChatFrontend::encode_messages(const std::vector<ChatMessage>& messages) const {
    std::vector<std::int64_t> ids = tokenizer_.encode(render(messages));
    if (tokenizer_config_.value("add_bos_token", false)) {
        json bos = get(generation_config_, "bos_token_id", tokenizer_config_.value("bos_token_id", json()));
        if (bos.is_array()) bos = bos.empty() ? json() : bos[0];
        if (bos.is_number_integer() && (ids.empty() || ids[0] != bos.get<std::int64_t>())) {
            ids.insert(ids.begin(), bos.get<std::int64_t>());
        }
    }
    return ids;
}

}  // namespace amica_engine
