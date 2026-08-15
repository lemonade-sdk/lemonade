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
const detailPanelSource = fs.readFileSync(path.join(root, 'src/components/ModelDetailPanel.tsx'), 'utf8');
const styles = fs.readFileSync(path.join(root, 'src/styles/styles.css'), 'utf8');
const huggingFaceSearchPath = path.join(root, 'src/features/models/huggingFaceSearch.ts');

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
assert.match(api, /limit: String\(HUGGING_FACE_SEARCH_LIMIT\)/);
assert.match(api, /filterHuggingFaceSearchResults\(data\)/);

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
assert.doesNotMatch(manager, /expandedRemoteModel|row__detail row__detail--remote|row__expand/,
  'remote registry rows must select the detail pane instead of expanding duplicate inline details');
assert.doesNotMatch(manager, /row__action--download|row__action--hf-link/,
  'remote registry rows must not duplicate download or provider-link actions from the detail pane');
assert.match(styles, /\.hf-detail__gguf-list \{[\s\S]*?display: flex;[\s\S]*?flex-direction: column;/,
  'remote variants must remain one vertical list at every detail-pane width');
assert.match(detailPanelSource, /role="radiogroup"[\s\S]*?onClick=\{\(\) => setSelectedVariantName\(v\.name\)\}/,
  'variant rows must select a quantization instead of starting a download');
assert.match(detailPanelSource, /appearance="primary"[\s\S]*?onClick=\{\(\) => onHfPull\?\.\(hfModel\.id, selectedVariant\.name, hfVariants\.recipe\)\}/,
  'the selected quantization must have one explicit primary download action');
assert.ok(detailPanelSource.indexOf('hf-detail__gguf-primary-action') < detailPanelSource.indexOf('role="radiogroup"'),
  'the primary download action must stay above the variant list so long lists cannot hide it');
assert.match(detailPanelSource, /hfVariants\.mmproj_files\.length > 0[\s\S]*?t\('detail\.hf\.included'\)[\s\S]*?t\('detail\.hf\.visionProjector'\)/,
  'remote details must describe a detected mmproj through localized semantic labels');
assert.doesNotMatch(detailPanelSource, /hfVariants\.suggested_labels\.length > 0[\s\S]*?>Capabilities<\/span>/,
  'provider capability labels must not create a duplicate Capabilities block in remote details');
assert.doesNotMatch(detailPanelSource, /hf-detail__gguf-action/,
  'variant rows must not repeat a Download action for every quantization');

assert.doesNotMatch(capabilitiesSource, /found\.size === 0\) found\.add\('chat'\)/,
  'unknown capability must not silently become Chat');
assert.doesNotMatch(capabilitiesSource, /MULTIMODAL_CHAT_NAME_PATTERNS|capabilityFromName/,
  'classification must never guess from a model or repository name');
assert.doesNotMatch(capabilitiesSource, /registry_source/,
  'an installed model must not be classified by which registry it came from');
assert.match(remoteCapabilitiesSource, /Repository IDs, display names and the user's search query are deliberately\n \* excluded/,
  'remote registry rows must avoid name guessing');
assert.match(listPanelSource, /const identity = identityFromModelInfo\(m\);/,
  'task filtering asks for the identity rather than re-deriving collection rules');

