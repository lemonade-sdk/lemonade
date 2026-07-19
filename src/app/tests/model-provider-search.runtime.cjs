const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const manager = fs.readFileSync(path.join(root, 'src/components/ModelManager.tsx'), 'utf8');
const nav = fs.readFileSync(path.join(root, 'src/components/ModelNavRail.tsx'), 'utf8');
const api = fs.readFileSync(path.join(root, 'src/api.ts'), 'utf8');
const capabilitiesSource = fs.readFileSync(path.join(root, 'src/modelCapabilities.ts'), 'utf8');
const remoteCapabilitiesSource = fs.readFileSync(path.join(root, 'src/remoteModelCapabilities.ts'), 'utf8');
const listPanelSource = fs.readFileSync(path.join(root, 'src/components/ModelListPanel.tsx'), 'utf8');

function loadTypeScriptModule(filename) {
  const source = fs.readFileSync(filename, 'utf8');
  const compiled = ts.transpileModule(source, {
    compilerOptions: {
      target: ts.ScriptTarget.ES2020,
      module: ts.ModuleKind.CommonJS,
      esModuleInterop: true,
    },
    fileName: filename,
    reportDiagnostics: true,
  });
  const diagnostics = compiled.diagnostics || [];
  assert.equal(diagnostics.length, 0, diagnostics.map(d => ts.flattenDiagnosticMessageText(d.messageText, '\n')).join('\n'));
  const module = { exports: {} };
  Function('exports', 'require', 'module', '__filename', '__dirname', compiled.outputText)(
    module.exports,
    require,
    module,
    filename,
    path.dirname(filename),
  );
  return module.exports;
}

assert.match(api, /registrySearch\(\s*source: ModelRegistryProvider/);
assert.match(api, /format: 'gguf'/);
assert.match(api, /searchModelScope/);
assert.match(api, /source: 'modelscope'/);

// User models and Omni collections must be server-backed, not only browser
// localStorage entries. This keeps them visible through /models after restart.
assert.match(api, /async registerModelDefinition\(modelName: string/,
  'API client must expose synchronous model-definition registration');
assert.match(api, /stream: false[\s\S]*subscribe: false[\s\S]*do_not_upgrade: true/,
  'definition registration must use the non-streaming persistent /pull path');
assert.match(api, /name\.startsWith\('user\.'\)[\s\S]*labels\.includes\('custom'\)/,
  'server-returned user models must be normalized as custom models');
assert.match(manager, /await api\.registerModelDefinition\(saved\.name/,
  'saved Omni collections must be registered with lemond');
assert.match(manager, /persistedModels\.data\.some/,
  'collection save must verify that /models exposes the registered definition');
assert.match(listPanelSource, /name\.startsWith\('user\.'\)[\s\S]*labels\.includes\('custom'\)/,
  'My Models must include custom definitions restored from lemond');

// Server selection remains the prototype's existing contract; ModelScope uses
// Lemonade's registry endpoint without a browser-side fallback.
assert.doesNotMatch(api, /modelscope\.cn\/openapi/,
  'the GUI must not bypass lemond with a direct ModelScope search');


assert.match(manager, /\}, \[searchQuery, providerEnabled\.huggingface\]\);/,
  'Hugging Face search must only depend on query/provider state');
assert.match(manager, /\}, \[searchQuery, providerEnabled\.modelscope\]\);/,
  'ModelScope search must only depend on query/provider state');
assert.ok(manager.includes('REMOTE_SEARCH_CACHE'), 'search results must be cached');
assert.ok(manager.includes('REMOTE_VARIANT_CACHE'), 'variant/capability metadata must be cached');
assert.match(manager, /remoteVariantCheckpoint\(modelId, variantName, recipe\)/,
  'remote pulls must keep the selected GGUF variant in the registration checkpoint');
assert.match(manager, /String\(recipe \|\| ''\)\.toLowerCase\(\) !== 'llamacpp' \|\| !variantName/,
  'only llama.cpp registrations should receive a variant suffix');
assert.match(manager, /const fallbackBase = safeOwner \? `\$\{defaultName\}-\$\{safeOwner\}` : defaultName/,
  'colliding remote model names must fall back to the repository owner');
assert.match(manager, /existingCandidate && !matchesCheckpoint\(existingCandidate\)/,
  'collision suffixes must remain idempotent for the same checkpoint');
assert.match(manager, /active\.downloadId \|\| `model:\$\{active\.modelName\}`/,
  'remote cancellation must address the exact resolved download id');
assert.ok(remoteCapabilitiesSource.includes('remoteCapabilityEvidence'), 'remote capabilities must use structured evidence');
assert.ok(remoteCapabilitiesSource.includes('tokenizer.chat_template'), 'GGUF chat-template metadata must be consumed when provided');
assert.ok(remoteCapabilitiesSource.includes('pooling_type'), 'GGUF pooling metadata must be consumed when provided');
assert.ok(remoteCapabilitiesSource.includes('hasUsableGguf'), 'validated GGUF structure must be available as repository evidence');
assert.doesNotMatch(remoteCapabilitiesSource, /pipeline\.includes\(/,
  'remote capability matching must not be broad substring guessing');
assert.ok(manager.indexOf("renderProviderZone('huggingface')") < manager.indexOf("renderProviderZone('modelscope')"),
  'ModelScope results must be rendered below Hugging Face');

assert.doesNotMatch(capabilitiesSource, /found\.size === 0\) found\.add\('chat'\)/,
  'unknown capability must not silently become Chat');
