const assert = require('node:assert/strict');
const path = require('node:path');
const {
  repoRoot,
  readSource,
  assertIncludes,
  assertMatches,
  normalizeWhitespace,
} = require('./helpers/source.cjs');

const CHAT_MODEL_OVERRIDE = 'src/app/src/features/chatModelOverride.ts';
const CHAT_VIEW = 'src/app/src/components/ChatView.tsx';
const COLLECTION_MODELS = 'src/app/src/features/collections/collectionModels.ts';
const MODEL_MANAGER = 'src/app/src/components/ModelManager.tsx';
const OMNI_TOOLS = 'src/app/src/tools/omniTools.ts';
const TOOL_DEFINITIONS = 'src/app/src/tools/toolDefinitions.json';

// chatModelOverride.ts is dependency-free so it can be transpiled and exercised
// directly, without standing up React or the rest of ChatView.
function loadChatModelOverride() {
  let ts = null;
  try { ts = require(path.join(repoRoot, 'src', 'app', 'node_modules', 'typescript')); }
  catch (_) { try { ts = require('typescript'); } catch (_2) { return null; } }

  const compiled = ts.transpileModule(readSource(CHAT_MODEL_OVERRIDE), {
    compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS },
    fileName: CHAT_MODEL_OVERRIDE,
    reportDiagnostics: true,
  });
  assert.equal(
    (compiled.diagnostics || []).length,
    0,
    (compiled.diagnostics || []).map(d => ts.flattenDiagnosticMessageText(d.messageText, '\n')).join('\n'),
  );
  const loaded = { exports: {} };
  Function('exports', 'module', compiled.outputText)(loaded.exports, loaded);
  return loaded.exports;
}

