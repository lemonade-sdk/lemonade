const assert = require('node:assert/strict');
const {
  readSource,
  normalizeWhitespace,
  assertIncludes,
  assertMatches,
} = require('./helpers/source.cjs');

const COLLECTION_MODELS = 'src/app/src/renderer/utils/collectionModels.ts';

function isExported(source, name) {
  return new RegExp(`export\\s+(?:const|function)\\s+${name}\\b|export\\s*\\{[^}]*\\b${name}\\b[^}]*\\}`).test(source);
}

const tests = [
  {
    name: 'getCollectionComponents filters invalid component names without reordering',
    run() {
      const source = normalizeWhitespace(readSource(COLLECTION_MODELS));
      assertMatches(
        source,
        /Array\.isArray\(info\.composite_models\)[\s\S]*?info\.composite_models\.filter\(/,
        'getCollectionComponents should only read array-valued composite_models.',
      );
      assertIncludes(
        source,
        "typeof name === 'string' && name.length > 0",
        'getCollectionComponents should filter to non-empty string component names.',
      );
      assert.ok(
        !/\.sort\(/.test(source.slice(source.indexOf('getCollectionComponents'), source.indexOf('export const isCollectionModel'))),
        'Component order is semantically meaningful and must not be sorted.',
      );
    },
  },
  {
    name: 'isCollectionModel requires recipe=collection and at least one component',
    run() {
      const source = normalizeWhitespace(readSource(COLLECTION_MODELS));
      assertMatches(
        source,
        /isCollectionModel[\s\S]*?info\.recipe === 'collection'[\s\S]*?getCollectionComponents\(info\)\.length > 0/,
        'A collection model must be recipe=collection with at least one concrete component.',
      );
    },
  },
  {
    name: 'collection downloaded state requires every component to be downloaded',
    run() {
      const source = normalizeWhitespace(readSource(COLLECTION_MODELS));
      assertMatches(
        source,
        /isModelEffectivelyDownloaded[\s\S]*?isCollectionModel\(info\)[\s\S]*?isCollectionFullyDownloaded\(modelName, modelsData\)[\s\S]*?info\?\.downloaded === true/,
        'Downloaded state must delegate collections to component completeness and concrete models to info.downloaded.',
      );
      assertMatches(
        source,
        /isCollectionFullyDownloaded[\s\S]*?components\.length === 0\) return false[\s\S]*?components\.every\(\(component\) => modelsData\[component\]\?\.downloaded === true\)/,
        'A collection is downloaded only when every component is downloaded.',
      );
    },
  },
  {
    name: 'collection loaded state requires every component to be loaded',
    run() {
      const source = normalizeWhitespace(readSource(COLLECTION_MODELS));
      assertMatches(
        source,
        /isModelEffectivelyLoaded[\s\S]*?isCollectionModel\(info\)[\s\S]*?isCollectionFullyLoaded\(modelName, modelsData, loadedModels\)[\s\S]*?loadedModels\.has\(modelName\)/,
        'Loaded state must delegate collections to component completeness and concrete models to loadedModels.has(modelName).',
      );
      assertMatches(
        source,
        /isCollectionFullyLoaded[\s\S]*?components\.length === 0\) return false[\s\S]*?components\.every\(\(component\) => loadedModels\.has\(component\)\)/,
        'A collection is loaded only when every component is loaded.',
      );
    },
  },
  {
    name: 'getCollectionImageModel picks the image-labeled component only',
    run() {
      const source = normalizeWhitespace(readSource(COLLECTION_MODELS));
      assertMatches(
        source,
        /getCollectionImageModel[\s\S]*?components\.find[\s\S]*?modelsData\[component\][\s\S]*?labels\?\.includes\('image'\)[\s\S]*?imageModel \|\| null/,
        'Image tool routing must pick the component with the image label and otherwise return null.',
      );
    },
  },
  {
    name: 'getCollectionPrimaryChatModel skips concrete non-chat components',
    run() {
      const source = normalizeWhitespace(readSource(COLLECTION_MODELS));
      const usesMainDenylist = source.includes("NON_LLM_LABELS = new Set(['image', 'speech', 'tts', 'audio', 'transcription', 'embeddings', 'embedding', 'reranking'])")
        && /getCollectionPrimaryChatModel[\s\S]*?components\.find[\s\S]*?labels\.some\(\(label\) => NON_LLM_LABELS\.has\(label\)\)[\s\S]*?return explicitLLM \|\| components\[0\]/.test(source);
      const usesCentralPlannerPredicate = /isChatPlannerCandidate/.test(source)
        && /getCollectionPrimaryChatModel[\s\S]*?components\.find\(\(component\) => isChatPlannerCandidate\(modelsData\[component\]\)\)[\s\S]*?return explicitLLM \|\| components\[0\]/.test(source);

      assert.ok(
        usesMainDenylist || usesCentralPlannerPredicate,
        'Primary chat selection should either use the main denylist or the centralized chat-planner predicate.',
      );
    },
  },
  {
    name: 'collection helper exports stay stable for app and tool callers',
    run() {
      const source = readSource(COLLECTION_MODELS);
      for (const name of [
        'NON_LLM_LABELS',
        'getCollectionComponents',
        'isCollectionModel',
        'isModelEffectivelyDownloaded',
        'isModelEffectivelyLoaded',
        'isCollectionFullyDownloaded',
        'isCollectionFullyLoaded',
        'getCollectionImageModel',
        'getCollectionPrimaryChatModel',
      ]) {
        assert.ok(isExported(source, name), `${name} should remain exported.`);
      }
    },
  },
];

module.exports = { tests };
