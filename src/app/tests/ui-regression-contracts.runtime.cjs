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
  backendManager: path.join(root, 'src/components/BackendManager.tsx'),
  modelManager: path.join(root, 'src/components/ModelManager.tsx'),
  catalogLayout: path.join(root, 'src/components/WorkspaceCatalogLayout.tsx'),
  mcpPanel: path.join(root, 'src/components/McpPanel.tsx'),
  api: path.join(root, 'src/api.ts'),
  mcpRuntime: path.join(root, 'src/tools/mcpRuntime.ts'),
  navigation: path.join(root, 'src/features/navigation/workspaceNavigation.ts'),
};
const sources = Object.fromEntries(Object.entries(files).map(([key, filename]) => [key, fs.readFileSync(filename, 'utf8')]));
const mcpClientHeader = fs.readFileSync(path.join(root, '../cpp/include/lemon/mcp_client.h'), 'utf8');
const mcpClientSource = fs.readFileSync(path.join(root, '../cpp/server/mcp_client.cpp'), 'utf8');
const styles = fs.readFileSync(path.join(root, 'src/styles/styles.css'), 'utf8');

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

// JSX text and JSX attribute strings are not JavaScript string literals: a `\uXXXX`
// sequence there reaches the user verbatim instead of the character it names.
const jsxEscape = /\\(?:u\{[0-9a-fA-F]+\}|u[0-9a-fA-F]{4}|x[0-9a-fA-F]{2})/;
const collectTsx = dir => fs.readdirSync(dir, { withFileTypes: true }).flatMap(entry => {
  const full = path.join(dir, entry.name);
  if (entry.isDirectory()) return collectTsx(full);
  return entry.isFile() && full.endsWith('.tsx') ? [full] : [];
});

for (const filename of collectTsx(path.join(root, 'src'))) {
  const sourceFile = ts.createSourceFile(filename, fs.readFileSync(filename, 'utf8'), ts.ScriptTarget.ES2022, true, ts.ScriptKind.TSX);
  const visit = node => {
    if (ts.isJsxText(node) || (ts.isStringLiteral(node) && node.parent && ts.isJsxAttribute(node.parent))) {
      const found = node.getText(sourceFile).match(jsxEscape);
      if (found) {
        const { line } = sourceFile.getLineAndCharacterOfPosition(node.getStart(sourceFile));
        assert.fail(`${path.relative(root, filename)}:${line + 1} renders "${found[0]}" literally; write the character itself`);
      }
    }
    ts.forEachChild(node, visit);
  };
  visit(sourceFile);
}

// The accent stays yellow in both themes but every --bg-* token flips from
// near-black to near-white, so accent-background text must come from --accent-on.
const accentBackground = /background(?:-color)?:\s*var\(--accent(?:-hover|-strong)?\)/;
const themeFlippedText = /color:\s*var\(--bg-[\w-]+\)/;

for (const stylesheet of ['src/styles/styles.css', 'src/styles/critical.generated.css']) {
  const css = fs.readFileSync(path.join(root, stylesheet), 'utf8');
  for (const [, selector, body] of css.matchAll(/([^{}]+)\{([^{}]*)\}/g)) {
    if (accentBackground.test(body) && themeFlippedText.test(body)) {
      assert.fail(`${stylesheet}: ${selector.trim().split('\n').pop().trim()} puts theme-flipped text on an accent background; use var(--accent-on)`);
    }
  }
}

