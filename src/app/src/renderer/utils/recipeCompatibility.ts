/**
 * Recipe-aware model compatibility classification.
 *
 * Maps HuggingFace model metadata (pipeline_tag, tags, model ID) to Lemonade
 * recipes and compatibility levels. Task takes priority over format — GGUF is
 * a container format, not a task indicator.
 *
 * Mirrors server-side logic in model_types.h (get_model_type_from_labels)
 * and model_manager.cpp (register_user_model label assignment).
 */

export interface TaskRecipeMapping {
  pipelineTags: string[];
  hfTags: string[];
  namePatterns: RegExp[];
  recipe: string;
  modelType: string;
  label: string;
}

/**
 * Task-to-recipe mapping table.
 * Order matters: first match wins. LLM is the fallback and not listed here.
 */
export const TASK_RECIPE_MAP: TaskRecipeMapping[] = [
  {
    pipelineTags: ['text-to-image', 'image-to-image'],
    hfTags: ['stable-diffusion', 'text-to-image', 'diffusers'],
    namePatterns: [/stable-diffusion/i, /\bflux\b/i, /\bsdxl\b/i],
    recipe: 'sd-cpp',
    modelType: 'image',
    label: 'sd.cpp',
  },
  {
    pipelineTags: ['automatic-speech-recognition'],
    hfTags: ['whisper'],
    namePatterns: [/whisper/i],
    recipe: 'whispercpp',
    modelType: 'audio',
    label: 'whisper.cpp',
  },
  {
    pipelineTags: ['text-to-speech', 'text-to-audio'],
    hfTags: ['tts', 'kokoro'],
    namePatterns: [/kokoro/i],
    recipe: 'kokoro',
    modelType: 'tts',
    label: 'Kokoro',
  },
];

/** Pipeline tags that indicate an LLM (including multimodal/vision LLMs). */
const LLM_PIPELINE_TAGS = ['text-generation', 'conversational', 'text2text-generation', 'image-text-to-text'];

export type CompatibilityLevel = 'supported' | 'likely' | 'experimental' | 'incompatible';

export interface ModelCompatibility {
  recipe: string;
  modelType: string;
  label: string;
  level: CompatibilityLevel;
  reason: string;
}

export interface ClassifyInput {
  modelId: string;
  pipelineTag?: string;
  tags: string[];
  hasGgufFiles: boolean;
  hasOnnxFiles: boolean;
  hasFlmFiles: boolean;
  hasBinFiles: boolean;
}

/**
 * Classify a HuggingFace model into a Lemonade recipe with a confidence level.
 *
 * Priority order:
 *   1. pipeline_tag match against TASK_RECIPE_MAP  → supported
 *   2. pipeline_tag is a known LLM tag + GGUF      → supported
 *   3. HF tags match against TASK_RECIPE_MAP        → likely
 *   4. Model ID name pattern match                  → experimental
 *   5. FLM files/tags (format-specific)             → likely
 *   6. ONNX files (format-specific)                 → likely
 *   7. GGUF present, no other signals               → experimental (was silently "supported")
 *   8. Nothing matched                              → incompatible
 */
export function classifyModel(input: ClassifyInput): ModelCompatibility {
  const { modelId, pipelineTag, tags, hasGgufFiles, hasOnnxFiles, hasFlmFiles, hasBinFiles } = input;
  const idLower = modelId.toLowerCase();

  // --- Pass 1: pipeline_tag (highest confidence) ---

  if (pipelineTag) {
    // Check non-LLM mappings first
    for (const mapping of TASK_RECIPE_MAP) {
      if (mapping.pipelineTags.includes(pipelineTag)) {
        return {
          recipe: mapping.recipe,
          modelType: mapping.modelType,
          label: mapping.label,
          level: 'supported',
          reason: `Task "${pipelineTag}" maps to ${mapping.label}`,
        };
      }
    }

    // Known LLM pipeline tag
    if (LLM_PIPELINE_TAGS.includes(pipelineTag)) {
      if (hasGgufFiles) {
        return {
          recipe: 'llamacpp',
          modelType: 'llm',
          label: 'llama.cpp',
          level: 'supported',
          reason: `Task "${pipelineTag}" with GGUF files`,
        };
      }
      if (hasOnnxFiles) {
        return {
          recipe: 'ryzenai-llm',
          modelType: 'llm',
          label: 'RyzenAI',
          level: 'likely',
          reason: `Task "${pipelineTag}" with ONNX files`,
        };
      }
    }

    // pipeline_tag present but doesn't match anything we support
    // (e.g. "feature-extraction", "fill-mask", "summarization", etc.)
    if (!LLM_PIPELINE_TAGS.includes(pipelineTag)) {
      return {
        recipe: '',
        modelType: 'unknown',
        label: pipelineTag,
        level: 'incompatible',
        reason: `Task "${pipelineTag}" is not supported by any Lemonade backend`,
      };
    }
  }

  // --- Pass 2: HF tags (medium confidence) ---

  for (const mapping of TASK_RECIPE_MAP) {
    if (mapping.hfTags.some(t => tags.includes(t))) {
      return {
        recipe: mapping.recipe,
        modelType: mapping.modelType,
        label: mapping.label,
        level: 'likely',
        reason: `Repository tags suggest ${mapping.label} model`,
      };
    }
  }

  // --- Pass 3: Model ID name patterns (low confidence) ---

  for (const mapping of TASK_RECIPE_MAP) {
    if (mapping.namePatterns.some(p => p.test(idLower))) {
      return {
        recipe: mapping.recipe,
        modelType: mapping.modelType,
        label: mapping.label,
        level: 'experimental',
        reason: `Model name suggests ${mapping.label} — no confirming metadata`,
      };
    }
  }

  // --- Pass 4: Format-only fallbacks ---

  // FLM detection
  if (hasFlmFiles || idLower.startsWith('fastflowlm/') || tags.includes('flm')) {
    return {
      recipe: 'flm',
      modelType: 'llm',
      label: 'FastFlowLM',
      level: 'likely',
      reason: 'FLM files or tags detected',
    };
  }

  // ONNX detection (without LLM pipeline_tag — lower confidence)
  if (hasOnnxFiles) {
    let recipe = 'ryzenai-llm';
    let label = 'RyzenAI';
    if (idLower.includes('-ryzenai-npu') || tags.includes('npu')) { recipe = 'ryzenai-llm'; label = 'RyzenAI NPU'; }
    else if (idLower.includes('-ryzenai-hybrid') || tags.includes('hybrid')) { recipe = 'ryzenai-llm'; label = 'RyzenAI Hybrid'; }
    else if (tags.includes('igpu')) { recipe = 'ryzenai-llm'; label = 'RyzenAI iGPU'; }
    return {
      recipe,
      modelType: 'llm',
      label,
      level: 'likely',
      reason: 'ONNX files detected',
    };
  }

  // GGUF present but no task metadata — this is the case that was causing issues
  if (hasGgufFiles) {
    return {
      recipe: 'llamacpp',
      modelType: 'llm',
      label: 'llama.cpp',
      level: 'experimental',
      reason: 'GGUF files present but no task metadata — assuming LLM',
    };
  }

  // Nothing matched
  return {
    recipe: '',
    modelType: 'unknown',
    label: 'Unknown',
    level: 'incompatible',
    reason: 'No compatible format or task metadata detected',
  };
}
