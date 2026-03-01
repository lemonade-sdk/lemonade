#include "lemon/omni_loop.h"
#include "lemon/router.h"
#include "lemon/model_manager.h"
#include "lemon/error_types.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/json_utils.h"
#include "lemon/recipe_options.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <set>
#include <algorithm>
#include <cstdio>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

namespace lemon {

// === OmniConfig ===

OmniConfig OmniConfig::from_json(const json& j, const std::string& model) {
    OmniConfig config;
    config.brain_model = model;

    if (j.is_null()) return config;

    if (j.contains("max_iterations")) config.max_iterations = j["max_iterations"].get<int>();
    if (j.contains("image_model")) config.image_model = j["image_model"].get<std::string>();
    if (j.contains("audio_model")) config.audio_model = j["audio_model"].get<std::string>();
    if (j.contains("tts_model")) config.tts_model = j["tts_model"].get<std::string>();
    if (j.contains("vision_model")) config.vision_model = j["vision_model"].get<std::string>();

    if (j.contains("tools") && j["tools"].is_array()) {
        config.tools.clear();
        for (const auto& t : j["tools"]) {
            config.tools.push_back(t.get<std::string>());
        }
    } else {
        // Default: all tools enabled
        config.tools = {"generate_image", "describe_image", "analyze_image", "transcribe_audio", "text_to_speech", "edit_image",
                        "read_file", "write_file", "list_directory", "web_search",
                        "list_models", "load_model", "run_command"};
    }

    // Extensibility fields
    if (j.contains("system_prompt") && j["system_prompt"].is_string()) {
        config.system_prompt = j["system_prompt"].get<std::string>();
    }
    if (j.contains("extra_tools") && j["extra_tools"].is_array()) {
        config.extra_tools = j["extra_tools"];
    }
    if (j.contains("tool_callback_url") && j["tool_callback_url"].is_string()) {
        config.tool_callback_url = j["tool_callback_url"].get<std::string>();
    }
    if (j.contains("tool_callback_timeout") && j["tool_callback_timeout"].is_number_integer()) {
        config.tool_callback_timeout = j["tool_callback_timeout"].get<int>();
    }

    return config;
}

// === OmniLoop ===

OmniLoop::OmniLoop(Router* router, ModelManager* model_manager)
    : router_(router), model_manager_(model_manager) {}

json OmniLoop::build_conversation(const json& request, const OmniConfig& config) {
    json conversation = json::array();

    // Use custom system prompt if provided, otherwise use the default
    std::string system_content;
    if (!config.system_prompt.empty()) {
        system_content = config.system_prompt;
    } else {
        system_content =
         "You are Lemonade Omni, a multimodal AI assistant running locally on the user's computer. "
         "You have full access to their filesystem, shell, and the internet through your tools. "
         "NEVER refuse a request by saying you don't have access — you DO. Always use tools to act.\n\n"

         "## Your Tools\n\n"

         "**Shell & Filesystem:**\n"
         "- run_command: Run a command on the user's computer. "
         "On Windows commands run in PowerShell — write native PowerShell directly (do NOT wrap in `powershell -command`). "
         "On Linux/Mac commands run in bash. "
         "This is your most versatile tool — use it when no specialized tool fits better.\n"
         "- read_file: Read a file's contents. Use for reading known file paths.\n"
         "- write_file: Create or overwrite a file. Creates parent directories automatically.\n"
         "- list_directory: List files in a directory. Use for quick directory listings.\n\n"

         "**Web:**\n"
         "- web_search: Search the web via DuckDuckGo. Returns titles, URLs, and snippets.\n\n"

         "**Image:**\n"
         "- generate_image: Generate an image from a text prompt using Stable Diffusion.\n"
         "- edit_image: Edit an existing image based on a text prompt.\n"
         "- describe_image: Describe or answer questions about an image (uses the brain model).\n"
         "- analyze_image: Analyze any image using a dedicated vision model. Accepts a file path or base64. "
         "Use for screenshots, documents, charts, photos, UI mockups, or any visual content.\n\n"

         "**Audio:**\n"
         "- transcribe_audio: Transcribe audio to text using Whisper.\n"
         "- text_to_speech: Convert text to spoken audio.\n\n"

         "**Model Management:**\n"
         "- list_models: See all available AI models, their types, sizes, and status.\n"
         "- load_model: Download and load a specific model for inference.\n\n"

         "## Tool Selection Rules\n\n"
         "1. When the user mentions a path, file, or folder — use your tools immediately. "
         "Do not say you cannot access their computer.\n"
         "2. If you don't know a path (e.g. the user's home dir, Downloads, Desktop), "
         "use run_command to discover it (e.g. `$env:USERPROFILE` on Windows, `echo $HOME` on Linux/Mac).\n"
         "3. Prefer specialized tools over run_command when possible: "
         "use read_file over `type`/`cat`, write_file over `echo >`, list_directory over `dir`/`ls`.\n"
         "4. Use run_command for anything the specialized tools can't do: "
         "piping, chaining commands, running scripts, environment variables, system info, etc.\n"
         "5. Call multiple tools in one turn when they are independent of each other.\n"
         "6. When generating images or audio, always include the result in your response — "
         "don't just describe what you did.\n\n"

         "## Response Style\n\n"
         "Be concise. Show results, not process. "
         "If a tool returns data, summarize the key points rather than dumping raw output. "
         "If something fails, explain what went wrong and try an alternative approach.";
    }

    conversation.push_back({
        {"role", "system"},
        {"content", system_content}
    });

    for (const auto& msg : request.value("messages", json::array())) {
        conversation.push_back(msg);
    }
    return conversation;
}

json OmniLoop::build_tool_definitions(const OmniConfig& config) {
    const auto& enabled_tools = config.tools;
    static const std::map<std::string, json> all_tools = {
        {"generate_image", {
            {"type", "function"},
            {"function", {
                {"name", "generate_image"},
                {"description", "Generate an image from a text prompt using Stable Diffusion"},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"prompt", {{"type", "string"}, {"description", "Text description of the image to generate"}}},
                        {"size", {{"type", "string"}, {"description", "Image size (e.g. '512x512')"}, {"default", "512x512"}}},
                        {"n", {{"type", "integer"}, {"description", "Number of images to generate"}, {"default", 1}}}
                    }},
                    {"required", {"prompt"}}
                }}
            }}
        }},
        {"edit_image", {
            {"type", "function"},
            {"function", {
                {"name", "edit_image"},
                {"description", "Edit an existing image based on a text prompt"},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"prompt", {{"type", "string"}, {"description", "Description of the edit to make"}}},
                        {"image", {{"type", "string"}, {"description", "Base64-encoded image to edit"}}},
                        {"mask", {{"type", "string"}, {"description", "Base64-encoded mask (white areas will be edited)"}}}
                    }},
                    {"required", {"prompt", "image"}}
                }}
            }}
        }},
        {"describe_image", {
            {"type", "function"},
            {"function", {
                {"name", "describe_image"},
                {"description", "Describe or answer questions about an image using a vision model"},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"image", {{"type", "string"}, {"description", "Base64-encoded image to describe"}}},
                        {"question", {{"type", "string"}, {"description", "Question to ask about the image"}, {"default", "Describe this image in detail."}}}
                    }},
                    {"required", {"image"}}
                }}
            }}
        }},
        {"analyze_image", {
            {"type", "function"},
            {"function", {
                {"name", "analyze_image"},
                {"description", "Analyze an image using a dedicated vision language model. Can read and reason about screenshots, photos, documents, diagrams, charts, UI mockups, or any visual content. Accepts a file path or base64-encoded image."},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"image", {{"type", "string"}, {"description", "File path to an image (e.g. 'C:/Users/me/screenshot.png') or base64-encoded image data"}}},
                        {"prompt", {{"type", "string"}, {"description", "What to analyze or question to ask about the image"}}}
                    }},
                    {"required", {"image", "prompt"}}
                }}
            }}
        }},
        {"transcribe_audio", {
            {"type", "function"},
            {"function", {
                {"name", "transcribe_audio"},
                {"description", "Transcribe audio to text using Whisper"},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"audio", {{"type", "string"}, {"description", "Base64-encoded audio data"}}},
                        {"language", {{"type", "string"}, {"description", "Language code (e.g. 'en')"}, {"default", "en"}}}
                    }},
                    {"required", {"audio"}}
                }}
            }}
        }},
        {"text_to_speech", {
            {"type", "function"},
            {"function", {
                {"name", "text_to_speech"},
                {"description", "Convert text to speech audio"},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"input", {{"type", "string"}, {"description", "Text to convert to speech"}}},
                        {"voice", {{"type", "string"}, {"description", "Voice to use"}, {"default", "af_heart"}}}
                    }},
                    {"required", {"input"}}
                }}
            }}
        }},
        {"read_file", {
            {"type", "function"},
            {"function", {
                {"name", "read_file"},
                {"description", "Read the contents of a file at the given path"},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"path", {{"type", "string"}, {"description", "Absolute or relative path to the file to read"}}}
                    }},
                    {"required", {"path"}}
                }}
            }}
        }},
        {"write_file", {
            {"type", "function"},
            {"function", {
                {"name", "write_file"},
                {"description", "Write content to a file, creating parent directories if needed"},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"path", {{"type", "string"}, {"description", "Absolute or relative path to the file to write"}}},
                        {"content", {{"type", "string"}, {"description", "Content to write to the file"}}}
                    }},
                    {"required", {"path", "content"}}
                }}
            }}
        }},
        {"list_directory", {
            {"type", "function"},
            {"function", {
                {"name", "list_directory"},
                {"description", "List files and subdirectories in a directory"},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"path", {{"type", "string"}, {"description", "Absolute or relative path to the directory to list"}}}
                    }},
                    {"required", {"path"}}
                }}
            }}
        }},
        {"web_search", {
            {"type", "function"},
            {"function", {
                {"name", "web_search"},
                {"description", "Search the web using DuckDuckGo and return result titles, URLs, and snippets"},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"query", {{"type", "string"}, {"description", "Search query"}}},
                        {"num_results", {{"type", "integer"}, {"description", "Maximum number of results to return"}, {"default", 5}}}
                    }},
                    {"required", {"query"}}
                }}
            }}
        }},
        {"list_models", {
            {"type", "function"},
            {"function", {
                {"name", "list_models"},
                {"description", "List available AI models on this server. Returns model names, types (LLM, vision, image, audio, TTS, embedding), sizes, and whether they are downloaded or currently loaded. Use this to discover what models are available before loading one."},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"type_filter", {{"type", "string"}, {"description", "Filter by model type: 'llm', 'vision', 'image', 'audio', 'tts', 'embedding', 'reranking', or 'all'"}, {"default", "all"}}},
                        {"downloaded_only", {{"type", "boolean"}, {"description", "If true, only show models that are already downloaded"}, {"default", false}}}
                    }}
                }}
            }}
        }},
        {"load_model", {
            {"type", "function"},
            {"function", {
                {"name", "load_model"},
                {"description", "Download (if needed) and load an AI model so it is ready for inference. Use list_models first to see available models. Loading a model may take time if it needs to be downloaded."},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"model", {{"type", "string"}, {"description", "Name of the model to load (exact name from list_models)"}}}
                    }},
                    {"required", {"model"}}
                }}
            }}
        }},
        {"run_command", {
            {"type", "function"},
            {"function", {
                {"name", "run_command"},
                {"description", "Run a command on the user's computer and return the output. "
                 "On Windows this runs in PowerShell, on Linux/Mac in bash. "
                 "Use this to explore the filesystem, check environment variables, run scripts, or perform any system task."},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"command", {{"type", "string"}, {"description", "The shell command to execute"}}}
                    }},
                    {"required", {"command"}}
                }}
            }}
        }}
    };

    json tools_array = json::array();
    for (const auto& tool_name : enabled_tools) {
        auto it = all_tools.find(tool_name);
        if (it != all_tools.end()) {
            tools_array.push_back(it->second);
        }
    }

    // Append extra tool definitions from the request
    if (config.extra_tools.is_array()) {
        for (const auto& tool : config.extra_tools) {
            tools_array.push_back(tool);
        }
    }

    return tools_array;
}