assert.match(sources.app, /<span className="titlebar__brand-name">lemonade<\/span>/);
assert.match(sources.app, /type="search"[\s\S]*?role="combobox"[\s\S]*?aria-expanded=\{navigationSearchOpen\}/);
assert.match(sources.app, /aria-activedescendant=/);
assert.match(sources.app, /className=\{`titlebar\$\{isDesktop \? ' titlebar--desktop' : ''\}`\}/);
assert.match(styles, /\.titlebar\s*\{[\s\S]*?grid-template-columns:\s*minmax\(0, 1fr\) auto minmax\(0, 1fr\)/);
assert.match(styles, /@media \(max-width: 900px\)[\s\S]*?\.titlebar\.titlebar--desktop\s*\{[\s\S]*?column-gap:\s*var\(--space-1\)/);
assert.match(styles, /@media \(max-width: 900px\)[\s\S]*?\.titlebar--desktop \.titlebar__brand\s*\{[\s\S]*?margin-inline-start:\s*var\(--space-1\)/);
assert.match(styles, /@media \(max-width: 820px\)[\s\S]*?\.titlebar--desktop \.titlebar__nav\s*\{[\s\S]*?transform:\s*translateX\(clamp\(-44px, calc\(\(100vw - 820px\) \* 0\.2\), 0px\)\)/);
assert.match(styles, /\.titlebar--desktop \.titlebar__right,[\s\S]*?\.titlebar--desktop \.titlebar__utility-menu\s*\{\s*gap:\s*var\(--space-1\)/);
assert.match(styles, /@media \(max-width: 480px\)[\s\S]*?\.titlebar\.titlebar--desktop\s*\{[\s\S]*?grid-template-columns:\s*minmax\(0, 1fr\) auto minmax\(0, 1fr\)/);
assert.match(styles, /@media \(max-width: 480px\)[\s\S]*?\.titlebar--desktop \.titlebar__nav button\s*\{[\s\S]*?min-width:\s*30px/);

assert.match(sources.chat, /role="menuitem"[\s\S]*?data-mcp-entry="tools"[\s\S]*?aria-label="Tools"/);
assert.doesNotMatch(sources.chat, /data-mcp-entry="(?:lemonade|external)"/);
assert.match(sources.chat, /const openMcpPicker = useCallback\(\(\) => \{[\s\S]*?setMcpPickerOpen\(true\)/);
assert.match(sources.chat, /role="tab"[\s\S]*?>[\s\S]*?Lemonade tools[\s\S]*?<\/button>/);
assert.match(sources.chat, /role="tab"[\s\S]*?>[\s\S]*?External MCP servers[\s\S]*?<\/button>/);

assert.doesNotMatch(sources.navigation, /defineSection\('app-directory'/);
assert.doesNotMatch(sources.connect, /AppsView|app-directory/);
assert.match(sources.apps, /<WorkspaceCatalogLayout[\s\S]*?railLabel="App categories"/);
assert.match(sources.apps, /className="apps-workspace"/);
assert.doesNotMatch(sources.apps, /embedded\?: boolean|apps__category-filters|WorkspaceMetadataChip/);
assert.match(sources.catalogLayout, /workspace-catalog-layout\$\{railCollapsed \? ' workspace--rail-collapsed' : ''\}/);
assert.match(sources.catalogLayout, /className="workspace-filter-list"/);

assert.match(sources.backendManager, /const \[showLogos, setShowLogos\] = useState\(true\)/);
assert.match(sources.backendManager, /data-backends-unsupported-toggle[\s\S]*?data-backends-logo-toggle/);
assert.match(sources.backendManager, /checked=\{showLogos\}[\s\S]*?<span>Show logos<\/span>/);
assert.match(sources.backendManager, /showLogos \? \([\s\S]*?data-backend-logo[\s\S]*?: \([\s\S]*?workspace-card__name backend-card__name/);

assert.match(sources.modelManager, /EXTRA_MODELS_DIR_SOURCE = 'extra_models_dir'/);
assert.match(sources.modelManager, /isExtraModelsDirModel\(model\)[\s\S]*?showExternalModelDeleteNotice\(model\)[\s\S]*?return;/);
assert.match(sources.modelManager, /managed in your external models folder\. Delete it directly from that folder\./);
assert.doesNotMatch(sources.modelManager, /openExternalModelsFolder|manager__toast-folder-action/);

assert.match(sources.mcpPanel, /transport: 'streamable-http'/);
assert.match(sources.mcpPanel, />HTTP endpoint</);
assert.match(sources.mcpPanel, />Local process</);
assert.match(sources.mcpPanel, /placeholder="http:\/\/127\.0\.0\.1:3000\/mcp"/);
assert.match(sources.mcpPanel, />Bearer token from environment</);
assert.match(sources.mcpPanel, /Test connection/);
assert.match(sources.mcpPanel, /api\.testMcpServer\(serverPayload\(draft\)\)/);
assert.match(sources.api, /export type McpTransport = 'streamable-http' \| 'stdio' \| 'builtin'/);
assert.match(sources.api, /async testMcpServer\(/);
assert.doesNotMatch(sources.api, /body: \{ server: \{ \.\.\.server, transport: 'stdio' \} \}/);
assert.match(sources.mcpRuntime, /server\.transport === 'streamable-http'/);

assert.match(mcpClientHeader, /std::string url;/);
assert.match(mcpClientHeader, /std::string bearer_token;/);
assert.match(mcpClientSource, /kStreamableHttpTransport = "streamable-http"/);
assert.match(mcpClientSource, /server\.Post\("\/internal\/mcp\/servers\/test"/);
assert.match(mcpClientSource, /enable_server_certificate_verification\(true\)/);
assert.match(mcpClientSource, /Plain HTTP is allowed only for localhost/);
assert.match(mcpClientSource, /connect_http\(new_config\)/);
assert.doesNotMatch(mcpClientSource, /httplib::stream/);
assert.match(mcpClientSource, /httplib::Request request;/);
assert.match(mcpClientSource, /client\.send\(request\)/);
assert.match(mcpClientSource, /kMaxMessageBytes - received/);
assert.match(mcpClientSource, /class SseJsonDecoder/);
assert.doesNotMatch(mcpClientSource, /Mcp-Method|Mcp-Name/);

console.log('UI regression contract checks passed.');