assert.match(capabilitiesSource, /Remote search rows must not become Chat/,
  'remote registry rows must avoid name/recipe guessing');
assert.match(listPanelSource, /if \(filter === 'llm'\) return cap === 'chat';/,
  'Unknown capability rows must not leak into the LLM capability filter');
assert.match(listPanelSource, /return cap === 'omni' \|\| recipe === 'collection\.omni'/,
  'single-repository vision-language models must match the Omni filter');

assert.match(nav, />Model-Provider</);
assert.match(nav, /name=\{enabled \? 'cloud' : 'cloud-off'\}/);
assert.match(nav, /providerCounts\.huggingface \+ providerCounts\.modelscope/,
  'collapsed provider section must show the combined result count');
assert.match(nav, /aria-pressed=\{enabled\}/);

const { remoteCapabilityEvidence } = loadTypeScriptModule(path.join(root, 'src/remoteModelCapabilities.ts'));
const { capabilityFromModelInfo, modelCapabilityTags } = loadTypeScriptModule(path.join(root, 'src/modelCapabilities.ts'));

const unknownByNameOnly = remoteCapabilityEvidence({
  id: 'org/embedding-chat-reranker-name-only',
  modelId: 'org/embedding-chat-reranker-name-only',
  likes: 0,
  downloads: 0,
  tags: [],
  source: 'modelscope',
}, {
  checkpoint: 'org/embedding-chat-reranker-name-only',
  recipe: 'llamacpp',
  repo_kind: 'gguf',
  suggested_name: 'embedding-chat-reranker-name-only',
  suggested_labels: [],
  mmproj_files: [],
  variants: [],
});
assert.equal(unknownByNameOnly.primary, 'unknown', 'repository names must not determine capability');

const ggufOmni = remoteCapabilityEvidence({
  id: 'org/model', modelId: 'org/model', likes: 0, downloads: 0, tags: [], source: 'modelscope',
}, {
  checkpoint: 'org/model', recipe: 'llamacpp', repo_kind: 'gguf', suggested_name: 'model',
  suggested_labels: [], mmproj_files: ['mmproj-f16.gguf'], variants: [],
  gguf_metadata: { tokenizer: { chat_template: '{{ messages }}' } },
});
assert.deepEqual({ primary: ggufOmni.primary, confidence: ggufOmni.confidence }, { primary: 'omni', confidence: 'repository' });
assert.ok(ggufOmni.labels.includes('vision'));

const mmprojOmniWithoutMetadata = remoteCapabilityEvidence({
  id: 'org/model', modelId: 'org/model', likes: 0, downloads: 0, tags: [], source: 'modelscope',
}, {
  checkpoint: 'org/model', recipe: 'llamacpp', repo_kind: 'gguf', suggested_name: 'model',
  suggested_labels: [], mmproj_files: ['mmproj-f16.gguf'],
  variants: [{ name: 'Q4_K_M', primary_file: 'model-Q4_K_M.gguf', files: ['model-Q4_K_M.gguf'], sharded: false, size_bytes: 1 }],
});
assert.deepEqual(
  { primary: mmprojOmniWithoutMetadata.primary, confidence: mmprojOmniWithoutMetadata.confidence },
  { primary: 'omni', confidence: 'repository' },
  'validated GGUF + mmproj structure is repository evidence for Omni',
);

const ggufEmbedding = remoteCapabilityEvidence({
  id: 'org/model', modelId: 'org/model', likes: 0, downloads: 0, tags: [], source: 'huggingface',
}, {
  checkpoint: 'org/model', recipe: 'llamacpp', repo_kind: 'gguf', suggested_name: 'model',
  suggested_labels: [], mmproj_files: [], variants: [],
  gguf_metadata: { bert: { pooling_type: 2 } },
});
assert.deepEqual({ primary: ggufEmbedding.primary, confidence: ggufEmbedding.confidence }, { primary: 'embedding', confidence: 'repository' });

const providerReranker = remoteCapabilityEvidence({
  id: 'org/model', modelId: 'org/model', likes: 0, downloads: 0, tags: [],
  pipeline_tag: 'text-ranking', source: 'modelscope',
});
assert.deepEqual({ primary: providerReranker.primary, confidence: providerReranker.confidence }, { primary: 'reranking', confidence: 'provider' });

const providerVisionChat = remoteCapabilityEvidence({
  id: 'unsloth/Qwen3.6-35B-A3B-GGUF', modelId: 'unsloth/Qwen3.6-35B-A3B-GGUF',
  likes: 0, downloads: 0, tags: ['Image-Text-to-Text', 'GGUF'], source: 'modelscope',
});
assert.deepEqual(
  { primary: providerVisionChat.primary, confidence: providerVisionChat.confidence },
  { primary: 'omni', confidence: 'provider' },
  'exact provider task tags may provide capability evidence without name guessing',
);
assert.ok(providerVisionChat.labels.includes('vision'));

const remoteUnknown = {
  id: 'org/embedding-name-only', name: 'org/embedding-name-only', recipe: 'llamacpp',
  source: 'modelscope', registry_source: 'modelscope', labels: [],
};
assert.equal(capabilityFromModelInfo(remoteUnknown), 'unknown');
assert.deepEqual(modelCapabilityTags(remoteUnknown), []);
assert.equal(capabilityFromModelInfo({ id: 'local-model', recipe: 'llamacpp', labels: [] }), 'chat',
  'local recipe behavior must remain unchanged');

console.log('Model provider search and capability contract checks passed.');