void OmniLoop::ensure_model_loaded(const std::string& model_name) {
    if (router_->is_model_loaded(model_name)) return;

    if (!model_manager_->model_exists(model_name)) {
        throw InvalidRequestException("Model not found: " + model_name);
    }

    auto model_info = model_manager_->get_model_info(model_name);

    // Download if not yet cached
    if (model_info.recipe != "flm" && !model_manager_->is_model_downloaded(model_name)) {
        std::cout << "[OmniLoop] Downloading model: " << model_name << std::endl;
        model_manager_->download_registered_model(model_info, true);
        model_info = model_manager_->get_model_info(model_name);
    }

    RecipeOptions options(model_info.recipe, json::object());
    router_->load_model(model_name, model_info, options, true);
    std::cout << "[OmniLoop] Model loaded: " << model_name << std::endl;
}

// === Tool Executors ===

ToolResult OmniLoop::execute_generate_image(const json& args, const std::string& tool_call_id, const OmniConfig& config) {
    ToolResult result;
    result.tool_call_id = tool_call_id;
    result.tool_name = "generate_image";

    try {
        ensure_model_loaded(config.image_model);

        json img_request = {
            {"model", config.image_model},
            {"prompt", args.value("prompt", "")},
            {"size", args.value("size", "512x512")},
            {"n", args.value("n", 1)},
            {"response_format", "b64_json"}
        };

        json response = router_->image_generations(img_request);

        result.result_data = response;
        result.llm_summary = "Generated " + std::to_string(args.value("n", 1)) +
                            " image(s) for prompt: '" + args.value("prompt", "") + "'. "
                            "The image data is in this tool result as base64. "
                            "To save it, use write_file with the base64-decoded content, "
                            "or use run_command to decode and save it.";
        result.success = !response.contains("error");

        if (response.contains("error")) {
            result.llm_summary = "Image generation failed: " + response["error"]["message"].get<std::string>();
        }
    } catch (const std::exception& e) {
        result.success = false;
        result.llm_summary = std::string("Image generation failed: ") + e.what();
        result.result_data = {{"error", e.what()}};
    }

    return result;
}

