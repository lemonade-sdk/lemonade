const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const appRoot = path.resolve(__dirname, '..');
const srcRoot = path.resolve(appRoot, '..');
const connectPath = path.join(appRoot, 'src/components/ConnectView.tsx');
const apiPath = path.join(appRoot, 'src/api.ts');
const runtimeConfigPath = path.join(srcRoot, 'cpp/server/runtime_config.cpp');
const modelManagerPath = path.join(srcRoot, 'cpp/server/model_manager.cpp');

const connectSource = fs.readFileSync(connectPath, 'utf8');
const apiSource = fs.readFileSync(apiPath, 'utf8');
const runtimeConfigSource = fs.readFileSync(runtimeConfigPath, 'utf8');
const modelManagerSource = fs.readFileSync(modelManagerPath, 'utf8');

for (const [filename, source] of [[connectPath, connectSource], [apiPath, apiSource]]) {
  const compiled = ts.transpileModule(source, {
    compilerOptions: {
      target: ts.ScriptTarget.ES2022,
      module: ts.ModuleKind.ESNext,
      jsx: ts.JsxEmit.ReactJSX,
    },
    fileName: filename,
    reportDiagnostics: true,
  });
  const errors = (compiled.diagnostics || []).filter(
    diagnostic => diagnostic.category === ts.DiagnosticCategory.Error,
  );
  assert.equal(
    errors.length,
    0,
    errors.map(diagnostic => ts.flattenDiagnosticMessageText(diagnostic.messageText, '\n')).join('\n'),
  );
}

assert.match(apiSource, /await this\._json\('\/internal\/set',[\s\S]*models_dir:[\s\S]*extra_models_dir:/,
  'directory settings must be applied through the server runtime config endpoint');

assert.match(connectSource,
  /useEffect\(\(\) => \{\s*setDirectoryNotice\(null\);\s*setDirectoryError\(null\);\s*\}, \[isActive, activeSection\]\);/,
  'directory result messages must clear whenever the active view or Settings section changes');
assert.match(connectSource,
  /const handleDirectoryChange = [\s\S]*setDirectoryNotice\(null\);[\s\S]*setDirectoryError\(null\);/,
  'editing a directory after save must clear stale saved/error state');

assert.match(runtimeConfigSource, /validate_extra_models_dir_access/,
  'runtime config must preflight external directory access before applying it');
assert.match(runtimeConfigSource, /fs::directory_iterator probe\(dir, read_ec\)/,
  'runtime config must verify that an existing external directory can be enumerated');
assert.match(runtimeConfigSource, /not readable by the Lemonade server/,
  'permission failures must surface a user-facing server error');

assert.match(modelManagerSource, /fs::directory_options::skip_permission_denied/,
  'extra-model discovery must skip inaccessible nested directories');
assert.match(modelManagerSource,
  /try \{\s*discovered_models = discover_extra_models\(\);\s*\} catch \(const std::exception& e\)/,
  'optional extra-model discovery failures must not abort the registered model cache rebuild');
assert.match(modelManagerSource, /keeping registered models/,
  'the cache fallback should document preservation of registered models');

console.log('Directory settings regression contract checks passed.');
