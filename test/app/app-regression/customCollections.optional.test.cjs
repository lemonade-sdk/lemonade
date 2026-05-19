const assert = require('node:assert/strict');
const {
  hasFile,
  readSource,
  normalizeWhitespace,
  assertIncludes,
  assertMatches,
  countMatches,
} = require('./helpers/source.cjs');

const CUSTOM_COLLECTIONS = 'src/app/src/renderer/utils/customCollections.ts';
const MODEL_DATA = 'src/app/src/renderer/utils/modelData.ts';
const MODEL_MANAGER = 'src/app/src/renderer/ModelManager.tsx';
const MODEL_SELECTOR = 'src/app/src/renderer/components/ModelSelector.tsx';
const APP = 'src/app/src/renderer/App.tsx';

function skipIfMissing() {
  if (!hasFile(CUSTOM_COLLECTIONS)) {
    return { skip: true, reason: 'customCollections.ts is not present on this branch' };
  }
  return null;
}

const tests = [
  {
    name: 'custom collection storage constants use collection terminology and localStorage versioning',
    run() {
      const skip = skipIfMissing();
      if (skip) return skip;
      const source = readSource(CUSTOM_COLLECTIONS);
      assertIncludes(source, "CUSTOM_COLLECTION_PREFIX = 'collection.'", 'Custom collection ids should use collection.* ids.');
      assertIncludes(source, "CUSTOM_COLLECTIONS_STORAGE_KEY = 'lemonade.customCollections.v1'", 'Storage key should be versioned.');
      assertIncludes(source, 'CUSTOM_COLLECTIONS_EXPORT_VERSION = 1', 'Export format should be versioned.');
    },
  },
  {
    name: 'custom collection component list deduplicates while preserving role order',
    run() {
      const skip = skipIfMissing();
      if (skip) return skip;
      const source = normalizeWhitespace(readSource(CUSTOM_COLLECTIONS));
      assertMatches(
        source,
        /getCustomCollectionComponentList[\s\S]*?const ordered = \[[\s\S]*?llm[\s\S]*?vision[\s\S]*?image[\s\S]*?edit[\s\S]*?transcription[\s\S]*?speech[\s\S]*?return Array\.from\(new Set\(ordered\)\)/,
        'Component lists should preserve semantic role order and remove duplicates through Set insertion order.',
      );
    },
  },
  {
    name: 'custom collection merge hides stale collections until every component is present',
    run() {
      const skip = skipIfMissing();
      if (skip) return skip;
      const source = normalizeWhitespace(readSource(CUSTOM_COLLECTIONS));
      assertMatches(
        source,
        /mergeCustomCollectionsIntoModelsData[\s\S]*?components\.every\(\(component\) => merged\[component\]\)[\s\S]*?continue[\s\S]*?merged\[collection\.id\] = customCollectionToModelInfo/,
        'Synthetic collection models should only be inserted when all referenced components exist.',
      );
    },
  },
  {
    name: 'custom collection synthetic model metadata remains collection-shaped',
    run() {
      const skip = skipIfMissing();
      if (skip) return skip;
      const source = normalizeWhitespace(readSource(CUSTOM_COLLECTIONS));
      assertMatches(source, /recipe: 'collection'/, 'Synthetic custom collections should use recipe=collection.');
      assertMatches(source, /source: 'custom-collection'/, 'Synthetic custom collections should retain a source marker.');
      assertMatches(source, /collection_source: 'custom'/, 'Synthetic custom collections should retain collection_source=custom.');
      assertMatches(source, /collection_components: collection\.components/, 'Synthetic metadata should keep original role assignments.');
    },
  },
  {
    name: 'custom collection role options include only downloaded concrete compatible models',
    run() {
      const skip = skipIfMissing();
      if (skip) return skip;
      const source = normalizeWhitespace(readSource(CUSTOM_COLLECTIONS));
      assertMatches(
        source,
        /isCollectionEligibleModel[\s\S]*?!info \|\| isCustomCollectionId\(modelId\) \|\| info\.recipe === 'collection' \|\| info\.downloaded !== true[\s\S]*?return false/,
        'Role options must exclude missing, synthetic collection, and not-downloaded models.',
      );
      for (const label of ['vision', 'image', 'edit', 'transcription', 'speech']) {
        assertIncludes(source, label, `Role filtering should mention ${label}.`);
      }
    },
  },
  {
    name: 'custom collection refresh event is wired into model data refresh',
    run() {
      const skip = skipIfMissing();
      if (skip) return skip;
      const customSource = readSource(CUSTOM_COLLECTIONS);
      const modelData = readSource(MODEL_DATA);
      assertIncludes(customSource, "window.dispatchEvent(new CustomEvent('customCollectionsUpdated'))", 'Saving collections should dispatch refresh event.');
      assertIncludes(modelData, 'mergeCustomCollectionsIntoModelsData', 'Model data should merge custom collections into server models.');
    },
  },
  {
    name: 'custom collection UI events are handled by App and exposed from ModelManager/selector',
    run() {
      const skip = skipIfMissing();
      if (skip) return skip;
      const app = readSource(APP);
      const manager = readSource(MODEL_MANAGER);
      assertIncludes(app, "openCustomCollection", 'App should listen for custom collection creation/import events.');
      assertIncludes(app, "editCustomCollection", 'App should listen for custom collection edit events.');
      assertIncludes(manager, 'renderCustomCollectionOptionsButton', 'ModelManager should expose an edit/options action for custom collections.');
      if (hasFile(MODEL_SELECTOR)) {
        const selector = readSource(MODEL_SELECTOR);
        assertIncludes(selector, 'CUSTOM_COLLECTION_PREFIX', 'Model selector should display custom collections intentionally.');
      }
    },
  },
  {
    name: 'custom collections do not reintroduce workflow terminology in new source files',
    run() {
      const skip = skipIfMissing();
      if (skip) return skip;
      const source = readSource(CUSTOM_COLLECTIONS);
      assert.equal(countMatches(source, /workflow/gi), 0, 'customCollections.ts should not use workflow terminology.');
    },
  },
];

module.exports = { tests };
