const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const chat = fs.readFileSync(path.join(root, 'src/components/ChatView.tsx'), 'utf8');
const styles = fs.readFileSync(path.join(root, 'src/styles/styles.css'), 'utf8');

const userActions = chat.slice(chat.indexOf("if (message.role === 'user')"), chat.indexOf('const displayModel = message.model'));
assert.match(userActions, /!isEditing && message\.content && \(\s*<div className="message__actions"/);
assert.match(userActions, /<CopyButton text=\{message\.content\} \/>/);
assert.match(userActions, /onEditUser && \(\s*<button[\s\S]*?t\('Edit & resend'\)/);
assert.match(chat, /<CopyButton text=\{message\.content \|\| message\.thinking \|\| ''\}/);

const copyButton = chat.slice(chat.indexOf('const CopyButton'), chat.indexOf('const MessageBubble'));
assert.match(copyButton, /\.then\(\(\) => setState\('copied'\)\)/);
assert.match(copyButton, /\.catch\(\(\) => setState\('failed'\)\)/);
assert.match(copyButton, /setTimeout\(\(\) => setState\('idle'\), 2000\)/);
assert.match(copyButton, /name=\{state === 'copied' \? 'check' : 'copy'\}/);
assert.match(styles, /\.message__action--copied[\s\S]*?color:\s*var\(--success\)/);
assert.match(styles, /\.message__action--failed[\s\S]*?color:\s*var\(--danger\)/);

const generatedImages = chat.slice(chat.indexOf('{message.generatedImages && message.generatedImages.length > 0 && ('), chat.indexOf('{message.audioUrl && ('));
assert.doesNotMatch(generatedImages, /href=\{src\}/, 'generated images download through a Blob, not a data: href');
assert.match(generatedImages, /downloadImage\(src, filename\)/);
assert.match(generatedImages, /message\.generatedImageNames\?\.\[i\]/);
assert.match(generatedImages, /aria-label=\{`\$\{t\('Download'\)\} \$\{t\('Image'\)\} \$\{i \+ 1\}`\}/);
assert.match(chat, /generatedImageNames: undefined,/, 'image names are stripped before persisting');
assert.match(styles, /\.message__image-download\s*\{\s*position:\s*absolute;/);
assert.match(styles, /\.message__image-item\s*\{[^}]*align-self:\s*flex-start;/);

function extractFunction(name) {
  const source = chat.match(new RegExp(`function ${name}\\([^)]*\\): string \\{[\\s\\S]*?\\n\\}`))?.[0];
  assert.ok(source, `${name} helper exists`);
  return source;
}
const helperJs = ts.transpileModule(`${extractFunction('imageDownloadExtension')}\n${extractFunction('imageDownloadName')}`, {
  compilerOptions: { target: ts.ScriptTarget.ES2022 },
}).outputText;
const { imageDownloadExtension, imageDownloadName } = Function(`${helperJs}\nreturn { imageDownloadExtension, imageDownloadName };`)();

assert.equal(imageDownloadExtension('data:image/png;base64,AA=='), 'png');
assert.equal(imageDownloadExtension('data:image/jpeg;base64,AA=='), 'jpg');
assert.equal(imageDownloadExtension('data:image/webp;base64,AA=='), 'webp');
assert.equal(imageDownloadExtension('data:image/svg+xml;base64,AA=='), 'svg');
assert.equal(imageDownloadExtension('https://example.org/image.gif?cache=1'), 'gif');

assert.equal(imageDownloadName('data:image/png;base64,AA==', 'SD-Turbo-1.png', 'fallback-1'), 'SD-Turbo-1.png');
assert.equal(imageDownloadName('data:image/jpeg;base64,AA==', 'SD-Turbo-edit-2.png', 'fallback-1'), 'SD-Turbo-edit-2.jpg');
assert.equal(imageDownloadName('data:image/png;base64,AA==', '', 'Flux 2 Klein-3'), 'Flux-2-Klein-3.png');
assert.equal(imageDownloadName('data:image/png;base64,AA==', undefined, 'lemonade-image-1'), 'lemonade-image-1.png');
assert.equal(imageDownloadName('data:image/png;base64,AA==', '../../evil/name.png', 'x'), '..-..-evil-name.png');

console.log('Chat message actions regression checks passed.');
