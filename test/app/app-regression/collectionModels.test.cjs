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
    name: 'collection components preserve declared component order and ignore invalid entries',
    run() {
      const source = normalizeWhitespace(readSource(COLLECTION_MODELS));
      assertMatches(
        source,
        /getCollectionComponents[\s\S]*?const components = info\?\.components[\s\S]*?Array\.isArray\(components\)[\s\S]*?components\.filter\(/,
        'Collection components should only come from array-valued components.',
      );
      assertIncludes(
        source,
        "typeof name === 'string' && name.length > 0",
        'Collection components should be filtered to non-empty strings.',
      );
      const componentBody = source.slice(source.indexOf('getCollectionComponents'), source.indexOf('isCollectionModel'));
      assert.ok(!/\.sort\(/.test(componentBody), 'Component order is semantic and should not be sorted.');
    },
  },
  {
    name: 'collection identity depends on collection.omni recipe helper and at least one component',
    run() {
      const source = normalizeWhitespace(readSource(COLLECTION_MODELS));
      assertMatches(
        source,
        /isCollectionModel[\s\S]*?isCollectionRecipe\(info\.recipe\)[\s\S]*?getCollectionComponents\(info\)\.length > 0/,
        'A collection model should require the collection recipe helper and at least one concrete component.',
      );
    },
  },
  {
    name: 'collection downloaded and loaded state require every component',
    run() {
      const source = normalizeWhitespace(readSource(COLLECTION_MODELS));
      assertMatches(
        source,
        /isCollectionFullyDownloaded[\s\S]*?components\.every\(\(component\) => modelsData\[component\]\?\.downloaded === true\)/,
        'A collection should be downloaded only when every component is downloaded.',
      );
      assertMatches(
        source,
        /isCollectionFullyLoaded[\s\S]*?components\.every\(\(component\) => loadedModels\.has\(component\)\)/,
        'A collection should be loaded only when every component is loaded.',
      );
    },
  },
  {
    name: 'collection image routing uses the image-capable component',
    run() {
      const source = normalizeWhitespace(readSource(COLLECTION_MODELS));
      assertMatches(
        source,
        /getCollectionImageModel[\s\S]*?components\.find[\s\S]*?labels\?\.includes\('image'\)[\s\S]*?imageModel \|\| null/,
        'Image tool routing should pick the image-labeled component and otherwise return null.',
      );
    },
  },
  {
    name: 'primary chat routing uses the shared planner predicate before fallback',
    run() {
      const source = normalizeWhitespace(readSource(COLLECTION_MODELS));
      assertMatches(
        source,
        /getCollectionPrimaryChatModel[\s\S]*?components\.find\(\(component\) => isChatPlannerCandidate\(modelsData\[component\]\)\)[\s\S]*?return explicitLLM \|\| components\[0\]/,
        'Primary chat selection should use the shared planner predicate before falling back to the first component.',
      );
    },
  },
  {
    name: 'collection helper public surface stays available to app callers',
    run() {
      const source = readSource(COLLECTION_MODELS);
      for (const name of [
        'getCollectionComponents',
        'isCollectionModel',
        'isModelEffectivelyDownloaded',
        'isModelEffectivelyLoaded',
        'getCollectionImageModel',
        'getCollectionPrimaryChatModel',
      ]) {
        assert.ok(isExported(source, name), `${name} should remain exported.`);
      }
    },
  },
];

module.exports = { tests };