ToolResult OmniLoop::execute_edit_image(const json& args, const std::string& tool_call_id, const OmniConfig& config) {
    ToolResult result;
    result.tool_call_id = tool_call_id;
    result.tool_name = "edit_image";

    try {
        ensure_model_loaded(config.image_model);

        json edit_request = {
            {"model", config.image_model},
            {"prompt", args.value("prompt", "")},
            {"image", args.value("image", "")},
            {"response_format", "b64_json"}
        };

        if (args.contains("mask")) {
            edit_request["mask"] = args["mask"];
        }

        json response = router_->image_edits(edit_request);

        result.result_data = response;
        result.llm_summary = "Edited image based on prompt: '" + args.value("prompt", "") + "'. "
                            "The edited image data is in this tool result as base64.";
        result.success = !response.contains("error");

        if (response.contains("error")) {
            result.llm_summary = "Image editing failed: " + response["error"]["message"].get<std::string>();
        }
    } catch (const std::exception& e) {
        result.success = false;
        result.llm_summary = std::string("Image editing failed: ") + e.what();
        result.result_data = {{"error", e.what()}};
    }

    return result;
}

ToolResult OmniLoop::execute_describe_image(const json& args, const std::string& tool_call_id, const OmniConfig& config) {
    ToolResult result;
    result.tool_call_id = tool_call_id;
    result.tool_name = "describe_image";

    try {
        std::string question = args.value("question", "Describe this image in detail.");
        std::string image_b64 = args.value("image", "");

        json vision_request = {
            {"model", config.brain_model},
            {"messages", json::array({
                {{"role", "user"}, {"content", json::array({
                    {{"type", "image_url"}, {"image_url", {{"url", "data:image/png;base64," + image_b64}}}},
                    {{"type", "text"}, {"text", question}}
                })}}
            })}
        };

        json response = router_->chat_completion(vision_request);

        std::string description;
        if (response.contains("choices") && !response["choices"].empty()) {
            auto& choice = response["choices"][0];
            if (choice.contains("message") && choice["message"].contains("content")) {
                auto& content = choice["message"]["content"];
                if (content.is_string()) {
                    description = content.get<std::string>();
                }
            }
        }

        result.result_data = response;
        result.llm_summary = description;
        result.success = !response.contains("error");

        if (response.contains("error")) {
            result.llm_summary = "Image description failed: " + response["error"]["message"].get<std::string>();
        }
    } catch (const std::exception& e) {
        result.success = false;
        result.llm_summary = std::string("Image description failed: ") + e.what();
        result.result_data = {{"error", e.what()}};
    }

    return result;
}

ToolResult OmniLoop::execute_analyze_image(const json& args, const std::string& tool_call_id, const OmniConfig& config) {
    ToolResult result;
    result.tool_call_id = tool_call_id;
    result.tool_name = "analyze_image";

    try {
        std::string prompt = args.value("prompt", "Describe this image in detail.");
        std::string image_input = args.value("image", "");

        if (image_input.empty()) {
            throw std::runtime_error("Missing required parameter: image");
        }

        // Detect whether input is a file path or base64 data
        std::string image_b64;
        std::string mime_type = "image/png";

        // Heuristic: if it contains path separators or ends with an image extension, treat as file path
        bool is_file_path = false;
        if (image_input.find('/') != std::string::npos || image_input.find('\\') != std::string::npos) {
            is_file_path = true;
        } else {
            // Check for common image extensions
            std::string lower = image_input;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            for (const auto& ext : {".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp"}) {
                if (lower.size() > strlen(ext) && lower.substr(lower.size() - strlen(ext)) == ext) {
                    is_file_path = true;
                    break;
                }
            }
        }

        if (is_file_path) {
            std::filesystem::path fs_path(image_input);
            if (!std::filesystem::exists(fs_path)) {
                throw std::runtime_error("Image file not found: " + image_input);
            }
            if (!std::filesystem::is_regular_file(fs_path)) {
                throw std::runtime_error("Not a regular file: " + image_input);
            }

            auto file_size = std::filesystem::file_size(fs_path);
            const size_t max_size = 20 * 1024 * 1024; // 20MB cap for images
            if (file_size > max_size) {
                throw std::runtime_error("Image file too large (" + std::to_string(file_size) +
                                         " bytes, max " + std::to_string(max_size) + ")");
            }

            // Read file contents
            std::ifstream ifs(fs_path, std::ios::binary);
            if (!ifs.is_open()) {
                throw std::runtime_error("Cannot open image file: " + image_input);
            }
            std::string file_content((std::istreambuf_iterator<char>(ifs)),
                                      std::istreambuf_iterator<char>());

            // Base64-encode
            image_b64 = utils::JsonUtils::base64_encode(file_content);

            // Detect mime type from extension
            std::string ext = fs_path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".jpg" || ext == ".jpeg") mime_type = "image/jpeg";
            else if (ext == ".gif") mime_type = "image/gif";
            else if (ext == ".bmp") mime_type = "image/bmp";
            else if (ext == ".webp") mime_type = "image/webp";
            else mime_type = "image/png";

            std::cout << "[OmniLoop] analyze_image: read file " << image_input
                      << " (" << file_size << " bytes, " << mime_type << ")" << std::endl;
        } else {
            image_b64 = image_input;
        }

        // Load the dedicated vision model
        ensure_model_loaded(config.vision_model);

        // Build multimodal chat completion request
        json vision_request = {
            {"model", config.vision_model},
            {"messages", json::array({
                {{"role", "user"}, {"content", json::array({
                    {{"type", "image_url"}, {"image_url", {{"url", "data:" + mime_type + ";base64," + image_b64}}}},
                    {{"type", "text"}, {"text", prompt}}
                })}}
            })}
        };

        json response = router_->chat_completion(vision_request);

        std::string analysis;
        if (response.contains("choices") && !response["choices"].empty()) {
            auto& choice = response["choices"][0];
            if (choice.contains("message") && choice["message"].contains("content")) {
                auto& content = choice["message"]["content"];
                if (content.is_string()) {
                    analysis = content.get<std::string>();
                }
            }
        }

        result.result_data = response;
        result.llm_summary = analysis;
        result.success = !response.contains("error");

        if (response.contains("error")) {
            result.llm_summary = "Image analysis failed: " + response["error"]["message"].get<std::string>();
        }
    } catch (const std::exception& e) {
        result.success = false;
        result.llm_summary = std::string("Image analysis failed: ") + e.what();
        result.result_data = {{"error", e.what()}};
    }

    return result;
}

