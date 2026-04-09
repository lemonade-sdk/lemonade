#include "lemon_cli/hf_pull.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lemon_cli {

namespace {

using json = nlohmann::json;

std::string url_encode(const std::string& s) {
    std::ostringstream out;
    out << std::hex << std::uppercase << std::setfill('0');
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out << static_cast<char>(c);
        } else {
            out << '%' << std::setw(2) << static_cast<int>(c);
        }
    }
    return out.str();
}

std::string human_size(uint64_t bytes) {
    if (bytes == 0) return "unknown size";
    constexpr double KB = 1024.0;
    constexpr double MB = KB * 1024.0;
    constexpr double GB = MB * 1024.0;
    std::ostringstream out;
    out << std::fixed << std::setprecision(1);
    double b = static_cast<double>(bytes);
    if (b >= GB) out << (b / GB) << " GB";
    else if (b >= MB) out << (b / MB) << " MB";
    else if (b >= KB) out << (b / KB) << " KB";
    else out << bytes << " B";
    return out.str();
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Local copy of the prompt helper from recipe_import.cpp (which keeps it
// translation-unit-local in an anonymous namespace).
void trim(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
}

// Show a numbered list of variants and read either a number (selecting one
// of them) or a free-text variant name (e.g. "UD-IQ2_M" or a full filename),
// which is passed straight through to /v1/pull. Returns the chosen variant
// name in `out`. Returns false on EOF or empty input.
bool prompt_variant(const std::vector<std::string>& labels,
                    const std::vector<std::string>& names,
                    std::string& out) {
    std::cout << "Select a variant:" << std::endl;
    for (size_t i = 0; i < labels.size(); ++i) {
        std::cout << "  " << (i + 1) << ") " << labels[i] << std::endl;
    }
    std::cout << "Enter number, or type any variant name: " << std::flush;

    std::string input;
    if (!std::getline(std::cin, input)) return false;
    trim(input);
    if (input.empty()) {
        std::cerr << "Error: variant is required." << std::endl;
        return false;
    }

    // Try to parse as a number first.
    size_t parsed_chars = 0;
    try {
        int selected = std::stoi(input, &parsed_chars);
        if (parsed_chars == input.size()) {
            if (selected < 1 || static_cast<size_t>(selected) > names.size()) {
                std::cerr << "Error: selection out of range." << std::endl;
                return false;
            }
            out = names[selected - 1];
            return true;
        }
    } catch (const std::exception&) {
        // Not a number — fall through to free-text path.
    }

    out = input;
    return true;
}

bool prompt_model_name(const std::string& default_name, std::string& out) {
    std::cout << "Choose a model name." << std::endl;
    std::cout << "Press enter to use the default: user." << default_name << std::endl;
    std::cout << "Or type a name starting with \"user.\" and press enter:" << std::endl;
    std::cout << "> " << std::flush;

    std::string input;
    if (!std::getline(std::cin, input)) return false;
    trim(input);

    out = input.empty() ? default_name : input;
    return true;
}

std::string normalize_user_model_name(std::string name) {
    const std::string prefix = "user.";
    if (name.rfind(prefix, 0) == 0) {
        return name;
    }
    return prefix + name;
}

void split_checkpoint_variant(const std::string& arg,
                              std::string& checkpoint, std::string& variant) {
    // HF repo ids never contain ':', so split on the last ':'.
    size_t pos = arg.rfind(':');
    if (pos == std::string::npos) {
        checkpoint = arg;
        variant.clear();
    } else {
        checkpoint = arg.substr(0, pos);
        variant = arg.substr(pos + 1);
    }
}

std::string format_variant_label(const json& v) {
    std::string name = v.value("name", "");
    size_t file_count = v.contains("files") && v["files"].is_array() ? v["files"].size() : 1;
    uint64_t size = v.value("size_bytes", static_cast<uint64_t>(0));
    std::ostringstream out;
    out << name << "  (" << file_count << (file_count == 1 ? " file, " : " files, ")
        << human_size(size) << ")";
    return out.str();
}

}  // namespace

int hf_pull_flow(lemonade::LemonadeClient& client,
                 const std::string& model_arg,
                 bool assume_yes) {
    std::string checkpoint;
    std::string variant;
    split_checkpoint_variant(model_arg, checkpoint, variant);

    if (checkpoint.find('/') == std::string::npos) {
        std::cerr << "Error: '" << model_arg
                  << "' does not look like a Hugging Face checkpoint (expected owner/repo)."
                  << std::endl;
        return 1;
    }

    // Fetch variants from the local server.
    std::string path = "/api/v1/pull/variants?checkpoint=" + url_encode(checkpoint);
    json variants_response;
    try {
        std::string body = client.make_request(path, "GET");
        variants_response = json::parse(body);
    } catch (const lemonade::HttpError& e) {
        if (e.status_code() == 404) {
            std::cerr << "Checkpoint '" << checkpoint << "' not found on Hugging Face." << std::endl;
            return 1;
        }
        std::cerr << "Error fetching variants: " << lemonade::extract_server_error_message(e) << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error fetching variants: " << e.what() << std::endl;
        return 1;
    }

    if (!variants_response.contains("variants") || !variants_response["variants"].is_array() ||
        variants_response["variants"].empty()) {
        std::cerr << "No GGUF variants found for '" << checkpoint << "'." << std::endl;
        return 1;
    }

    const auto& variants = variants_response["variants"];
    std::string recipe = variants_response.value("recipe", std::string("llamacpp"));

    // Non-llamacpp recipes (currently: ONNX RyzenAI) ship as a single
    // installable unit — no per-variant menu, no `:variant` checkpoint
    // suffix, no `-VARIANT` model name tail.
    if (recipe != "llamacpp") {
        if (!variant.empty()) {
            std::cerr << "warning: variant '" << variant << "' ignored for "
                      << recipe << " checkpoints" << std::endl;
        }

        // Backend preflight: ryzenai-llm is an optional, platform-gated
        // backend. Bail early with a friendly message instead of starting
        // a multi-GB download that will fail at load time.
        try {
            std::string sys_info_body = client.make_request("/api/v1/system-info", "GET");
            json sys_info = json::parse(sys_info_body);
            bool installed = false;
            std::vector<std::string> available_backends;
            if (sys_info.contains("recipes") && sys_info["recipes"].contains(recipe) &&
                sys_info["recipes"][recipe].contains("backends")) {
                for (auto& [name, b] : sys_info["recipes"][recipe]["backends"].items()) {
                    available_backends.push_back(name);
                    if (b.value("state", "") == "installed") { installed = true; break; }
                }
            }
            if (!installed) {
                std::cerr << "Error: this checkpoint requires the '" << recipe
                          << "' backend, which is not installed." << std::endl;
                if (available_backends.size() == 1) {
                    std::cerr << "       Install it with: lemonade backends install "
                              << recipe << ":" << available_backends[0] << std::endl;
                } else if (!available_backends.empty()) {
                    std::cerr << "       Install one of: ";
                    for (size_t i = 0; i < available_backends.size(); ++i) {
                        if (i) std::cerr << ", ";
                        std::cerr << recipe << ":" << available_backends[i];
                    }
                    std::cerr << std::endl;
                    std::cerr << "       e.g. lemonade backends install "
                              << recipe << ":" << available_backends[0] << std::endl;
                } else {
                    std::cerr << "       No backends are available for '" << recipe
                              << "' on this system." << std::endl;
                }
                return 1;
            }
        } catch (const std::exception& e) {
            std::cerr << "warning: could not verify backend availability: "
                      << e.what() << std::endl;
        }

        std::string suggested_name = variants_response.value("suggested_name", checkpoint);
        json pull_body;
        pull_body["model_name"] = "user." + suggested_name;
        pull_body["checkpoint"] = checkpoint;
        pull_body["recipe"] = recipe;

        std::cout << "Pulling " << checkpoint
                  << " as " << pull_body["model_name"].get<std::string>() << std::endl;
        return client.pull_model(pull_body);
    }

    // Resolve variant by case-insensitive name match.
    int selected_idx = -1;
    if (!variant.empty()) {
        std::string vlow = to_lower(variant);
        for (size_t i = 0; i < variants.size(); ++i) {
            if (to_lower(variants[i].value("name", "")) == vlow) {
                selected_idx = static_cast<int>(i);
                break;
            }
        }
    }

    std::string variant_name;
    bool prompted_for_variant = false;
    if (selected_idx >= 0) {
        variant_name = variants[selected_idx].value("name", "");
    } else if (!variant.empty()) {
        variant_name = variant;
    } else if (assume_yes) {
        variant_name = variants[0].value("name", "");
    } else {
        std::vector<std::string> labels;
        std::vector<std::string> names;
        labels.reserve(variants.size());
        names.reserve(variants.size());
        for (const auto& v : variants) {
            labels.push_back(format_variant_label(v));
            names.push_back(v.value("name", ""));
        }
        if (!prompt_variant(labels, names, variant_name)) return 1;
        prompted_for_variant = true;
    }

    // Build /v1/pull body.
    std::string suggested_name = variants_response.value("suggested_name", checkpoint);
    std::string default_model_name = suggested_name + "-" + variant_name;
    std::string model_name = default_model_name;
    if (prompted_for_variant) {
        if (!prompt_model_name(default_model_name, model_name)) return 1;
    }
    json pull_body;
    pull_body["model_name"] = normalize_user_model_name(model_name);
    pull_body["checkpoint"] = checkpoint + ":" + variant_name;
    pull_body["recipe"] = recipe;

    if (variants_response.contains("suggested_labels") &&
        variants_response["suggested_labels"].is_array() &&
        !variants_response["suggested_labels"].empty()) {
        pull_body["labels"] = variants_response["suggested_labels"];
    }

    if (variants_response.contains("mmproj_files") &&
        variants_response["mmproj_files"].is_array() &&
        !variants_response["mmproj_files"].empty()) {
        pull_body["mmproj"] = variants_response["mmproj_files"][0];
    }

    std::cout << "Pulling " << pull_body["checkpoint"].get<std::string>()
              << " as " << pull_body["model_name"].get<std::string>() << std::endl;

    return client.pull_model(pull_body);
}

}  // namespace lemon_cli
