const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const files = {
  app: path.join(root, 'src/App.tsx'),
  chat: path.join(root, 'src/components/ChatView.tsx'),
  connect: path.join(root, 'src/components/ConnectView.tsx'),
  apps: path.join(root, 'src/components/AppsView.tsx'),
  navigation: path.join(root, 'src/features/navigation/workspaceNavigation.ts'),
};
const sources = Object.fromEntries(Object.entries(files).map(([key, filename]) => [key, fs.readFileSync(filename, 'utf8')]));

for (const [key, filename] of Object.entries(files)) {
  const compiled = ts.transpileModule(sources[key], {
    compilerOptions: {
      target: ts.ScriptTarget.ES2022,
      module: ts.ModuleKind.ESNext,
      jsx: ts.JsxEmit.ReactJSX,
    },
    fileName: filename,
    reportDiagnostics: true,
  });
  const errors = (compiled.diagnostics || []).filter(d => d.category === ts.DiagnosticCategory.Error);
  assert.equal(errors.length, 0, errors.map(d => ts.flattenDiagnosticMessageText(d.messageText, '\n')).join('\n'));
}

assert.match(sources.app, /<span className="titlebar__brand-name">lemonade<\/span>/);
assert.match(sources.app, /type="search"[\s\S]*?role="combobox"[\s\S]*?aria-expanded=\{navigationSearchOpen\}/);
assert.match(sources.app, /aria-activedescendant=/);

assert.match(sources.chat, /role="menuitem"[\s\S]*?data-mcp-entry="lemonade"[\s\S]*?aria-label="Lemonade tools"/);
assert.match(sources.chat, /role="menuitem"[\s\S]*?data-mcp-entry="external"[\s\S]*?aria-label="External MCP servers"/);
assert.match(sources.chat, /openMcpPicker\('lemonade'\)/);
assert.match(sources.chat, /openMcpPicker\('external'\)/);

assert.match(sources.navigation, /defineSection\('app-directory', 'Compatible clients and integrations', 'layers'\)/);
assert.match(sources.connect, /activeSection === 'app-directory'[\s\S]*?<AppsView isActive=\{isActive\} embedded \/>/);
assert.match(sources.apps, /embedded\?: boolean/);

console.log('UI regression contract checks passed.');