ToolResult OmniLoop::execute_transcribe_audio(const json& args, const std::string& tool_call_id, const OmniConfig& config) {
    ToolResult result;
    result.tool_call_id = tool_call_id;
    result.tool_name = "transcribe_audio";

    try {
        ensure_model_loaded(config.audio_model);

        json transcribe_request = {
            {"model", config.audio_model},
            {"file", args.value("audio", "")},
            {"language", args.value("language", "en")}
        };

        json response = router_->audio_transcriptions(transcribe_request);

        std::string text;
        if (response.contains("text")) {
            text = response["text"].get<std::string>();
        }

        result.result_data = response;
        result.llm_summary = "Transcription: '" + text + "'";
        result.success = !response.contains("error");

        if (response.contains("error")) {
            result.llm_summary = "Transcription failed: " + response["error"]["message"].get<std::string>();
        }
    } catch (const std::exception& e) {
        result.success = false;
        result.llm_summary = std::string("Transcription failed: ") + e.what();
        result.result_data = {{"error", e.what()}};
    }

    return result;
}

ToolResult OmniLoop::execute_text_to_speech(const json& args, const std::string& tool_call_id, const OmniConfig& config) {
    ToolResult result;
    result.tool_call_id = tool_call_id;
    result.tool_name = "text_to_speech";

    try {
        ensure_model_loaded(config.tts_model);

        // Get the backend address for the TTS model
        std::string backend_addr = router_->get_backend_address(config.tts_model);
        if (backend_addr.empty()) {
            throw std::runtime_error("TTS model not loaded: " + config.tts_model);
        }

        json tts_request = {
            {"model", config.tts_model},
            {"input", args.value("input", "")},
            {"voice", args.value("voice", "af_heart")},
            {"response_format", "mp3"}
        };

        // Call the TTS backend directly via HTTP
        auto http_response = utils::HttpClient::post(
            backend_addr + "/audio/speech",
            tts_request.dump(),
            {{"Content-Type", "application/json"}},
            300
        );

        if (http_response.status_code == 200) {
            std::string audio_b64 = utils::JsonUtils::base64_encode(http_response.body);
            result.result_data = {
                {"audio_base64", audio_b64},
                {"format", "mp3"},
                {"model", config.tts_model}
            };
            std::string input_text = args.value("input", "");
            std::string preview = input_text.substr(0, 100);
            result.llm_summary = "Generated speech for: '" + preview + "'";
            result.success = true;
        } else {
            result.success = false;
            result.llm_summary = "TTS request failed with status " + std::to_string(http_response.status_code);
            result.result_data = {{"error", http_response.body}};
        }
    } catch (const std::exception& e) {
        result.success = false;
        result.llm_summary = std::string("Text-to-speech failed: ") + e.what();
        result.result_data = {{"error", e.what()}};
    }

    return result;
}

// === Filesystem & Web Search Tool Executors ===