assert.match(nav, /const \[catalogsOpen, setCatalogsOpen\] = useState\(true\)/, 'Online Catalogs must be independently collapsible');
assert.match(nav, /aria-expanded=\{catalogsOpen\}[\s\S]*aria-controls="nav-online-catalogs"[\s\S]*setCatalogsOpen/, 'Online Catalogs must use the standard collapsible section header');
assert.match(nav, /\{catalogsOpen && \([\s\S]*id="nav-online-catalogs"/, 'catalog checkboxes must hide when collapsed');
assert.match(nav, /className="model-nav-rail__section-toggle"[\s\S]*aria-expanded=\{catalogsOpen\}[\s\S]*>\s*<Icon name=\{catalogsOpen \? 'chevron-down' : 'chevron-right'\}[\s\S]*<span>\{t\('nav\.catalogs'\)\}<\/span>/,
  'Online Catalogs must use the same localized collapsible heading treatment as the other filter sections');
assert.match(nav, /className="backends__toggle model-nav-rail__provider-option"/,
  'online catalogs must reuse the Backends checkbox treatment');
assert.match(nav, /type="checkbox"[\s\S]*checked=\{enabled\}[\s\S]*onChange=\{\(\) => onToggleProvider\(provider\.key\)\}/,
  'online catalogs must use explicit checkbox toggles');
assert.match(nav, /const showCount = searchActive && primaryFilter === 'all' && enabled;/,
  'provider counts must only render while that provider is actually participating in an online search');
assert.match(nav, /\{showCount && \([\s\S]*model-nav-rail__nav-count[\s\S]*\{count\}/,
  'each provider must show its own result count, including zero during an active search');
assert.doesNotMatch(nav, /providerCounts\.huggingface \+ providerCounts\.modelscope/,
  'provider counts must not be aggregated on the Online Catalogs heading');
const catalogsLabelIndex = nav.indexOf("t('nav.catalogs')");
assert.ok(nav.indexOf('model-nav-rail__section--backends') < catalogsLabelIndex,
  'Online Catalogs must be below Backends');
assert.ok(catalogsLabelIndex < nav.indexOf('aria-controls="nav-tags"'),
  'Online Catalogs must remain directly before Tags');
assert.doesNotMatch(styles, /model-nav-rail__section--providers\s*\{[\s\S]*border-top:/,
  'Online Catalogs must not add a divider below Backends');
assert.match(styles, /\.model-nav-rail__provider-list \{[\s\S]*padding: 2px var\(--space-2\) 0;/,
  'catalog checkboxes must align horizontally with the task/backend controls');
assert.match(styles, /input\[type="checkbox"\] \{[\s\S]*width: 16px;[\s\S]*height: 16px;[\s\S]*border-radius: 4px;/,
  'catalog and Backends checkboxes must share the global checkbox geometry');
assert.match(manager, /providerCounts=\{providerCounts\}[\s\S]*searchActive=\{searchActive\}/,
  'ModelManager must pass the existing search activity signal to the nav rail');

const { remoteCapabilityEvidence } = loadTypeScriptModule(path.join(root, 'src/remoteModelCapabilities.ts'));
const { capabilityFromModelInfo, modelCapabilityTags } = loadTypeScriptModule(path.join(root, 'src/modelCapabilities.ts'));
const {
  filterHuggingFaceSearchResults,
  HUGGING_FACE_SEARCH_LIMIT,
  detectHuggingFaceBackendFromMetadata,
  isCompatibleHuggingFaceRecipe,
  isCompatibleHuggingFaceVariantResult,
} = loadTypeScriptModule(huggingFaceSearchPath);

const rankedHuggingFaceResults = Array.from({ length: 14 }, (_, index) => ({
  id: `org/model-${index + 1}`,
  pipeline_tag: index === 3 ? 'image-text-to-image' : 'text-generation',
}));
const filteredHuggingFaceResults = filterHuggingFaceSearchResults(rankedHuggingFaceResults);
assert.equal(HUGGING_FACE_SEARCH_LIMIT, 12, 'GUI2 caps Hugging Face search at its top 12 download-ranked results');
assert.equal(filteredHuggingFaceResults.length, 11, 'incompatible pipelines within the top 12 must be removed');
assert.equal(filteredHuggingFaceResults.at(-1).id, 'org/model-12', 'filtering must not refill from lower-ranked results');
assert.ok(
  filteredHuggingFaceResults.every(result => result.pipeline_tag !== 'image-text-to-image'),
  'non-LLM image pipelines must be excluded from model search',
);
assert.equal(isCompatibleHuggingFaceVariantResult(null), false, 'failed variant discovery must use repository fallback detection');
assert.equal(isCompatibleHuggingFaceVariantResult({ recipe: 'llamacpp', variants: [] }), false,
  'repositories without downloadable variants must use repository fallback detection');
assert.equal(isCompatibleHuggingFaceVariantResult({ recipe: 'llamacpp', variants: [{}] }), true,
  'downloadable llama.cpp repositories must remain visible');
for (const recipe of ['sd-cpp', 'whispercpp', 'moonshine']) {
  assert.equal(isCompatibleHuggingFaceVariantResult({ recipe, variants: [{}] }), false,
    `${recipe} repositories must match GUI2's backend compatibility exclusions`);
}
assert.equal(detectHuggingFaceBackendFromMetadata('FastFlowLM/model', { siblings: [] }), 'flm',
  'empty variant discovery must recover supported FastFlowLM repositories');
assert.equal(detectHuggingFaceBackendFromMetadata('org/model', { siblings: [{ rfilename: 'model.onnx' }] }), 'ryzenai-llm',
  'empty variant discovery must recover supported ONNX repositories');
assert.equal(detectHuggingFaceBackendFromMetadata('org/moonshine-base', { siblings: [{ rfilename: 'model.onnx' }] }), 'moonshine',
  'Moonshine detection must precede generic ONNX detection');
assert.equal(detectHuggingFaceBackendFromMetadata('org/whisper', { tags: ['whisper'], siblings: [{ rfilename: 'model.bin' }] }), 'whispercpp',
  'Whisper repositories must be identified from repository files');
assert.equal(detectHuggingFaceBackendFromMetadata('org/flux-model', { siblings: [] }), 'sd-cpp',
  'image repositories must be identified even without a pipeline tag');
assert.equal(detectHuggingFaceBackendFromMetadata('org/unknown', { siblings: [{ rfilename: 'weights.bin' }] }), null,
  'unknown repositories must resolve as incompatible');
assert.equal(isCompatibleHuggingFaceRecipe('flm'), true, 'supported fallback recipes must remain visible');
assert.equal(isCompatibleHuggingFaceRecipe('ryzenai-llm'), true, 'supported ONNX fallback recipes must remain visible');
assert.equal(isCompatibleHuggingFaceRecipe(null), false, 'unknown fallback results must be removed');
assert.match(manager, /detectHuggingFaceBackend\(candidate\.id, ac\.signal\)/,
  'failed or empty variants must fall through to repository backend detection');
assert.match(manager, /isCompatibleHuggingFaceVariantResult\(variants\)/,
  'Hugging Face results must apply backend compatibility after enrichment');

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
