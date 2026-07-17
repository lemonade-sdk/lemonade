const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const React = require('react');
const { renderToStaticMarkup } = require('react-dom/server');
const {
  Eye,
  MessageCircle,
  Router,
  Settings,
  UserRoundCog,
  Wrench,
} = require('lucide-react');
const { SiDiscord, SiGithub, SiHuggingface } = require('react-icons/si');

const root = path.resolve(__dirname, '..');
const pkg = JSON.parse(fs.readFileSync(path.join(root, 'package.json'), 'utf8'));
const iconSource = fs.readFileSync(path.join(root, 'src/components/Icon.tsx'), 'utf8');
const chatSource = fs.readFileSync(path.join(root, 'src/components/ChatView.tsx'), 'utf8');
const markdownSource = fs.readFileSync(path.join(root, 'src/components/MarkdownMessage.tsx'), 'utf8');
const stylesSource = fs.readFileSync(path.join(root, 'src/styles/styles.css'), 'utf8');

assert.equal(pkg.dependencies['lucide-react'], '1.25.0');
assert.equal(pkg.dependencies['react-icons'], '5.7.0');
assert.match(iconSource, /from 'lucide-react'/);
assert.match(iconSource, /from 'react-icons\/si'/);
assert.doesNotMatch(iconSource, /<(?:svg|path|circle|rect|line|polyline|polygon)\b/);
assert.doesNotMatch(iconSource, /switch\s*\(name\)/);
assert.match(iconSource, /strokeWidth=\{1\.5\}/);
assert.match(stylesSource, /\.composer__tools-toggle\s*\{[\s\S]*?display:\s*inline-flex;[\s\S]*?white-space:\s*nowrap;/);

for (const expected of [
  /chat:\s*MessageCircle/,
  /eye:\s*Eye/,
  /wrench:\s*Wrench/,
  /'user-round-cog':\s*UserRoundCog/,
  /router:\s*Router/,
  /settings:\s*Settings/,
]) {
  assert.match(iconSource, expected);
}

assert.doesNotMatch(chatSource, /<svg\b/);
assert.doesNotMatch(markdownSource, /const\s+(?:COPY|CHECK)_ICON/);
assert.doesNotMatch(markdownSource, /<svg[^>]*stroke=/);


for (const Component of [UserRoundCog, Router, Settings, MessageCircle, Eye, Wrench]) {
  const markup = renderToStaticMarkup(React.createElement(Component, {
    size: 19,
    strokeWidth: 1.5,
    absoluteStrokeWidth: true,
  }));
  assert.match(markup, /^<svg[^>]*>/);
  assert.match(markup, /stroke="currentColor"/);
}

for (const Component of [SiHuggingface, SiGithub, SiDiscord]) {
  const markup = renderToStaticMarkup(React.createElement(Component, { size: 19 }));
  assert.match(markup, /^<svg[^>]*>/);
}

console.log('Icon library contract tests passed.');
