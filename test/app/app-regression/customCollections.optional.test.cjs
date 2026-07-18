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
const PANEL = 'src/app/src/renderer/components/CustomCollectionPanel.tsx';

function skipIfMissing() {
  if (!hasFile(CUSTOM_COLLECTIONS)) {
    return { skip: true, reason: 'customCollections.ts is not present on this branch' };
  }
  return null;
}

const tests = [
  {
    name: 'custom collection constants use server-side user namespace',
    run() {
      const skip = skipIfMissing();
      if (skip) return skip;
      const source = readSource(CUSTOM_COLLECTIONS);
      assertIncludes(source, 'CUSTOM_COLLECTION_PREFIX = USER_MODEL_PREFIX', 'Custom collections should use the server user.* namespace.');
      assertIncludes(source, 'COLLECTION_OMNI_MODEL_RECIPE', 'Custom collections should use the collection.omni recipe constant.');
    },
  },
  {
    name: 'custom collection component list deduplicates while preserving role order',
    run() {
      const skip = skipIfMissing();
      if (skip) return skip;
      const source = normalizeWhitespace(readSource(CUSTOM_COLLECTIONS));
      assertIncludes(source, 'getCustomCollectionComponentList', 'Component list helper should exist.');
      assertIncludes(source, 'const ordered = [ components.llm, components.vision, components.image, components.edit, components.transcription, components.speech', 'Component lists should preserve semantic role order.');
      assertIncludes(source, 'return Array.from(new Set(ordered))', 'Component lists should deduplicate through Set insertion order.');
    },
  },
  {
    name: 'custom collection export saves the normalized /models/{id} object like regular models',
    run() {
      const skip = skipIfMissing();
      if (skip) return skip;
      const app = normalizeWhitespace(readSource(APP));
      assertIncludes(app, 'buildModelExportFile(request.model_name)', 'Collection export should fetch /models/{id} and save it via the shared export helper.');
      const modelData = normalizeWhitespace(readSource(MODEL_DATA));
      assertIncludes(modelData, 'EXPORT_KNOWN_KEYS', 'The shared export transform should allow-list keys (mirroring the CLI kKnownKeys).');
      assertMatches(modelData, /normalizeModelExportPayload/, 'The export transform should be a pure, testable function.');
      assertMatches(modelData, /filename: `\$\{bareName\}\.json`/, 'Export filenames should not carry the user. prefix.');
    },
  },
  {
    name: 'custom collection import hands the exported file to /pull as a single registration',
    run() {
      const skip = skipIfMissing();
      if (skip) return skip;
      const app = normalizeWhitespace(readSource(APP));
      assertIncludes(app, 'pullModel(requestBody.model_name', 'Omni model registration should use the UI download path so server-owned pulls appear in Download Manager.');
      assertIncludes(app, 'collectionComponents', 'Omni model pulls should pass component names to the Download Manager row.');
      assertMatches(app, /const record = parsed as PullRegistrationPayload[\s\S]*?await pullRegistration\(record\)/, 'Import should POST the whole exported file to /pull; the server registers embedded component definitions.');
    },
  },
  {
    name: 'custom collection panel can edit custom collections and clone built-in collection templates',
    run() {
      const skip = skipIfMissing();
      if (skip) return skip;
      const panel = normalizeWhitespace(readSource(PANEL));
      assertIncludes(panel, 'isTemplateEdit', 'Panel should distinguish built-in templates from editable user collections.');
      assertIncludes(panel, 'Components have changed. Saving re-registers this Omni Model through /pull with recipe collection.omni and the updated components list.', 'Panel should flag modified component drafts without reintroducing the removed checkpoint summary card.');
      assertIncludes(panel, 'autoProviderOption', 'Panel should keep the automatically provided Vision/Edit model available as an explicit top dropdown option.');
      assertIncludes(panel, 'collection-auto-provided-option', 'Panel should style the automatic provider option distinctly.');
      assertIncludes(panel, 'Available locally', 'Panel dropdowns should distinguish downloaded components.');
      assert.ok(!panel.includes('collection-options-summary'), 'Panel should not keep the removed top checkpoint summary card.');
      assert.ok(!panel.includes('Checkpoint: {selectedSourceLabel}'), 'Panel should not show per-component checkpoint source strings in role rows.');
      assertIncludes(panel, 'Registered - will download when pulled', 'Panel dropdowns should distinguish registered components that still need download.');
      assert.ok(!panel.includes('collection-delete-button'), 'Panel should not keep a redundant red delete button in the footer.');
    },
  },
  {
    name: 'custom collection role options include registered concrete compatible models',
    run() {
      const skip = skipIfMissing();
      if (skip) return skip;
      const source = normalizeWhitespace(readSource(CUSTOM_COLLECTIONS));
      assertMatches(
        source,
        /isCollectionEligibleModel[\s\S]*?!info \|\| isCollectionRecipe\(info\.recipe\)[\s\S]*?return false/,
        'Role options must exclude missing models and collections, while allowing registered components that can be pulled later.',
      );
      for (const label of ['vision', 'image', 'edit', 'transcription', 'speech']) {
        assertIncludes(source, label, `Role filtering should mention ${label}.`);
      }
    },
  },
  {
    name: 'custom collection UI events are handled by App and exposed from ModelManager/selector',
    run() {
      const skip = skipIfMissing();
      if (skip) return skip;
      const app = readSource(APP);
      const manager = readSource(MODEL_MANAGER);
      assertIncludes(app, 'openCustomCollection', 'App should listen for custom collection creation/import events.');
      assertIncludes(app, 'editCustomCollection', 'App should listen for custom collection edit events.');
      assertIncludes(manager, 'renderCustomCollectionOptionsButton', 'ModelManager should expose an edit/options action for collections.');
      assertIncludes(manager, 'canDeleteFromRow', 'ModelManager should keep a row delete action for custom collections.');
      const hasDeleteGuard = manager.includes('const canDeleteFromRow = !isCollection || !isBuiltInCollection') ||
        manager.includes('const canDeleteFromRow = !isCollection || isUserCollection');
      assert.ok(hasDeleteGuard, 'Custom collection rows should be deletable while built-in collection rows stay protected.');
      assert.ok(
        manager.includes('isBuiltInCollection') ||
        manager.includes("info?.source === 'user'") ||
        manager.includes("(info?.labels ?? []).includes('custom')"),
        'ModelManager should distinguish built-in collections from user/custom collection rows.',
      );
      assertIncludes(manager, 'canDeleteFromRow && renderDeleteButton(modelName,', 'Custom collection rows should render the row delete action.');
      assertIncludes(manager, 'isCollectionEditableAsCustom', 'ModelManager should expose settings for collection templates and custom collections.');
      if (hasFile(MODEL_SELECTOR)) {
        const selector = readSource(MODEL_SELECTOR);
        assertIncludes(selector, 'isCustomCollectionModel', 'Model selector should mark server-side custom collections intentionally.');
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