ToolResult OmniLoop::execute_read_file(const json& args, const std::string& tool_call_id) {
    ToolResult result;
    result.tool_call_id = tool_call_id;
    result.tool_name = "read_file";

    try {
        std::string path = args.value("path", "");
        if (path.empty()) {
            throw std::runtime_error("Missing required parameter: path");
        }

        std::filesystem::path fs_path(path);
        if (!std::filesystem::exists(fs_path)) {
            throw std::runtime_error("File not found: " + path);
        }
        if (!std::filesystem::is_regular_file(fs_path)) {
            throw std::runtime_error("Not a regular file: " + path);
        }

        auto file_size = std::filesystem::file_size(fs_path);
        const size_t max_size = 100 * 1024; // 100KB cap
        if (file_size > max_size) {
            throw std::runtime_error("File too large (" + std::to_string(file_size) +
                                     " bytes, max " + std::to_string(max_size) + "): " + path);
        }

        std::ifstream ifs(fs_path, std::ios::binary);
        if (!ifs.is_open()) {
            throw std::runtime_error("Permission denied or cannot open file: " + path);
        }

        std::string content((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());

        // Detect binary files (null bytes)
        if (content.find('\0') != std::string::npos) {
            throw std::runtime_error("Binary file detected, cannot display: " + path);
        }

        result.result_data = {{"content", content}, {"path", path}, {"size", file_size}};

        // Truncated preview for LLM context
        std::string preview = content.substr(0, 2000);
        if (content.size() > 2000) {
            preview += "\n... (truncated, " + std::to_string(content.size()) + " bytes total)";
        }
        result.llm_summary = "File: " + path + " (" + std::to_string(file_size) + " bytes)\n" + preview;
        result.success = true;
    } catch (const std::exception& e) {
        result.success = false;
        result.llm_summary = std::string("read_file failed: ") + e.what();
        result.result_data = {{"error", e.what()}};
    }

    return result;
}

ToolResult OmniLoop::execute_write_file(const json& args, const std::string& tool_call_id) {
    ToolResult result;
    result.tool_call_id = tool_call_id;
    result.tool_name = "write_file";

    try {
        std::string path = args.value("path", "");
        std::string content = args.value("content", "");
        if (path.empty()) {
            throw std::runtime_error("Missing required parameter: path");
        }

        std::filesystem::path fs_path(path);

        // Create parent directories if needed
        auto parent = fs_path.parent_path();
        if (!parent.empty() && !std::filesystem::exists(parent)) {
            std::filesystem::create_directories(parent);
        }

        std::ofstream ofs(fs_path, std::ios::binary);
        if (!ofs.is_open()) {
            throw std::runtime_error("Permission denied or cannot open file for writing: " + path);
        }

        ofs.write(content.data(), content.size());
        ofs.close();

        if (ofs.fail()) {
            throw std::runtime_error("Failed to write to file: " + path);
        }

        result.result_data = {{"path", path}, {"bytes_written", content.size()}};
        result.llm_summary = "Wrote " + std::to_string(content.size()) + " bytes to " + path;
        result.success = true;
    } catch (const std::exception& e) {
        result.success = false;
        result.llm_summary = std::string("write_file failed: ") + e.what();
        result.result_data = {{"error", e.what()}};
    }

    return result;
}

ToolResult OmniLoop::execute_list_directory(const json& args, const std::string& tool_call_id) {
    ToolResult result;
    result.tool_call_id = tool_call_id;
    result.tool_name = "list_directory";

    try {
        std::string path = args.value("path", ".");
        if (path.empty()) path = ".";

        std::filesystem::path fs_path(path);
        if (!std::filesystem::exists(fs_path)) {
            throw std::runtime_error("Directory not found: " + path);
        }
        if (!std::filesystem::is_directory(fs_path)) {
            throw std::runtime_error("Not a directory: " + path);
        }

        json entries = json::array();
        std::string listing;
        int count = 0;
        const int max_entries = 200;

        for (const auto& entry : std::filesystem::directory_iterator(fs_path)) {
            if (count >= max_entries) {
                listing += "... (truncated at " + std::to_string(max_entries) + " entries)\n";
                break;
            }

            std::string name = entry.path().filename().string();
            std::string type = entry.is_directory() ? "dir" : "file";
            size_t size = 0;
            if (entry.is_regular_file()) {
                try { size = entry.file_size(); } catch (...) {}
            }

            entries.push_back({{"name", name}, {"type", type}, {"size", size}});
            listing += (entry.is_directory() ? "[DIR]  " : "[FILE] ") + name;
            if (entry.is_regular_file()) {
                listing += " (" + std::to_string(size) + " bytes)";
            }
            listing += "\n";
            count++;
        }

        result.result_data = {{"path", path}, {"entries", entries}, {"count", count}};
        result.llm_summary = "Directory: " + path + " (" + std::to_string(count) + " entries)\n" + listing;
        result.success = true;
    } catch (const std::exception& e) {
        result.success = false;
        result.llm_summary = std::string("list_directory failed: ") + e.what();
        result.result_data = {{"error", e.what()}};
    }

    return result;
}

ToolResult OmniLoop::execute_web_search(const json& args, const std::string& tool_call_id) {
    ToolResult result;
    result.tool_call_id = tool_call_id;
    result.tool_name = "web_search";

    try {
        std::string query = args.value("query", "");
        int num_results = args.value("num_results", 5);
        if (query.empty()) {
            throw std::runtime_error("Missing required parameter: query");
        }

        // URL-encode the query
        std::string encoded_query;
        for (char c : query) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
                encoded_query += c;
            } else if (c == ' ') {
                encoded_query += '+';
            } else {
                std::ostringstream oss;
                oss << '%' << std::uppercase << std::hex
                    << std::setw(2) << std::setfill('0')
                    << (static_cast<unsigned int>(static_cast<unsigned char>(c)));
                encoded_query += oss.str();
            }
        }

        std::string body = "q=" + encoded_query;
        auto http_response = utils::HttpClient::post(
            "https://html.duckduckgo.com/html/",
            body,
            {{"Content-Type", "application/x-www-form-urlencoded"},
             {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"}},
            30
        );

        if (http_response.status_code != 200) {
            throw std::runtime_error("DuckDuckGo returned HTTP " + std::to_string(http_response.status_code));
        }

        // Parse DuckDuckGo HTML results
        const std::string& html = http_response.body;
        json results = json::array();
        std::string summary;
        int found = 0;

        // Find each result link: <a class="result__a" href="...">title</a>
        std::string link_marker = "class=\"result__a\"";
        std::string snippet_marker = "class=\"result__snippet\"";
        size_t pos = 0;

        while (found < num_results && pos < html.size()) {
            // Find the next result link
            size_t link_pos = html.find(link_marker, pos);
            if (link_pos == std::string::npos) break;

            // Extract href
            std::string url;
            size_t href_pos = html.rfind("href=\"", link_pos);
            if (href_pos != std::string::npos && (link_pos - href_pos) < 200) {
                size_t href_start = href_pos + 6;
                size_t href_end = html.find('"', href_start);
                if (href_end != std::string::npos) {
                    url = html.substr(href_start, href_end - href_start);
                }
            }

            // Extract title (text between > and </a>)
            std::string title;
            size_t tag_end = html.find('>', link_pos);
            if (tag_end != std::string::npos) {
                size_t title_start = tag_end + 1;
                size_t title_end = html.find("</a>", title_start);
                if (title_end != std::string::npos) {
                    title = html.substr(title_start, title_end - title_start);
                    // Strip HTML tags from title
                    std::string clean_title;
                    bool in_tag = false;
                    for (char c : title) {
                        if (c == '<') in_tag = true;
                        else if (c == '>') in_tag = false;
                        else if (!in_tag) clean_title += c;
                    }
                    title = clean_title;
                }
            }

            // Extract snippet
            std::string snippet;
            size_t snippet_pos = html.find(snippet_marker, link_pos);
            if (snippet_pos != std::string::npos && (snippet_pos - link_pos) < 2000) {
                size_t snippet_tag_end = html.find('>', snippet_pos);
                if (snippet_tag_end != std::string::npos) {
                    size_t snippet_start = snippet_tag_end + 1;
                    // Find end of snippet span/div
                    size_t snippet_end = html.find("</", snippet_start);
                    if (snippet_end != std::string::npos) {
                        snippet = html.substr(snippet_start, snippet_end - snippet_start);
                        // Strip HTML tags from snippet
                        std::string clean_snippet;
                        bool in_tag = false;
                        for (char c : snippet) {
                            if (c == '<') in_tag = true;
                            else if (c == '>') in_tag = false;
                            else if (!in_tag) clean_snippet += c;
                        }
                        snippet = clean_snippet;
                    }
                }
            }

            if (!title.empty() || !url.empty()) {
                results.push_back({{"title", title}, {"url", url}, {"snippet", snippet}});
                found++;
                summary += std::to_string(found) + ". " + title + " - " + url + "\n";
                if (!snippet.empty()) {
                    summary += "   " + snippet.substr(0, 200) + "\n";
                }
            }

            pos = link_pos + link_marker.size();
        }

        result.result_data = {{"query", query}, {"results", results}, {"count", found}};
        if (found == 0) {
            result.llm_summary = "No results found for: " + query;
        } else {
            result.llm_summary = "Search results for '" + query + "' (" + std::to_string(found) + " results):\n" + summary;
        }
        result.success = true;
    } catch (const std::exception& e) {
        result.success = false;
        result.llm_summary = std::string("web_search failed: ") + e.what();
        result.result_data = {{"error", e.what()}};
    }

    return result;
}

// === Model Management Tool Executors ===

ToolResult OmniLoop::execute_list_models(const json& args, const std::string& tool_call_id) {
    ToolResult result;
    result.tool_call_id = tool_call_id;
    result.tool_name = "list_models";

    try {
        std::string type_filter = args.value("type_filter", "all");
        bool downloaded_only = args.value("downloaded_only", false);

        auto all_models = model_manager_->get_supported_models();
        json loaded_models = router_->get_all_loaded_models();

        // Build a set of currently loaded model names
        std::set<std::string> loaded_set;
        if (loaded_models.is_array()) {
            for (const auto& lm : loaded_models) {
                if (lm.contains("model")) {
                    loaded_set.insert(lm["model"].get<std::string>());
                }
            }
        }

        json models_array = json::array();
        std::string summary;
        int count = 0;

        for (const auto& [name, info] : all_models) {
            // Apply downloaded filter
            if (downloaded_only && !info.downloaded) continue;

            // Determine display type from labels
            std::string model_type = "llm";
            for (const auto& label : info.labels) {
                if (label == "vision") { model_type = "vision"; break; }
                if (label == "audio") { model_type = "audio"; break; }
                if (label == "image") { model_type = "image"; break; }
                if (label == "tts") { model_type = "tts"; break; }
                if (label == "embeddings" || label == "embedding") { model_type = "embedding"; break; }
                if (label == "reranking") { model_type = "reranking"; break; }
            }

            // Apply type filter
            if (type_filter != "all" && model_type != type_filter) continue;

            bool is_loaded = loaded_set.count(name) > 0;

            json model_entry = {
                {"name", name},
                {"type", model_type},
                {"recipe", info.recipe},
                {"size_gb", info.size},
                {"downloaded", info.downloaded},
                {"loaded", is_loaded},
                {"labels", info.labels}
            };
            models_array.push_back(model_entry);

            // Build summary line
            std::string status;
            if (is_loaded) status = "LOADED";
            else if (info.downloaded) status = "downloaded";
            else status = "available";

            summary += name + " [" + model_type + "] " +
                       std::to_string(info.size).substr(0, std::to_string(info.size).find('.') + 2) + "GB " +
                       "(" + status + ")\n";
            count++;
        }

        result.result_data = {{"models", models_array}, {"count", count}};
        result.llm_summary = "Available models (" + std::to_string(count) + "):\n" + summary;
        result.success = true;
    } catch (const std::exception& e) {
        result.success = false;
        result.llm_summary = std::string("list_models failed: ") + e.what();
        result.result_data = {{"error", e.what()}};
    }

    return result;
}

ToolResult OmniLoop::execute_load_model(const json& args, const std::string& tool_call_id) {
    ToolResult result;
    result.tool_call_id = tool_call_id;
    result.tool_name = "load_model";

    try {
        std::string model_name = args.value("model", "");
        if (model_name.empty()) {
            throw std::runtime_error("Missing required parameter: model");
        }

        // Check if already loaded
        if (router_->is_model_loaded(model_name)) {
            result.result_data = {{"model", model_name}, {"status", "already_loaded"}};
            result.llm_summary = "Model '" + model_name + "' is already loaded and ready.";
            result.success = true;
            return result;
        }

        // Use the existing ensure_model_loaded which handles download + load
        ensure_model_loaded(model_name);

        auto info = model_manager_->get_model_info(model_name);
        std::string model_type = "llm";
        for (const auto& label : info.labels) {
            if (label == "vision") { model_type = "vision"; break; }
            if (label == "audio") { model_type = "audio"; break; }
            if (label == "image") { model_type = "image"; break; }
            if (label == "tts") { model_type = "tts"; break; }
        }

        result.result_data = {
            {"model", model_name},
            {"status", "loaded"},
            {"type", model_type},
            {"recipe", info.recipe},
            {"size_gb", info.size}
        };
        result.llm_summary = "Model '" + model_name + "' (" + model_type + ", " +
                             std::to_string(info.size).substr(0, std::to_string(info.size).find('.') + 2) +
                             "GB) is now loaded and ready for inference.";
        result.success = true;
    } catch (const std::exception& e) {
        result.success = false;
        result.llm_summary = std::string("load_model failed: ") + e.what();
        result.result_data = {{"error", e.what()}};
    }

    return result;
}

// === Shell Tool Executor ===

ToolResult OmniLoop::execute_run_command(const json& args, const std::string& tool_call_id) {
    ToolResult result;
    result.tool_call_id = tool_call_id;
    result.tool_name = "run_command";

    try {
        std::string command = args.value("command", "");
        if (command.empty()) {
            throw std::runtime_error("Missing required parameter: command");
        }

        std::cout << "[OmniLoop] Running command: " << command << std::endl;

        // Redirect stderr to stdout so we capture everything
#ifdef _WIN32
        // Strip "powershell[.exe] [-options] -command" prefix if the LLM
        // wrapped the command, since we already execute in PowerShell.
        {
            std::string lower = command;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower.find("powershell") == 0) {
                auto pos = lower.find("-command");
                if (pos != std::string::npos) {
                    std::string inner = command.substr(pos + 8);
                    // Trim leading whitespace
                    size_t start = inner.find_first_not_of(" \t");
                    if (start != std::string::npos) inner = inner.substr(start);
                    // Strip surrounding quotes
                    if (inner.size() >= 2 &&
                        ((inner.front() == '"' && inner.back() == '"') ||
                         (inner.front() == '\'' && inner.back() == '\''))) {
                        inner = inner.substr(1, inner.size() - 2);
                    }
                    std::cout << "[OmniLoop] Stripped powershell wrapper" << std::endl;
                    command = inner;
                }
            }
        }

        // Write command to a temp .ps1 file and execute via PowerShell -File.
        // This avoids all quoting/escaping issues — the command goes straight
        // into the file as-is, no shell interpretation in between.
        std::string temp_dir = ".";
        if (auto* tmp = std::getenv("TEMP")) temp_dir = tmp;
        std::string ps1_file = temp_dir + "\\omni_" + tool_call_id + ".ps1";
        {
            std::ofstream f(ps1_file);
            f << command;
        }
        std::string full_cmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + ps1_file + "\" 2>&1";
#else
        std::string full_cmd = command + " 2>&1";
#endif

        std::string output;
        const size_t max_output = 100 * 1024; // 100KB cap

        FILE* pipe = popen(full_cmd.c_str(), "r");
        if (!pipe) {
            throw std::runtime_error("Failed to execute command");
        }

        char buffer[4096];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            output += buffer;
            if (output.size() > max_output) {
                output += "\n... (output truncated at 100KB)";
                break;
            }
        }

        int exit_code = pclose(pipe);
#ifdef _WIN32
        // On Windows, pclose returns the process exit code directly.
        // Clean up the temp .ps1 file.
        std::remove(ps1_file.c_str());
#else
        exit_code = WEXITSTATUS(exit_code);
#endif

        result.result_data = {
            {"command", command},
            {"output", output},
            {"exit_code", exit_code}
        };

        // Truncated preview for LLM context — keep short to avoid
        // blowing up the conversation when many commands run in a loop.
        std::string preview = output.substr(0, 2000);
        if (output.size() > 2000) {
            preview += "\n... (truncated, " + std::to_string(output.size()) + " bytes total)";
        }
        result.llm_summary = "$ " + command + "\n" + preview +
                             "\n(exit code: " + std::to_string(exit_code) + ")";
        result.success = (exit_code == 0);
    } catch (const std::exception& e) {
        result.success = false;
        result.llm_summary = std::string("run_command failed: ") + e.what();
        result.result_data = {{"error", e.what()}};
    }

    return result;
}