const tests = [
  {
    name: 'collection helpers preserve component order and collection identity',
    run() {
      const source = normalizeWhitespace(readSource(COLLECTION_MODELS));
      assertMatches(
        source,
        /const candidates = \[[\s\S]*?model as any\)\.components[\s\S]*?model as any\)\.component_models[\s\S]*?model as any\)\.recipe_options\?\.components/,
        'Collection components should accept the GUI3 model shapes.',
      );
      assertIncludes(
        source,
        'Array.from(new Set(raw.filter',
        'Collection components should be deduplicated without changing their declared order.',
      );
      assertMatches(
        source,
        /isCollectionModel[\s\S]*?isCollectionRecipe\(\(model as any\)\.recipe\)[\s\S]*?getCollectionComponents\(model\)\.length > 0/,
        'A collection model should require the collection recipe and at least one component.',
      );
    },
  },
  {
    name: 'collection loading state requires every concrete component',
    run() {
      const source = normalizeWhitespace(readSource(COLLECTION_MODELS));
      assertMatches(
        source,
        /isCollectionFullyDownloaded[\s\S]*?components\.every[\s\S]*?downloaded === true/,
        'A collection should be downloaded only when every component is downloaded.',
      );
      assertMatches(
        source,
        /isCollectionFullyLoaded[\s\S]*?components\.every[\s\S]*?loaded\.has\(component\.toLowerCase\(\)\)/,
        'A collection should be loaded only when every component is loaded.',
      );
    },
  },
  {
    name: 'ModelScope results are validated through bounded variant lookups',
    run() {
      const source = normalizeWhitespace(readSource(MODEL_MANAGER));
      assertIncludes(source, 'searchModelScope(q, ac.signal)', 'GUI3 should use the ModelScope search API.');
      assertIncludes(
        source,
        "loadRemoteVariants('modelscope', candidate.id, ac.signal)",
        'ModelScope results should be checked for usable variants before display.',
      );
      assertMatches(
        source,
        /Promise\.all\(Array\.from\(\{ length: Math\.min\(REMOTE_VARIANT_CONCURRENCY, candidates\.length\)/,
        'Remote variant validation should remain concurrency-bounded.',
      );
    },
  },
  {
    name: 'GUI3 Omni definitions keep planner guidance and required media tools',
    run() {
      const definitions = JSON.parse(readSource(TOOL_DEFINITIONS));
      const names = definitions.tools.map((tool) => tool.function.name).sort();
      assert.deepEqual(names, ['edit_image', 'generate_image', 'text_to_speech']);
      assertIncludes(definitions.system_prompt, '{tool_list}', 'The Omni planner prompt should reserve the tool list slot.');
      assertIncludes(definitions.system_prompt, '{tool_guidance}', 'The Omni planner prompt should reserve the guidance slot.');

      const source = normalizeWhitespace(readSource(OMNI_TOOLS));
      assertIncludes(source, 'DEFAULT_OMNI_SYSTEM_PROMPT_TEMPLATE', 'GUI3 should keep a shared Omni prompt template.');
      assertIncludes(source, 'renderOmniSystemPrompt', 'GUI3 should render the planner prompt from available tools.');
      assertIncludes(source, 'resolveExplicitImageSize', 'GUI3 should support explicit image dimensions.');
    },
  },
  {
    name: 'GUI3 ModelManager loads collection components while keeping the collection virtual',
    run() {
      const source = normalizeWhitespace(readSource(MODEL_MANAGER));
      assertIncludes(
        source,
        'const components = info && isCollectionModel(info) ? getCollectionComponents(info) : []',
        'ModelManager should identify collection components before loading a model.',
      );
      assertMatches(
        source,
        /if \(components\.length > 0\)[\s\S]*?for \(const componentName of components\)[\s\S]*?loadModelRuntime\(componentInfo \|\| componentName, visited\)/,
        'Loading a collection should recursively load its concrete components.',
      );
      assertIncludes(
        source,
        'selected virtual model in the UI',
        'The collection should remain a virtual selection after its components load.',
      );
    },
  },
  {
    // An override that outlived its conversation used to win over a model that was
    // loaded right then, so the next prompt reloaded a model the user had already
    // deleted their way out of.
    name: 'GUI3 chat model override never outlives the conversation that set it',
    run() {
      const overrides = loadChatModelOverride();
      if (!overrides) return { skip: true, reason: "typescript not installed - run 'npm ci' in src/app first" };
      const { activateConversationModel, releaseConversationModel } = overrides;

      const opened = activateConversationModel(null, {
        model: 'Unloaded-7B',
        currentModel: 'Resident-4B',
        loaded: false,
      });
      assert.deepEqual(
        opened,
        { override: 'Unloaded-7B', select: null },
        'Opening a conversation whose model is not resident should pin it without selecting it.',
      );

      assert.equal(
        releaseConversationModel(opened.override, true),
        null,
        'Deleting the active conversation must drop its model affinity.',
      );
      assert.equal(
        releaseConversationModel(opened.override, false),
        'Unloaded-7B',
        'Deleting a background conversation must not disturb the active one.',
      );
    },
  },
  {
    name: 'GUI3 chat model override defers to resident models and idempotent activation',
    run() {
      const overrides = loadChatModelOverride();
      if (!overrides) return { skip: true, reason: "typescript not installed - run 'npm ci' in src/app first" };
      const { activateConversationModel } = overrides;

      assert.deepEqual(
        activateConversationModel('Stale-7B', {
          model: 'Resident-4B',
          currentModel: 'Other-3B',
          loaded: true,
        }),
        { override: null, select: 'Resident-4B' },
        'A resident conversation model should go through the real selection, not the override.',
      );

      assert.deepEqual(
        activateConversationModel('Unloaded-7B', {
          model: 'Unloaded-7B',
          currentModel: 'Unloaded-7B',
          loaded: false,
        }),
        { override: 'Unloaded-7B', select: null },
        'Re-activating the conversation already in view should not clear its own override.',
      );

      assert.deepEqual(
        activateConversationModel('Unloaded-7B', {
          model: undefined,
          currentModel: 'Unloaded-7B',
          loaded: false,
        }),
        { override: 'Unloaded-7B', select: null },
        'A conversation with no recorded model should leave the selection alone.',
      );
    },
  },
  {
    name: 'GUI3 ChatView clears the chat model override on every conversation exit',
    run() {
      const source = normalizeWhitespace(readSource(CHAT_VIEW));
      assertMatches(
        source,
        /handleDeleteConversation = useCallback[\s\S]{0,400}?setFallbackModelOverride\(prev => releaseConversationModel\(prev, wasActive\)\)/,
        'Deleting a conversation must release the override for the active conversation only.',
      );
      assertMatches(
        source,
        /handleNewChat = useCallback[\s\S]{0,200}?setFallbackModelOverride\(null\)/,
        'Starting a new chat must clear the override.',
      );
      assertMatches(
        source,
        /!conversations\.some\(c => c\.id === activeId\)\) \{ setActiveId\(null\); setFallbackModelOverride\(null\)/,
        'Dropping a stale active conversation must clear the override.',
      );
      assertMatches(
        source,
        /handleEmptyStateModelSelect = useCallback[\s\S]{0,200}?setFallbackModelOverride\(null\)/,
        'An explicit model pick must clear the override.',
      );
    },
  },
];

module.exports = { tests };
