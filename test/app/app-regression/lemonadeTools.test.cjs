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
    name: 'buildLemonadeTools keeps component matching separate from planner matching',
    run() {
      const source = normalizeWhitespace(readSource(LEMONADE_TOOLS));
      assertMatches(
        source,
        /requiresLabels[\s\S]*?components\.find[\s\S]*?models\[def\.function\.name\] = match/,
        'Tools with requires_labels should map to a concrete matching component.',
      );
      const hasPlannerPath = /requiresLlmLabels[\s\S]*?models\[def\.function\.name\] = llmModel/.test(source)
        || /requiresLlmLabels[\s\S]*?models\[def\.function\.name\] = match/.test(source);
      assert.ok(hasPlannerPath, 'Tools with requires_llm_labels should route through the selected planner path.');
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
