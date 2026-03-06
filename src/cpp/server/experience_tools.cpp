#include "lemon/experience_tools.h"
#include <lemon/utils/aixlog.hpp>
#include <algorithm>
#include <set>

namespace lemon {

// Labels that indicate non-LLM models (mirrors frontend NON_LLM_LABELS)
static const std::set<std::string> NON_LLM_LABELS = {
    "image", "speech", "tts", "audio", "transcription",
    "embeddings", "embedding", "reranking"
};

// Labels that map to image generation
static const std::set<std::string> IMAGE_LABELS = {"image"};

// Labels that map to text-to-speech
static const std::set<std::string> TTS_LABELS = {"tts", "speech"};

// Labels that map to audio transcription (speech-to-text)
static const std::set<std::string> TRANSCRIPTION_LABELS = {"audio", "transcription"};

// Labels that indicate vision capability on the LLM
static const std::set<std::string> VISION_LABELS = {"vision"};

// Labels that are skipped (embeddings/reranking not useful in chat)
static const std::set<std::string> SKIP_LABELS = {
    "embeddings", "embedding", "reranking"
};

static bool has_label(const std::vector<std::string>& labels, const std::set<std::string>& target) {
    for (const auto& label : labels) {
        if (target.count(label)) return true;
    }
    return false;
}

bool is_experience_model(const std::string& model_name, ModelManager* mm) {
    if (!mm->model_exists(model_name)) return false;
    auto info = mm->get_model_info(model_name);
    return info.recipe == "experience" && !info.composite_models.empty();
}

std::string get_experience_llm_model(const std::string& experience_name, ModelManager* mm) {
    auto info = mm->get_model_info(experience_name);

    for (const auto& component : info.composite_models) {
        if (!mm->model_exists(component)) continue;
        auto comp_info = mm->get_model_info(component);
        bool is_non_llm = false;
        for (const auto& label : comp_info.labels) {
            if (NON_LLM_LABELS.count(label)) {
                is_non_llm = true;
                break;
            }
        }
        if (!is_non_llm) return component;
    }

    // Fallback to first component
    return info.composite_models.empty() ? experience_name : info.composite_models[0];
}

json build_experience_tools(const std::string& experience_name, ModelManager* mm) {
    auto info = mm->get_model_info(experience_name);
    json tools = json::array();

    // Check if the LLM component has vision capability
    std::string llm_model = get_experience_llm_model(experience_name, mm);
    bool llm_has_vision = false;
    if (mm->model_exists(llm_model)) {
        auto llm_info = mm->get_model_info(llm_model);
        llm_has_vision = has_label(llm_info.labels, VISION_LABELS);
    }

    for (const auto& component : info.composite_models) {
        if (!mm->model_exists(component)) continue;
        auto comp_info = mm->get_model_info(component);

        if (has_label(comp_info.labels, SKIP_LABELS)) {
            continue;
        }

        if (has_label(comp_info.labels, IMAGE_LABELS)) {
            json tool = {
                {"type", "function"},
                {"function", {
                    {"name", "generate_image"},
                    {"description", "Generate a NEW image from scratch based on a text description. Use this ONLY when the user asks you to create an entirely new image. Do NOT use this to modify or change an existing image — use edit_image instead."},
                    {"parameters", {
                        {"type", "object"},
                        {"properties", {
                            {"prompt", {
                                {"type", "string"},
                                {"description", "A detailed description of the image to generate"}
                            }},
                            {"size", {
                                {"type", "string"},
                                {"description", "Image size (e.g. '512x512', '1024x1024')"},
                                {"default", "512x512"}
                            }}
                        }},
                        {"required", json::array({"prompt"})}
                    }}
                }}
            };
            tools.push_back(tool);
            LOG(DEBUG, "ExperienceTools") << "Added generate_image tool for component: " << component << std::endl;

            json edit_tool = {
                {"type", "function"},
                {"function", {
                    {"name", "edit_image"},
                    {"description", "Edit or modify a previously generated image. Use this when the user wants to add, remove, change, modify, update, fix, or adjust anything in an existing image from this conversation. The most recently generated image is used automatically as the source. Always prefer this over generate_image when an image already exists in the conversation."},
                    {"parameters", {
                        {"type", "object"},
                        {"properties", {
                            {"prompt", {
                                {"type", "string"},
                                {"description", "A description of the desired edit or modification to apply to the image"}
                            }},
                            {"size", {
                                {"type", "string"},
                                {"description", "Output image size (e.g. '512x512', '1024x1024')"},
                                {"default", "512x512"}
                            }}
                        }},
                        {"required", json::array({"prompt"})}
                    }}
                }}
            };
            tools.push_back(edit_tool);
            LOG(DEBUG, "ExperienceTools") << "Added edit_image tool for component: " << component << std::endl;
        }

        if (has_label(comp_info.labels, TTS_LABELS)) {
            json tool = {
                {"type", "function"},
                {"function", {
                    {"name", "text_to_speech"},
                    {"description", "Convert text to spoken audio. Use this when the user asks you to speak, say, read aloud, or convert text to speech."},
                    {"parameters", {
                        {"type", "object"},
                        {"properties", {
                            {"input", {
                                {"type", "string"},
                                {"description", "The text to convert to speech"}
                            }},
                            {"voice", {
                                {"type", "string"},
                                {"description", "Voice to use for speech synthesis"},
                                {"default", "af_heart"}
                            }}
                        }},
                        {"required", json::array({"input"})}
                    }}
                }}
            };
            tools.push_back(tool);
            LOG(DEBUG, "ExperienceTools") << "Added text_to_speech tool for component: " << component << std::endl;
        }

        if (has_label(comp_info.labels, TRANSCRIPTION_LABELS)) {
            json tool = {
                {"type", "function"},
                {"function", {
                    {"name", "transcribe_audio"},
                    {"description", "Transcribe audio to text (speech-to-text). Use this when the user provides an audio file or when you see '[User provided audio file #N]' placeholders in the conversation. The audio data is automatically provided by the system — just call this tool with the language parameter."},
                    {"parameters", {
                        {"type", "object"},
                        {"properties", {
                            {"language", {
                                {"type", "string"},
                                {"description", "Language of the audio (ISO 639-1 code, e.g. 'en', 'es', 'fr')"},
                                {"default", "en"}
                            }}
                        }},
                        {"required", json::array()}
                    }}
                }}
            };
            tools.push_back(tool);
            LOG(DEBUG, "ExperienceTools") << "Added transcribe_audio tool for component: " << component << std::endl;
        }
    }

    // If the LLM has vision capability, add analyze_image tool (routes back to LLM itself)
    if (llm_has_vision) {
        json tool = {
            {"type", "function"},
            {"function", {
                {"name", "analyze_image"},
                {"description", "Analyze, describe, or answer questions about an image. Use this when the user shares an image and asks you to look at it, describe it, read text from it, identify objects, or answer any question about what's in the image."},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"image_url", {
                            {"type", "string"},
                            {"description", "The URL or base64 data URI of the image to analyze"}
                        }},
                        {"question", {
                            {"type", "string"},
                            {"description", "The question to answer about the image, or 'describe' for a general description"}
                        }}
                    }},
                    {"required", json::array({"image_url", "question"})}
                }}
            }}
        };
        tools.push_back(tool);
        LOG(DEBUG, "ExperienceTools") << "Added analyze_image tool (VLM LLM: " << llm_model << ")" << std::endl;
    }

    return tools;
}