// === Tool Dispatcher ===

ToolResult OmniLoop::execute_external_tool(const json& tool_call, const OmniConfig& config) {
    ToolResult result;
    result.tool_call_id = tool_call.value("id", "");
    result.tool_name = tool_call["function"]["name"].get<std::string>();

    try {
        json args = json::parse(tool_call["function"]["arguments"].get<std::string>());

        json payload = {
            {"tool_call_id", result.tool_call_id},
            {"tool_name", result.tool_name},
            {"arguments", args}
        };

        std::cout << "[OmniLoop] Calling external tool '" << result.tool_name
                  << "' via callback: " << config.tool_callback_url << std::endl;

        auto http_response = utils::HttpClient::post(
            config.tool_callback_url,
            payload.dump(),
            {{"Content-Type", "application/json"}},
            config.tool_callback_timeout
        );

        if (http_response.status_code != 200) {
            result.success = false;
            result.llm_summary = "External tool callback returned HTTP " +
                                 std::to_string(http_response.status_code) + ": " + http_response.body;
            result.result_data = {{"error", result.llm_summary}};
            return result;
        }

        json resp = json::parse(http_response.body);
        result.success = resp.value("success", false);
        result.result_data = resp.value("result", json::object());
        result.llm_summary = resp.value("summary", "External tool returned no summary");
    } catch (const json::parse_error& e) {
        result.success = false;
        result.llm_summary = "External tool callback returned invalid JSON: " + std::string(e.what());
        result.result_data = {{"error", e.what()}};
    } catch (const std::exception& e) {
        result.success = false;
        result.llm_summary = "External tool call failed: " + std::string(e.what());
        result.result_data = {{"error", e.what()}};
    }

    return result;
}

