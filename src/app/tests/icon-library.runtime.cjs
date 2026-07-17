const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const pkg = JSON.parse(fs.readFileSync(path.join(root, 'package.json'), 'utf8'));
const iconSource = fs.readFileSync(path.join(root, 'src/components/Icon.tsx'), 'utf8');
const localIconSource = fs.readFileSync(path.join(root, 'src/components/localIcons.tsx'), 'utf8');
const chatSource = fs.readFileSync(path.join(root, 'src/components/ChatView.tsx'), 'utf8');
const markdownSource = fs.readFileSync(path.join(root, 'src/components/MarkdownMessage.tsx'), 'utf8');
const stylesSource = fs.readFileSync(path.join(root, 'src/styles/styles.css'), 'utf8');

assert.equal(pkg.dependencies?.['lucide-react'], undefined);
assert.equal(pkg.dependencies?.['react-icons'], undefined);
assert.doesNotMatch(iconSource, /from 'lucide-react'/);
assert.doesNotMatch(iconSource, /from 'react-icons\/si'/);
assert.match(iconSource, /LOCAL_ICON_DEFINITIONS/);
assert.match(iconSource, /data-icon-library=\{definition\.brand \? 'simple-icons' : 'lucide'\}/);
assert.match(localIconSource, /Vendored at build time from lucide-react 1\.25\.0 and react-icons 5\.7\.0/);
assert.match(localIconSource, /"router"/);
assert.match(localIconSource, /"hugging-face"/);
assert.match(localIconSource, /"github"/);
assert.match(localIconSource, /"discord"/);
assert.match(stylesSource, /\.composer__tools-toggle\s*\{[\s\S]*?display:\s*inline-flex;[\s\S]*?white-space:\s*nowrap;/);
assert.doesNotMatch(chatSource, /<svg\b/);
assert.doesNotMatch(markdownSource, /const\s+(?:COPY|CHECK)_ICON/);

const iconNameBlock = iconSource.match(/export type IconName =([\s\S]*?);\n\ninterface IconProps/)[1];
const iconNames = [...iconNameBlock.matchAll(/'([^']+)'/g)].map((match) => match[1]);
for (const iconName of iconNames) {
  assert.match(localIconSource, new RegExp(`"${iconName.replace(/[.*+?^${}()|[\\]\\]/g, '\\$&')}"\\s*:`));
}

console.log(`Local icon contract tests passed (${iconNames.length} registered icons).`);
