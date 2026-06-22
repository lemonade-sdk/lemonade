const assert = require('node:assert/strict');
const {
  readSource,
  normalizeWhitespace,
  assertIncludes,
  assertMatches,
} = require('./helpers/source.cjs');

const LEMONADE_TOOLS = 'src/app/src/renderer/utils/lemonadeTools.ts';
const TOOL_DEFINITIONS = 'src/app/src/renderer/utils/toolDefinitions.json';
const IMAGE_CONFIG = 'src/app/src/renderer/utils/collectionImageConfig.ts';
const LLM_CHAT_PANEL = 'src/app/src/renderer/components/panels/LLMChatPanel.tsx';

function readToolDefinitions() {
  return JSON.parse(readSource(TOOL_DEFINITIONS));
}

const tests = [
  {
    name: 'tool definitions expose the expected OmniRouter tool names',
    run() {
      const names = readToolDefinitions().tools.map((tool) => tool.function.name).sort();
      assert.deepEqual(names, [
        'analyze_image',
        'edit_image',
        'generate_image',
        'text_to_speech',
        'transcribe_audio',
      ]);
    },
  },
  {
    name: 'tool definitions keep canonical routing labels for each tool family',
    run() {
      const definitions = readToolDefinitions();
      const byName = Object.fromEntries(definitions.tools.map((tool) => [tool.function.name, tool]));
      assert.ok(byName.generate_image.requires_labels.includes('image'), 'Image generation should require image capability.');
      assert.ok(byName.edit_image.requires_labels.includes('edit'), 'Image editing should require edit capability.');
      assert.ok(byName.text_to_speech.requires_labels.some((label) => label === 'tts' || label === 'speech'), 'TTS should require a speech/TTS label.');
      assert.ok(byName.transcribe_audio.requires_labels.some((label) => label === 'audio' || label === 'transcription'), 'Transcription should require an audio/transcription label.');
      assert.deepEqual(byName.analyze_image.requires_llm_labels, ['vision']);
    },
  },
  {
    name: 'image tools keep default size optional and expose only one canvas-size argument',
    run() {
      const definitions = readToolDefinitions();
      const byName = Object.fromEntries(definitions.tools.map((tool) => [tool.function.name, tool]));
      assert.deepEqual(byName.generate_image.function.parameters.required, ['prompt']);
      assert.deepEqual(byName.edit_image.function.parameters.required, ['prompt']);

      const generateProps = byName.generate_image.function.parameters.properties;
      const editProps = byName.edit_image.function.parameters.properties;

      for (const props of [generateProps, editProps]) {
        assertIncludes(
          props.size.description,
          'Omit by default',
          'The planner should not have to emit a default size argument for every image request.',
        );
        assertIncludes(props.size.description, 'output', 'Size should be framed as output canvas, not content detail.');
        assertIncludes(
          props.size.description,
          'nearest multiple of 8',
          'Schema description should match the executor rounding behavior.',
        );
        assert.ok(!('width' in props), 'Canvas width should not duplicate the size string argument.');
        assert.ok(!('height' in props), 'Canvas height should not duplicate the size string argument.');
        assert.ok(!('aspect_ratio' in props), 'Aspect ratio should not be an extra planner schema field.');
        assert.ok(!('orientation' in props), 'Orientation should not be an extra planner schema field.');
      }
    },
  },
  {
    name: 'image size resolver does not treat prompt WxH text as canvas size',
    run() {
      const source = normalizeWhitespace(readSource(LEMONADE_TOOLS));
      assert.ok(
        !source.includes('parseSizeFromText(typeof args.prompt'),
        'Prompt text like "100x100 pixel-art grid" should not override the image canvas size.',
      );
      assertIncludes(
        source,
        'Prompt text is always image content',
        'The resolver should document why prompt text is not parsed as output dimensions.',
      );
      assert.ok(
        !source.includes('SIZE_HINT_TO_SIZE') && !source.includes('ASPECT_RATIO_TO_SIZE'),
        'Prompt words such as portrait/square/landscape should not change the default canvas size.',
      );
      assertIncludes(
        source,
        'const IMAGE_DIMENSION_STEP = 8',
        'Explicit dimensions should round to model-supported 8-pixel alignment.',
      );
      assertIncludes(
        source,
        'Math.round(value / IMAGE_DIMENSION_STEP) * IMAGE_DIMENSION_STEP',
        'Explicit dimensions should round to the nearest 8-pixel multiple rather than falling back unexpectedly.',
      );
    },
  },
  {
    name: 'buildLemonadeTools keeps component matching separate from planner matching',
    run() {
      const source = normalizeWhitespace(readSource(LEMONADE_TOOLS));
      // The include() helper maps each included tool's name to its resolved model.
      assertMatches(
        source,
        /const include = \(def[\s\S]*?models\[def\.function\.name\] = model/,
        'The include helper should map each tool name to its resolved model.',
      );
      assertMatches(
        source,
        /requiresLabels[\s\S]*?components\.find[\s\S]*?include\(def, match\)/,
        'Tools with requires_labels should map to a concrete matching component.',
      );
      const hasPlannerPath = /requiresLlmLabels[\s\S]*?include\(def, llmModel\)/.test(source);
      assert.ok(hasPlannerPath, 'Tools with requires_llm_labels should route through the selected planner path.');
    },
  },
  {
    name: 'unavailable tool guidance is omitted from the planner prompt',
    run() {
      const source = normalizeWhitespace(readSource(LEMONADE_TOOLS));
      const definitions = readToolDefinitions();
      assertIncludes(definitions.system_prompt, '{tool_guidance}', 'Conditional guidance should be injected at runtime.');
      assert.ok(!definitions.system_prompt.includes('flow_shift'), 'Static prompt should not mention image-only options.');
      assertIncludes(
        source,
        'const guidance: string[] = []',
        'Planner guidance should be collected only from included tools.',
      );
      assertIncludes(
        source,
        'if (def.prompt_guidance) guidance.push(substituteImageSize(def.prompt_guidance));',
        'Only included tool prompt_guidance should be added to the planner prompt.',
      );
      assertIncludes(
        source,
        ".replace('{tool_guidance}', toolGuidance)",
        'The 2071 server/frontend shared prompt should use the tool_guidance placeholder.',
      );
    },
  },
  {
    name: 'collection UI skips exact duplicate generate_image calls without blocking distinct image requests',
    run() {
      const source = normalizeWhitespace(readSource(LLM_CHAT_PANEL));
      assertIncludes(
        source,
        'const generatedImageRequestKeys = new Set<string>()',
        'The duplicate guard should track exact generate_image requests, not just whether any image exists.',
      );
      assertIncludes(
        source,
        'getImageToolRequestKey(toolCall)',
        'Duplicate detection should compare prompt/options so distinct image requests still run.',
      );
      assertIncludes(
        source,
        'Duplicate image generation skipped because this exact image request was already generated for this turn.',
        'Repeated identical generate_image tool calls in one request should not render duplicate images.',
      );
    },
  },
  {
    name: 'collection UI preserves planner agency for image edit tool choice',
    run() {
      const source = normalizeWhitespace(readSource(LLM_CHAT_PANEL));
      assert.ok(!source.includes('isLikelyImageEditRequest'), 'Regex edit-intent parsing should not override the planner tool choice.');
      assert.ok(!source.includes('withToolName(toolCall'), 'The UI should not rewrite generate_image tool calls into edit_image calls.');
      assert.ok(!source.includes('EDIT_INTENT_RE'), 'Edit routing should be planner-driven, not regex-driven.');
      assertMatches(source, /const funcName = toolCall\.function\.name;[\s\S]*?executeLemonadeTool\(toolCall, toolModel,/, 'The executed tool should match the planner-selected tool.');

      const definitions = readToolDefinitions();
      const byName = Object.fromEntries(definitions.tools.map((tool) => [tool.function.name, tool]));
      assertIncludes(
        byName.edit_image.prompt_guidance,
        'use edit_image rather than generate_image',
        'Prompt guidance should tell the planner when to choose edit_image.',
      );
      assert.ok(
        !('prompt_guidance' in byName.generate_image),
        'Generate-image canvas guidance belongs in the size parameter description, not duplicated in prompt_guidance.',
      );
    },
  },
  {
    name: 'image-size placeholders are materialized from the shared image config',
    run() {
      const source = normalizeWhitespace(readSource(LEMONADE_TOOLS));
      const config = readSource(IMAGE_CONFIG);
      // The collection image size is a single source of truth in
      // toolDefinitions.json, shared with the C++ server-side orchestrator.
      assert.equal(
        readToolDefinitions().image_size,
        '512x256',
        'Collection image size should be declared in toolDefinitions.json.',
      );
      // collectionImageConfig.ts derives the value from the JSON rather than
      // hardcoding it, so the desktop app and server cannot drift.
      assertMatches(
        config,
        /export const COLLECTION_IMAGE_SIZE = toolDefinitions\.image_size/,
        'COLLECTION_IMAGE_SIZE should be derived from toolDefinitions.image_size.',
      );
      assertMatches(
        source,
        /const substituteImageSize = \(text: string\): string => \([\s\S]*?text\.replace\(\/\\\{image_size\\\}\/g, COLLECTION_IMAGE_SIZE\)/,
        'Tool parameter descriptions and prompt guidance should share the image-size substitution helper.',
      );
      assertIncludes(
        source,
        'substituteImageSize(prop.description)',
        'Tool parameter descriptions should replace {image_size} at materialization time.',
      );
      assertIncludes(
        source,
        'substituteImageSize(def.prompt_guidance)',
        'Tool prompt guidance should replace {image_size} at materialization time.',
      );
    },
  },
  {
    name: 'transcription instructions are not duplicated in prompt guidance',
    run() {
      const definitions = readToolDefinitions();
      const byName = Object.fromEntries(definitions.tools.map((tool) => [tool.function.name, tool]));
      assert.ok(
        !('prompt_guidance' in byName.transcribe_audio),
        'Transcription guidance should live in the tool description rather than duplicate prompt_guidance.',
      );
      assertIncludes(
        byName.transcribe_audio.function.description,
        'The audio data is automatically provided by the system, just call this tool with the language parameter.',
        'The simplified transcription description should keep the operational instruction.',
      );
    },
  },
  {
    name: 'tool execution routes each tool to its dedicated OpenAI-compatible endpoint',
    run() {
      const source = normalizeWhitespace(readSource(LEMONADE_TOOLS));
      assertIncludes(source, "serverFetch('/images/generations'", 'generate_image should call /images/generations.');
      assertIncludes(source, "serverFetch('/images/edits'", 'edit_image should call /images/edits.');
      assertIncludes(source, "serverFetch('/audio/speech'", 'text_to_speech should call /audio/speech.');
      assertIncludes(source, "serverFetch('/audio/transcriptions'", 'transcribe_audio should call /audio/transcriptions.');
      assertIncludes(source, "serverFetch('/chat/completions'", 'analyze_image should call /chat/completions.');
    },
  },
  {
    name: 'vision tool rejects arbitrary non-data image URLs before calling chat completions',
    run() {
      const source = normalizeWhitespace(readSource(LEMONADE_TOOLS));
      assertIncludes(
        source,
        "rawImageUrl.startsWith('data:image/')",
        'Vision analysis should only forward explicit LLM image_url values when they are data:image URLs.',
      );
      assertMatches(
        source,
        /if \(!imageUrl && context\.extractedImages\.length > 0\)[\s\S]*?context\.extractedImages\[context\.extractedImages\.length - 1\]\.dataUrl/,
        'Vision analysis should fall back to the user-provided extracted image data URL.',
      );
    },
  },
];

module.exports = { tests };