ToolResult OmniLoop::execute_tool(const json& tool_call, const OmniConfig& config) {
    std::string tool_call_id = tool_call.value("id", "");
    std::string function_name = tool_call["function"]["name"].get<std::string>();
    json args = json::parse(tool_call["function"]["arguments"].get<std::string>());

    std::cout << "[OmniLoop] Executing tool: " << function_name
              << " (call_id: " << tool_call_id << ")" << std::endl;

    if (function_name == "generate_image") return execute_generate_image(args, tool_call_id, config);
    if (function_name == "edit_image") return execute_edit_image(args, tool_call_id, config);
    if (function_name == "describe_image") return execute_describe_image(args, tool_call_id, config);
    if (function_name == "analyze_image") return execute_analyze_image(args, tool_call_id, config);
    if (function_name == "transcribe_audio") return execute_transcribe_audio(args, tool_call_id, config);
    if (function_name == "text_to_speech") return execute_text_to_speech(args, tool_call_id, config);
    if (function_name == "read_file") return execute_read_file(args, tool_call_id);
    if (function_name == "write_file") return execute_write_file(args, tool_call_id);
    if (function_name == "list_directory") return execute_list_directory(args, tool_call_id);
    if (function_name == "web_search") return execute_web_search(args, tool_call_id);
    if (function_name == "list_models") return execute_list_models(args, tool_call_id);
    if (function_name == "load_model") return execute_load_model(args, tool_call_id);
    if (function_name == "run_command") return execute_run_command(args, tool_call_id);

    // Not a native tool — route to external callback if configured
    if (!config.tool_callback_url.empty()) {
        return execute_external_tool(tool_call, config);
    }

    ToolResult result;
    result.tool_call_id = tool_call_id;
    result.tool_name = function_name;
    result.success = false;
    result.llm_summary = "Unknown tool: " + function_name;
    result.result_data = {{"error", "Unknown tool: " + function_name}};
    return result;
}

// === Core Omni Loop (non-streaming) ===

json OmniLoop::run(const json& request) {
    auto config = OmniConfig::from_json(
        request.contains("omni") ? request["omni"] : json(),
        request.value("model", "")
    );

    std::cout << "[OmniLoop] Starting agent loop with brain_model=" << config.brain_model
              << ", max_iterations=" << config.max_iterations
              << ", tools=" << config.tools.size() << std::endl;

    // Ensure brain model is loaded
    ensure_model_loaded(config.brain_model);

    // Build tool definitions
    json tools = build_tool_definitions(config);

    json conversation = build_conversation(request, config);

    // Omni loop
    std::vector<OmniStep> steps;
    json final_response;

    for (int i = 0; i < config.max_iterations; i++) {
        // Build LLM request with tools
        json llm_request = {
            {"model", config.brain_model},
            {"messages", conversation},
            {"tools", tools},
            {"stream", false}
        };

        std::cout << "[OmniLoop] Iteration " << (i + 1) << "/" << config.max_iterations
                  << " - calling LLM" << std::endl;

        json llm_response = router_->chat_completion(llm_request);

        // Check for errors
        if (llm_response.contains("error")) {
            return llm_response;
        }

        // Extract assistant message
        auto& choice = llm_response["choices"][0];
        json assistant_msg = choice["message"];

        // If no tool_calls, this is the final response
        if (!assistant_msg.contains("tool_calls") || assistant_msg["tool_calls"].empty()) {
            std::cout << "[OmniLoop] No tool calls - final response" << std::endl;
            final_response = llm_response;
            break;
        }

        // Append assistant message with tool_calls to conversation
        conversation.push_back(assistant_msg);

        // Execute each tool call
        OmniStep step;
        step.step_number = i + 1;
        step.tool_calls = assistant_msg["tool_calls"];

        std::cout << "[OmniLoop] Step " << step.step_number
                  << ": " << assistant_msg["tool_calls"].size() << " tool call(s)" << std::endl;

        for (const auto& tool_call : assistant_msg["tool_calls"]) {
            ToolResult tool_result = execute_tool(tool_call, config);
            step.results.push_back(tool_result);

            // Append tool result to conversation (text summary only, for LLM context)
            conversation.push_back({
                {"role", "tool"},
                {"tool_call_id", tool_result.tool_call_id},
                {"content", tool_result.llm_summary}
            });
        }

        steps.push_back(step);

        // If this was the last iteration, do one final LLM call without tools
        if (i == config.max_iterations - 1) {
            std::cout << "[OmniLoop] Max iterations reached, final LLM call" << std::endl;

            // Trim conversation to avoid context overflow
            json trimmed_conversation = json::array();
            if (!conversation.empty()) {
                trimmed_conversation.push_back(conversation[0]); // system prompt
                const int tail_count = 10;
                int start = std::max(1, (int)conversation.size() - tail_count);
                for (int ci = start; ci < (int)conversation.size(); ci++) {
                    trimmed_conversation.push_back(conversation[ci]);
                }
            }
            trimmed_conversation.push_back({
                {"role", "user"},
                {"content", "You've used your tools to gather information. Now provide a clear, "
                            "helpful answer summarizing what you found. Do NOT call any more tools."}
            });

            json final_request = {
                {"model", config.brain_model},
                {"messages", trimmed_conversation},
                {"stream", false}
            };
            final_response = router_->chat_completion(final_request);

            // If final response has empty content, build a fallback
            if (final_response.contains("choices") && !final_response["choices"].empty()) {
                auto& fc = final_response["choices"][0];
                if (fc.contains("message")) {
                    auto& msg = fc["message"];
                    bool has_content = msg.contains("content") && msg["content"].is_string() && !msg["content"].get<std::string>().empty();
                    if (!has_content) {
                        std::string fallback = "Here's what I found:\n\n";
                        for (const auto& s : steps) {
                            for (const auto& r : s.results) {
                                if (r.success && !r.llm_summary.empty()) {
                                    fallback += "**" + r.tool_name + "**: " + r.llm_summary.substr(0, 500) + "\n\n";
                                }
                            }
                        }
                        msg["content"] = fallback;
                    }
                }
            }
        }
    }

    // Add omni_steps to response
    if (!final_response.is_null()) {
        json steps_json = json::array();
        for (const auto& step : steps) {
            json step_json = {
                {"step_number", step.step_number},
                {"tool_calls", step.tool_calls},
                {"results", json::array()}
            };
            for (const auto& r : step.results) {
                step_json["results"].push_back({
                    {"tool_call_id", r.tool_call_id},
                    {"tool_name", r.tool_name},
                    {"data", r.result_data},
                    {"summary", r.llm_summary},
                    {"success", r.success}
                });
            }
            steps_json.push_back(step_json);
        }
        final_response["omni_steps"] = steps_json;
    }

    std::cout << "[OmniLoop] Omni loop complete. Steps: " << steps.size() << std::endl;
    return final_response;
}

// === SSE Helper ===

