const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');

const repoRoot = path.resolve(__dirname, '../../..');
const modelManager = fs.readFileSync(
  path.join(repoRoot, 'src/app/src/renderer/ModelManager.tsx'),
  'utf8',
);
const registrySource = fs.readFileSync(
  path.join(repoRoot, 'src/cpp/server/model_registry.cpp'),
  'utf8',
);
const serverSource = fs.readFileSync(
  path.join(repoRoot, 'src/cpp/server/server.cpp'),
  'utf8',
);

module.exports.tests = [
  {
    name: 'ModelScope search is rendered after Hugging Face with explicit source badges',
    run() {
      const hfHeader = modelManager.indexOf('FROM HUGGING FACE');
      const msHeader = modelManager.indexOf('FROM MODELSCOPE');
      assert.ok(hfHeader >= 0 && msHeader > hfHeader);
      assert.match(modelManager, />HF<\/span>/);
      assert.match(modelManager, />MS<\/span>/);
    },
  },
  {
    name: 'ModelScope uses the canonical server search route and source-aware variants',
    run() {
      assert.match(
        modelManager,
        /\/registry\/search\?source=modelscope&query=\$\{encodeURIComponent\(normalizedQuery\)\}&limit=\$\{MODELSCOPE_SEARCH_LIMIT\}&format=gguf/,
      );
      assert.match(modelManager, /pull\/variants\?source=modelscope&checkpoint=/);
      assert.doesNotMatch(modelManager, /operation=registry-search/);
      assert.match(serverSource, /register_get\("registry\/search"/);
    },
  },
  {
    name: 'ModelScope results are file-validated incrementally with bounded concurrency',
    run() {
      assert.match(modelManager, /const maxResults = MODELSCOPE_MAX_RESULTS/);
      assert.match(modelManager, /\.slice\(0, MODELSCOPE_SEARCH_LIMIT\)/);
      assert.match(modelManager, /Math\.min\(MODELSCOPE_VALIDATION_CONCURRENCY, candidates\.length\)/);
      assert.match(modelManager, /publishValidated\(\)/);
      assert.match(modelManager, /modelScopeVariantCacheRef/);
      assert.match(modelManager, /\{ signal \}/);
      assert.match(modelManager, /\}, MODELSCOPE_SEARCH_DEBOUNCE_MS\);/);
      assert.match(modelManager, /Publish immediately instead of waiting/);
      assert.doesNotMatch(modelManager, />Unavailable</);
    },
  },
  {
    name: 'ModelScope candidate search avoids the previous third upstream request',
    run() {
      const functionStart = registrySource.indexOf('RegistrySearchResponse search_registry_models');
      const functionEnd = registrySource.indexOf('RemoteRegistrySource parse_remote_registry_source', functionStart);
      const searchFunction = registrySource.slice(functionStart, functionEnd);
      assert.match(searchFunction, /percent_encode\(query \+ " GGUF"\)/);
      assert.match(searchFunction, /filter\.library=gguf/);
      const ggufBlockStart = searchFunction.indexOf('if (gguf_only) {', searchFunction.indexOf('RemoteRegistrySource::HuggingFace'));
      const ggufBlockEnd = searchFunction.indexOf('} else {', ggufBlockStart);
      const ggufBlock = searchFunction.slice(ggufBlockStart, ggufBlockEnd);
      assert.equal((ggufBlock.match(/requests\.push_back/g) || []).length, 2);
      assert.match(searchFunction, /request\.optional/);
      assert.match(searchFunction, /confirmed >= std::min/);
      assert.match(searchFunction, /registry_search_rank/);
    },
  },
];
