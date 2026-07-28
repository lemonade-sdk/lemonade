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
constexpr size_t kShortVariantMenuLimit = 5;

bool prompt_variant_menu(const std::vector<std::string>& labels,
                         const std::vector<std::string>& names,
                         std::string& out,
                         bool show_all) {
    const size_t visible_count =
        show_all ? labels.size() : std::min(labels.size(), kShortVariantMenuLimit);

    std::cout << "Select a variant:" << std::endl;
    for (size_t i = 0; i < visible_count; ++i) {
        std::cout << "  " << (i + 1) << ") " << labels[i] << std::endl;
    }
    if (!show_all && labels.size() > visible_count) {
        std::cout << "  " << (visible_count + 1) << ") Browse all "
                  << labels.size() << " variants" << std::endl;
    }
    std::cout << "Enter number, or type any variant name: " << std::flush;

    std::string input;
    if (!std::getline(std::cin, input)) return false;
    trim(input);
    if (input.empty()) {
        std::cerr << "Error: variant is required." << std::endl;
        return false;
    }

    size_t parsed_chars = 0;
    try {
        int selected = std::stoi(input, &parsed_chars);
        if (parsed_chars == input.size()) {
            if (!show_all && labels.size() > visible_count &&
                selected == static_cast<int>(visible_count + 1)) {
                return prompt_variant_menu(labels, names, out, true);
            }
            if (selected < 1 || static_cast<size_t>(selected) > visible_count) {
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

bool prompt_variant(const std::vector<std::string>& labels,
                    const std::vector<std::string>& names,
                    std::string& out) {
    return prompt_variant_menu(labels, names, out, false);
}

bool prompt_model_name(const std::string& default_name, std::string& out) {
    std::cout << "Choose a model name." << std::endl;
    std::cout << "Press enter to use the default: " << default_name << std::endl;
    std::cout << "Or type a custom name and press enter." << std::endl;
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

std::string normalize_source(std::string source) {
    source = to_lower(std::move(source));
    if (source.empty() || source == "hf" || source == "huggingface") return "huggingface";
    if (source == "ms" || source == "modelscope") return "modelscope";
    return source;
}

std::string strip_query_fragment(std::string value) {
    const size_t pos = value.find_first_of("?#");
    if (pos != std::string::npos) value.resize(pos);
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

std::string first_repo_segments(const std::string& path) {
    const size_t slash = path.find('/');
    if (slash == std::string::npos) return path;
    const size_t second = path.find('/', slash + 1);
    return second == std::string::npos ? path : path.substr(0, second);
}

std::string normalize_registry_url(const std::string& arg,
                                   std::string& source) {
    static const std::vector<std::pair<std::string, std::string>> prefixes = {
        {"https://huggingface.co/", "huggingface"},
        {"http://huggingface.co/", "huggingface"},
        {"https://modelscope.cn/models/", "modelscope"},
        {"https://www.modelscope.cn/models/", "modelscope"},
        {"http://modelscope.cn/models/", "modelscope"},
        {"http://www.modelscope.cn/models/", "modelscope"},
        {"https://modelscope.ai/models/", "modelscope"},
        {"https://www.modelscope.ai/models/", "modelscope"},
    };
    for (const auto& [prefix, detected] : prefixes) {
        if (arg.rfind(prefix, 0) != 0) continue;
        source = detected;
        std::string path = strip_query_fragment(arg.substr(prefix.size()));
        return first_repo_segments(path);
    }
    return strip_query_fragment(arg);
}

void split_checkpoint_variant(const std::string& arg,
                              std::string& checkpoint, std::string& variant) {
    // Supported registry repo ids never contain ':', so split on the last ':'.
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

std::string normalize_registry_checkpoint_arg(const std::string& arg,
                                              const std::string& source_hint,
                                              std::string* detected_source) {
    std::string source = normalize_source(source_hint);
    std::string checkpoint = normalize_registry_url(arg, source);
    if (detected_source) *detected_source = source;
    return checkpoint;
}

std::string normalize_huggingface_checkpoint_arg(const std::string& arg) {
    return normalize_registry_checkpoint_arg(arg, "huggingface", nullptr);
}

int registry_pull_flow(lemonade::LemonadeClient& client,
                       const std::string& model_arg,
                       bool assume_yes,
                       const std::string& registry_source) {
    std::string source;
    std::string checkpoint;
    std::string variant;
    std::string normalized_model_arg = normalize_registry_checkpoint_arg(
        model_arg, registry_source, &source);
    split_checkpoint_variant(normalized_model_arg, checkpoint, variant);

    if (checkpoint.find('/') == std::string::npos) {
        std::cerr << "Error: '" << model_arg
                  << "' does not look like a model registry checkpoint (expected owner/repo)."
                  << std::endl;
        return 1;
    }

    // Fetch variants from the local server.
    std::string path = "/api/v1/pull/variants?checkpoint=" + url_encode(checkpoint) +
                       "&source=" + url_encode(source);
    json variants_response;
    try {
        std::string body = client.make_request(path, "GET");
        variants_response = json::parse(body);
    } catch (const lemonade::HttpError& e) {
        if (e.status_code() == 404) {
            std::cerr << "Checkpoint '" << checkpoint << "' not found on "
                      << (source == "modelscope" ? "ModelScope" : "Hugging Face")
                      << "." << std::endl;
            return 1;
        }
        std::cerr << "Error fetching variants: " << lemonade::extract_server_error_message(e) << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error fetching variants: " << e.what() << std::endl;
        return 1;
    }

    // Omni collection repos: /pull/variants only inspected the repo and told us
    // it is a collection. Send a *pointer* /pull body — model name + recipe +
    // the repo as the checkpoint. /pull does the actual downloading: it fetches
    // <RepoName>.json to disk and then pulls each component's weights. (We do not
    // forward the manifest content; /pull re-downloads it as the pull step, which
    // keeps inspection and download cleanly separated and mirrors regular models,
    // where the .gguf is downloaded by /pull rather than carried in the body.)
    if (variants_response.value("repo_kind", "") == "collection") {
        if (!variant.empty()) {
            std::cerr << "warning: variant '" << variant
                      << "' ignored for Omni collection repositories" << std::endl;
        }

        std::string model_name = normalize_user_model_name(
            variants_response.value("suggested_name", checkpoint));

        json pull_body;
        pull_body["model_name"] = model_name;
        pull_body["recipe"] = "collection.omni";
        pull_body["source"] = source;
        // The repo is the checkpoint pointer; /pull resolves components from the
        // manifest it downloads to disk, and a later `lemonade pull <name>`
        // refreshes them from the recorded remote registry.
        pull_body["checkpoints"] = json::object();
        pull_body["checkpoints"]["main"] = checkpoint;

        std::string display_name = model_name.substr(std::string("user.").size());
        size_t component_count = variants_response.value("component_count", static_cast<size_t>(0));
        std::cout << "Pulling Omni collection " << checkpoint << " as " << display_name
                  << " (" << component_count
                  << (component_count == 1 ? " component" : " components");
        if (variants_response.contains("size") && variants_response["size"].is_number()) {
            std::cout << ", ~" << variants_response["size"].get<double>() << " GB";
        }
        std::cout << ")" << std::endl;

        return client.pull_model(pull_body, display_name, /*upgrade=*/true);
    }

    if (!variants_response.contains("variants") || !variants_response["variants"].is_array() ||
        variants_response["variants"].empty()) {
        std::cerr << "No GGUF variants found for '" << checkpoint << "'." << std::endl;
        return 1;
    }

    const auto& variants = variants_response["variants"];
    std::string recipe = variants_response.value("recipe", std::string("llamacpp"));
    std::string repo_kind = variants_response.value("repo_kind", std::string("gguf"));

    // Non-GGUF repos (currently: ONNX RyzenAI) ship as a single installable
    // unit — no per-variant menu, no `:variant` checkpoint suffix, no
    // `-VARIANT` model name tail. (Collections returned earlier above.)
    if (repo_kind != "gguf") {
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
        pull_body["source"] = source;

        std::cout << "Pulling " << checkpoint
                  << " as " << suggested_name << std::endl;
        return client.pull_model(pull_body, suggested_name, /*upgrade=*/true);
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
    pull_body["source"] = source;

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
              << " as " << model_name << std::endl;

    return client.pull_model(pull_body, model_name, /*upgrade=*/true);
}


int hf_pull_flow(lemonade::LemonadeClient& client,
                 const std::string& model_arg,
                 bool assume_yes) {
    return registry_pull_flow(client, model_arg, assume_yes, "huggingface");
}

}  // namespace lemon_cli
