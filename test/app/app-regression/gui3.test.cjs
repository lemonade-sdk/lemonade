const assert = require('node:assert/strict');
const {
  readSource,
  assertIncludes,
  assertMatches,
  normalizeWhitespace,
} = require('./helpers/source.cjs');

const COLLECTION_MODELS = 'src/app/src/features/collections/collectionModels.ts';
const MODEL_MANAGER = 'src/app/src/components/ModelManager.tsx';
const OMNI_TOOLS = 'src/app/src/tools/omniTools.ts';
const TOOL_DEFINITIONS = 'src/app/src/tools/toolDefinitions.json';

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
];

module.exports = { tests };
