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
    name: 'custom collection registration uses the PR 1842 server contract',
    run() {
      const skip = skipIfMissing();
      if (skip) return skip;
      const source = readSource(CUSTOM_COLLECTIONS);
      assertIncludes(source, "CUSTOM_COLLECTION_PREFIX = USER_MODEL_PREFIX", 'Custom collection ids should use server user.* ids.');
      assertIncludes(source, 'COLLECTION_OMNI_MODEL_RECIPE', 'Custom collection registration should use recipe=collection.omni.');
      assertIncludes(source, 'model_name:', 'Custom collection registration should build a /pull model_name payload.');
      assertIncludes(source, 'components:', 'Custom collection registration should send components to /pull.');
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
        /getCustomCollectionComponentList[\s\S]*?Array\.from\(new Set\(\[[\s\S]*?components\.llm[\s\S]*?components\.vision[\s\S]*?components\.image[\s\S]*?components\.edit[\s\S]*?components\.transcription[\s\S]*?components\.speech[\s\S]*?\]\.filter/,
        'Component lists should preserve semantic role order and remove duplicates through Set insertion order.',
      );
    },
  },
  {
    name: 'custom collection import validates stale collections before server registration',
    run() {
      const skip = skipIfMissing();
      if (skip) return skip;
      const source = normalizeWhitespace(readSource(CUSTOM_COLLECTIONS));
      assertMatches(
        source,
        /normalizeCustomCollection[\s\S]*?componentList\.every\(\(component\) => !!modelsData\[component\]\)[\s\S]*?return null/,
        'Imported collections should be rejected until every referenced component exists.',
      );
    },
  },
  {
    name: 'custom collection metadata is server-shaped',
    run() {
      const skip = skipIfMissing();
      if (skip) return skip;
      const source = normalizeWhitespace(readSource(CUSTOM_COLLECTIONS));
      assertMatches(source, /recipe: COLLECTION_OMNI_MODEL_RECIPE/, 'Custom collections should use recipe=collection.omni.');
      assertMatches(source, /const components = getCustomCollectionComponentList/, 'Custom collection pull payloads should keep server components.');
      assert.ok(!/collection_components|collection_source|composite_models/.test(source), 'Custom collection helpers should not recreate pre-1842 synthetic metadata.');
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
        /isCollectionEligibleModel[\s\S]*?!info \|\| isCollectionRecipe\(info\.recipe\) \|\| info\.downloaded !== true[\s\S]*?return false/,
        'Role options must exclude missing, server collection, and not-downloaded models.',
      );
      for (const label of ['vision', 'image', 'edit', 'transcription', 'speech']) {
        assertIncludes(source, label, `Role filtering should mention ${label}.`);
      }
    },
  },
  {
    name: 'custom collection refresh uses server model refresh path',
    run() {
      const skip = skipIfMissing();
      if (skip) return skip;
      const app = readSource(APP);
      const modelData = readSource(MODEL_DATA);
      assertIncludes(app, "serverFetch('/pull'", 'Saving collections should register them through /pull.');
      assertIncludes(app, "window.dispatchEvent(new CustomEvent('modelsUpdated'))", 'Saving collections should trigger the shared server-model refresh event.');
      assertIncludes(modelData, "serverFetch('/models?show_all=true')", 'Model data should use the server as the source of truth after PR 1842.');
      assert.ok(!modelData.includes('mergeCustomCollectionsIntoModelsData'), 'Model data should not inject local synthetic collections after PR 1842.');
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
        assertIncludes(selector, 'isCustomCollectionModel', 'Model selector should display custom collections intentionally.');
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