std::string build_experience_system_prompt(const json& tools) {
    std::string prompt = "You are a helpful multimodal AI assistant with access to the following tools:\n\n";

    for (const auto& tool : tools) {
        const auto& func = tool["function"];
        prompt += "- " + func["name"].get<std::string>() + ": " +
                  func["description"].get<std::string>() + "\n";
    }

    prompt += "\nWhen the user asks you to perform an action that matches one of these tools, "
              "use the appropriate tool. You may call multiple tools if the request requires it. "
              "After using a tool, describe what you did to the user in a brief, friendly response. "
              "If the user's request does not require any tool, respond normally with text. "
              "IMPORTANT: When an image has already been generated in this conversation and the user wants "
              "to add something, remove something, change, modify, or adjust the image in any way, you MUST "
              "use the edit_image tool — NOT generate_image. Only use generate_image for creating a brand new "
              "image from scratch. The edit_image tool automatically uses the most recent image as its source. "
              "When the user sends an image (as an image_url in their message), use the analyze_image tool "
              "to look at the image before responding about it. "
              "When you see '[User provided audio file #N]' in a message, it means the user sent audio data. "
              "Call the transcribe_audio tool to transcribe it — the audio data is handled automatically by the system.";

    return prompt;
}

std::optional<ExperienceToolInfo> resolve_tool_call(
    const std::string& function_name,
    const std::string& experience_name,
    ModelManager* mm) {

    auto info = mm->get_model_info(experience_name);

    for (const auto& component : info.composite_models) {
        if (!mm->model_exists(component)) continue;
        auto comp_info = mm->get_model_info(component);

        if ((function_name == "generate_image" || function_name == "edit_image") &&
            has_label(comp_info.labels, IMAGE_LABELS)) {
            return ExperienceToolInfo{
                function_name,
                component,
                ModelType::IMAGE
            };
        }

        if (function_name == "text_to_speech" && has_label(comp_info.labels, TTS_LABELS)) {
            return ExperienceToolInfo{
                "text_to_speech",
                component,
                ModelType::TTS
            };
        }

        if (function_name == "transcribe_audio" && has_label(comp_info.labels, TRANSCRIPTION_LABELS)) {
            return ExperienceToolInfo{
                "transcribe_audio",
                component,
                ModelType::AUDIO
            };
        }
    }

    // analyze_image routes back to the LLM itself
    if (function_name == "analyze_image") {
        std::string llm_model = get_experience_llm_model(experience_name, mm);
        if (mm->model_exists(llm_model)) {
            auto llm_info = mm->get_model_info(llm_model);
            if (has_label(llm_info.labels, VISION_LABELS)) {
                return ExperienceToolInfo{
                    "analyze_image",
                    llm_model,
                    ModelType::LLM
                };
            }
        }
    }

    LOG(WARNING, "ExperienceTools") << "Could not resolve tool call: " << function_name
                                    << " for experience: " << experience_name << std::endl;
    return std::nullopt;
}

} // namespace lemon