void OmniLoop::send_sse_event(httplib::DataSink& sink, const std::string& event_type, const json& data) {
    std::string event = "event: " + event_type + "\ndata: " + data.dump() + "\n\n";
    sink.write(event.c_str(), event.size());
}

// === Streaming Omni Loop ===

void OmniLoop::run_stream(const json& request, httplib::DataSink& sink) {
    auto config = OmniConfig::from_json(
        request.contains("omni") ? request["omni"] : json(),
        request.value("model", "")
    );

    std::cout << "[OmniLoop] Starting streaming omni loop" << std::endl;

    try {
        ensure_model_loaded(config.brain_model);
    } catch (const std::exception& e) {
        send_sse_event(sink, "error", {{"message", e.what()}});
        sink.done();
        return;
    }

    json tools = build_tool_definitions(config);

    json conversation = build_conversation(request, config);

    std::vector<OmniStep> steps;

    for (int i = 0; i < config.max_iterations; i++) {
        json llm_request = {
            {"model", config.brain_model},
            {"messages", conversation},
            {"tools", tools},
            {"stream", false}
        };

        json llm_response;
        try {
            llm_response = router_->chat_completion(llm_request);
        } catch (const std::exception& e) {
            send_sse_event(sink, "error", {{"message", e.what()}});
            sink.done();
            return;
        }

        if (llm_response.contains("error")) {
            send_sse_event(sink, "error", llm_response["error"]);
            sink.done();
            return;
        }

        auto& choice = llm_response["choices"][0];
        json assistant_msg = choice["message"];

        // If no tool_calls, send final response and exit
        if (!assistant_msg.contains("tool_calls") || assistant_msg["tool_calls"].empty()) {
            std::string content;
            if (assistant_msg.contains("content") && assistant_msg["content"].is_string()) {
                content = assistant_msg["content"].get<std::string>();
            }

            send_sse_event(sink, "omni.response.delta", {{"content", content}});

            // Build steps array for the done event
            json steps_json = json::array();
            for (const auto& step : steps) {
                json step_json = {
                    {"step_number", step.step_number},
                    {"tool_calls", step.tool_calls},
                    {"results", json::array()}
                };
                for (const auto& r : step.results) {
                    step_json["results"].push_back({
                        {"tool_call_id", r.tool_call_id},
                        {"tool_name", r.tool_name},
                        {"data", r.result_data},
                        {"summary", r.llm_summary},
                        {"success", r.success}
                    });
                }
                steps_json.push_back(step_json);
            }

            json done_data = {{"omni_steps", steps_json}};
            if (llm_response.contains("usage")) {
                done_data["usage"] = llm_response["usage"];
            }
            send_sse_event(sink, "omni.response.done", done_data);
            sink.done();
            return;
        }

        // Tool calls present - process them
        conversation.push_back(assistant_msg);

        OmniStep step;
        step.step_number = i + 1;
        step.tool_calls = assistant_msg["tool_calls"];

        send_sse_event(sink, "omni.step.start", {
            {"step", step.step_number},
            {"tool_calls", step.tool_calls}
        });

        for (const auto& tool_call : assistant_msg["tool_calls"]) {
            ToolResult tool_result = execute_tool(tool_call, config);
            step.results.push_back(tool_result);

            send_sse_event(sink, "omni.step.result", {
                {"tool_call_id", tool_result.tool_call_id},
                {"tool_name", tool_result.tool_name},
                {"data", tool_result.result_data},
                {"summary", tool_result.llm_summary},
                {"success", tool_result.success}
            });

            conversation.push_back({
                {"role", "tool"},
                {"tool_call_id", tool_result.tool_call_id},
                {"content", tool_result.llm_summary}
            });
        }

        send_sse_event(sink, "omni.step.complete", {{"step", step.step_number}});
        steps.push_back(step);

        // If last iteration, do one more LLM call without tools to force a text response
        if (i == config.max_iterations - 1) {
            std::cout << "[OmniLoop] Max iterations reached (" << config.max_iterations
                      << "), making final LLM call without tools" << std::endl;

            // Trim conversation to last N messages to avoid context overflow.
            // Keep the system prompt (first message) + the most recent exchanges.
            json trimmed_conversation = json::array();
            if (!conversation.empty()) {
                trimmed_conversation.push_back(conversation[0]); // system prompt
                const int tail_count = 10;
                int start = std::max(1, (int)conversation.size() - tail_count);
                for (int ci = start; ci < (int)conversation.size(); ci++) {
                    trimmed_conversation.push_back(conversation[ci]);
                }
            }

            // Add an explicit instruction to summarize
            trimmed_conversation.push_back({
                {"role", "user"},
                {"content", "You've used your tools to gather information. Now provide a clear, "
                            "helpful answer summarizing what you found. Do NOT call any more tools."}
            });

            json final_request = {
                {"model", config.brain_model},
                {"messages", trimmed_conversation},
                {"stream", false}
            };

            json final_resp;
            try {
                final_resp = router_->chat_completion(final_request);
            } catch (const std::exception& e) {
                send_sse_event(sink, "error", {{"message", e.what()}});
                sink.done();
                return;
            }

            std::string content;
            if (final_resp.contains("choices") && !final_resp["choices"].empty()) {
                auto& fc = final_resp["choices"][0];
                if (fc.contains("message") && fc["message"].contains("content") && fc["message"]["content"].is_string()) {
                    content = fc["message"]["content"].get<std::string>();
                }
            }

            // Fallback: if the LLM still returned nothing, build a summary from tool results
            if (content.empty()) {
                std::cout << "[OmniLoop] Final LLM call returned empty content, building fallback summary" << std::endl;
                std::string fallback = "Here's what I found:\n\n";
                for (const auto& s : steps) {
                    for (const auto& r : s.results) {
                        if (r.success && !r.llm_summary.empty()) {
                            // Use a brief portion of each successful result
                            std::string summary_excerpt = r.llm_summary.substr(0, 500);
                            fallback += "**" + r.tool_name + "**: " + summary_excerpt + "\n\n";
                        }
                    }
                }
                content = fallback;
            }

            std::cout << "[OmniLoop] Final response length: " << content.size() << " chars" << std::endl;

            send_sse_event(sink, "omni.response.delta", {{"content", content}});

            json steps_json = json::array();
            for (const auto& s : steps) {
                json sj = {
                    {"step_number", s.step_number},
                    {"tool_calls", s.tool_calls},
                    {"results", json::array()}
                };
                for (const auto& r : s.results) {
                    sj["results"].push_back({
                        {"tool_call_id", r.tool_call_id},
                        {"tool_name", r.tool_name},
                        {"data", r.result_data},
                        {"summary", r.llm_summary},
                        {"success", r.success}
                    });
                }
                steps_json.push_back(sj);
            }

            json done_data = {{"omni_steps", steps_json}};
            if (final_resp.contains("usage")) {
                done_data["usage"] = final_resp["usage"];
            }
            send_sse_event(sink, "omni.response.done", done_data);
            sink.done();
            return;
        }
    }

    // Should not reach here, but just in case
    sink.done();
}

} // namespace lemon
