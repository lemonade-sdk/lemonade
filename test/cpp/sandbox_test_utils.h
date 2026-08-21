#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace lemon::test {

namespace fs = std::filesystem;

struct TestResult {
    std::atomic<int> passed{0};
    std::atomic<int> failed{0};
    std::vector<std::string> failure_messages;
    std::mutex mtx;

    void check(bool cond, const std::string& name, const std::string& details = "") {
        if (cond) {
            std::printf("  [PASS] %s\n", name.c_str());
            ++passed;
        } else {
            std::printf("  [FAIL] %s\n", name.c_str());
            if (!details.empty()) {
                std::printf("         Details: %s\n", details.c_str());
            }
            std::lock_guard<std::mutex> lock(mtx);
            failure_messages.push_back(name + (details.empty() ? "" : " (" + details + ")"));
            ++failed;
        }
    }

    void report_summary(const std::string& section) const {
        std::printf("-> Summary for '%s': %d passed, %d failed\n\n",
                    section.c_str(), passed.load(), failed.load());
    }

    int exit_code() const {
        return failed.load() == 0 ? 0 : 1;
    }
};

inline void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream ofs(p, std::ios::binary);
    ofs << content;
}

inline std::string read_file_content(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return "";
    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

inline std::vector<std::string> read_file_lines(const fs::path& p) {
    std::vector<std::string> lines;
    std::ifstream ifs(p);
    std::string line;
    while (std::getline(ifs, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

inline std::unordered_map<std::string, std::string> parse_env_lines(const std::vector<std::string>& lines) {
    std::unordered_map<std::string, std::string> map;
    for (const auto& line : lines) {
        auto pos = line.find('=');
        if (pos != std::string::npos && pos > 0) {
            map[line.substr(0, pos)] = line.substr(pos + 1);
        }
    }
    return map;
}

class TempSandboxFixture {
public:
    TempSandboxFixture(const std::string& prefix = "lemonade_test_") {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        base_dir_ = fs::temp_directory_path() / (prefix + std::to_string(now));

        hf_home_ = base_dir_ / "user_home" / ".cache" / "huggingface";
        hf_hub_ = hf_home_ / "hub";
        hf_dot_home_ = base_dir_ / "user_home" / ".huggingface";

        model_dir_ = hf_hub_ / "models--meta-llama--Llama-3-8B" / "snapshots" / "abc123def456";
        model_file_ = model_dir_ / "model-q4_k_m.gguf";

        custom_hf_home_ = base_dir_ / "custom_hf_home";
        custom_hub_ = custom_hf_home_ / "hub";
        custom_model_dir_ = custom_hub_ / "models--mistralai--Mistral-7B" / "snapshots" / "999888777";
        custom_model_file_ = custom_model_dir_ / "model.gguf";

        allowed_dir_ = base_dir_ / "confinement_allowed";
        writeable_dir_ = base_dir_ / "confinement_writeable";
        forbidden_dir_ = base_dir_ / "confinement_forbidden";
        parent_home_dir_ = base_dir_ / "parent_home";

        fs::create_directories(model_dir_);
        fs::create_directories(hf_dot_home_);
        fs::create_directories(custom_model_dir_);
        fs::create_directories(allowed_dir_);
        fs::create_directories(writeable_dir_);
        fs::create_directories(forbidden_dir_);
        fs::create_directories(parent_home_dir_);

        try {
            base_dir_ = fs::canonical(base_dir_);
            hf_home_ = fs::canonical(hf_home_);
            hf_hub_ = fs::canonical(hf_hub_);
            model_dir_ = fs::canonical(model_dir_);
            allowed_dir_ = fs::canonical(allowed_dir_);
            writeable_dir_ = fs::canonical(writeable_dir_);
            forbidden_dir_ = fs::canonical(forbidden_dir_);
            parent_home_dir_ = fs::canonical(parent_home_dir_);
        } catch (...) {}

        write_file(hf_home_ / "token", "HF_TOKEN_STANDARD_SECRET_KEY_12345");
        write_file(hf_home_ / "stored_tokens", "STORED_TOKEN_HASH_SECRET_67890");
        write_file(hf_home_ / "token.lock", "LOCK_FILE_DUMMY_DATA");
        write_file(hf_dot_home_ / "token", "LEGACY_DOT_HUGGINGFACE_SECRET");
        write_file(custom_hf_home_ / "token", "CUSTOM_HF_HOME_TOKEN_SECRET");

        write_file(model_file_, "GGUF_HEADER_LLAMA_3_8B_WEIGHTS_CONTENT");
        write_file(custom_model_file_, "GGUF_HEADER_MISTRAL_7B_WEIGHTS_CONTENT");

        allowed_file_ = allowed_dir_ / "allowed_corpus.txt";
        write_file(allowed_file_, "READ_ACCESS_GRANTED_PUBLIC_DATA");

        writeable_file_ = writeable_dir_ / "output.log";
        write_file(writeable_file_, "INITIAL_LOG_LINE\n");

        forbidden_canary_ = forbidden_dir_ / "top_secret_canary.txt";
        write_file(forbidden_canary_, "CRITICAL_CONFIDENTIAL_USER_PASSWORD_HASH");

        parent_canary_ = parent_home_dir_ / "id_rsa";
        write_file(parent_canary_, "-----BEGIN OPENSSH PRIVATE KEY-----\nMOCK_KEY\n-----END OPENSSH PRIVATE KEY-----");
    }

    ~TempSandboxFixture() {
        std::error_code ec;
        fs::remove_all(base_dir_, ec);
    }

    fs::path base() const { return base_dir_; }
    fs::path root() const { return base_dir_; }
    fs::path user_home() const { return base_dir_ / "user_home"; }
    fs::path hf_home() const { return hf_home_; }
    fs::path hf_hub() const { return hf_hub_; }
    fs::path hf_token() const { return hf_home_ / "token"; }
    fs::path hf_stored_tokens() const { return hf_home_ / "stored_tokens"; }
    fs::path hf_token_lock() const { return hf_home_ / "token.lock"; }
    fs::path hf_dot_token() const { return hf_dot_home_ / "token"; }
    fs::path model_dir() const { return model_dir_; }
    fs::path model_file() const { return model_file_; }
    fs::path hf_model() const { return model_file_; }

    fs::path custom_hf_home() const { return custom_hf_home_; }
    fs::path custom_token() const { return custom_hf_home_ / "token"; }
    fs::path custom_model_file() const { return custom_model_file_; }

    fs::path allowed_dir() const { return allowed_dir_; }
    fs::path allowed_file() const { return allowed_file_; }
    fs::path writeable_dir() const { return writeable_dir_; }
    fs::path writeable_file() const { return writeable_file_; }
    fs::path forbidden_dir() const { return forbidden_dir_; }
    fs::path forbidden_canary() const { return forbidden_canary_; }
    fs::path parent_home_dir() const { return parent_home_dir_; }
    fs::path parent_canary() const { return parent_canary_; }

private:
    fs::path base_dir_;
    fs::path hf_home_;
    fs::path hf_hub_;
    fs::path hf_dot_home_;
    fs::path model_dir_;
    fs::path model_file_;
    fs::path custom_hf_home_;
    fs::path custom_hub_;
    fs::path custom_model_dir_;
    fs::path custom_model_file_;
    fs::path allowed_dir_;
    fs::path allowed_file_;
    fs::path writeable_dir_;
    fs::path writeable_file_;
    fs::path forbidden_dir_;
    fs::path forbidden_canary_;
    fs::path parent_home_dir_;
    fs::path parent_canary_;
};

} // namespace lemon::test
