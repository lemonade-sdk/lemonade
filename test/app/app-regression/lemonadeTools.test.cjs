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
    name: 'buildLemonadeTools maps label-required tools to matching collection components',
    run() {
      const source = normalizeWhitespace(readSource(LEMONADE_TOOLS));
      const usesMainLabelSet = /const requiresLabels = def\.requires_labels[\s\S]*?const labelSet = new Set\(requiresLabels\)[\s\S]*?components\.find[\s\S]*?labels\.some\(l => labelSet\.has\(l\)\)[\s\S]*?models\[def\.function\.name\] = match/.test(source);
      const usesSharedLabelHelper = /const requiresLabels = def\.requires_labels[\s\S]*?const match = findComponentWithAnyLabel\(requiresLabels\)[\s\S]*?models\[def\.function\.name\] = match/.test(source)
        && /hasAnyModelLabel|componentHasAnyLabel/.test(source);

      assert.ok(
        usesMainLabelSet || usesSharedLabelHelper,
        'Tools with requires_labels should map to a matching component using either the main label-set path or the shared label helper path.',
      );
    },
  },
  {
    name: 'buildLemonadeTools maps LLM-required tools only through the selected planner model',
    run() {
      const source = normalizeWhitespace(readSource(LEMONADE_TOOLS));
      const usesMainPlannerModel = /const llmModel = components\.find[\s\S]*?NON_LLM_LABELS\.has\(l\)[\s\S]*?\|\| components\[0\] \|\| ''/.test(source)
        && /const requiresLlmLabels = def\.requires_llm_labels[\s\S]*?const llmLabels = modelsData\[llmModel\]\?\.labels \?\? \[\][\s\S]*?!llmLabels\.some\(l => labelSet\.has\(l\)\)\) continue[\s\S]*?models\[def\.function\.name\] = llmModel/.test(source);
      const usesCentralPlannerPredicate = /isChatPlannerCandidate/.test(source)
        && /const requiresLlmLabels = def\.requires_llm_labels[\s\S]*?const match = findComponentWithAnyLabel\(requiresLlmLabels, true\)[\s\S]*?models\[def\.function\.name\] = match/.test(source);

      assert.ok(
        usesMainPlannerModel || usesCentralPlannerPredicate,
        'Tools with requires_llm_labels should route through either the main planner model path or the centralized chat-planner predicate path.',
      );
    },
  },
  {
    name: 'image-size placeholders are materialized from the shared image config',
    run() {
      const source = normalizeWhitespace(readSource(LEMONADE_TOOLS));
      const config = readSource(IMAGE_CONFIG);
      assertMatches(config, /export const COLLECTION_IMAGE_SIZE = '512x256'/, 'Current collection image size should be explicit.');
      assertMatches(
        source,
        /prop\.description\.includes\('\{image_size\}'\)[\s\S]*?replaceAll\('\{image_size\}', COLLECTION_IMAGE_SIZE\)/,
        'Tool parameter descriptions should replace {image_size} at materialization time.',
      );
    },
  },
  {
    name: 'toolDefinitions encode both aliases for TTS and transcription labels',
    run() {
      const definitions = readToolDefinitions();
      const byName = Object.fromEntries(definitions.tools.map((tool) => [tool.function.name, tool]));
      assert.deepEqual(byName.text_to_speech.requires_labels, ['tts', 'speech']);
      assert.deepEqual(byName.transcribe_audio.requires_labels, ['audio', 'transcription']);
      assert.deepEqual(byName.analyze_image.requires_llm_labels, ['vision']);
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
      assertMatches(
        source,
        /rawImageUrl\.startsWith\('data:image\/'\) \? rawImageUrl : ''[\s\S]*?context\.extractedImages/,
        'Vision analysis should only use LLM-provided image_url when it is a data:image URL, otherwise fall back to extracted images.',
      );
    },
  },
];

module.exports = { tests };
