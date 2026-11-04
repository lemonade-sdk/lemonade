const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const chat = fs.readFileSync(path.join(root, 'src/components/ChatView.tsx'), 'utf8');
const styles = fs.readFileSync(path.join(root, 'src/styles/styles.css'), 'utf8');

const userActions = chat.slice(chat.indexOf("if (message.role === 'user')"), chat.indexOf('const displayModel = message.model'));
assert.match(userActions, /!isEditing && message\.content && \(\s*<div className="message__actions"/);
assert.match(userActions, /onClick=\{\(\) => \{\s*void copyTextToClipboard\(message\.content\)[\s\S]*?\.catch\(\(\) => setCopyFailed\(true\)\)/);
assert.match(userActions, /<Icon name="copy" size=\{13\} \/> \{copyFailed \? t\('Copy failed'\) : t\('Copy'\)\}/);
assert.match(userActions, /onEditUser && \(\s*<button[\s\S]*?t\('Edit & resend'\)/);

const generatedImages = chat.slice(chat.indexOf('{message.generatedImages && message.generatedImages.length > 0 && ('), chat.indexOf('{message.audioUrl && ('));
assert.match(generatedImages, /href=\{src\}/);
assert.match(generatedImages, /download=\{`lemonade-image-\$\{i \+ 1\}\.\$\{imageDownloadExtension\(src\)\}`\}/);
assert.match(generatedImages, /aria-label=\{`\$\{t\('Download'\)\} \$\{t\('Image'\)\} \$\{i \+ 1\}`\}/);
assert.match(generatedImages, /<Icon name="download" size=\{16\} \/>/);
assert.match(styles, /\.message__image-download\s*\{\s*position:\s*absolute;/);
assert.match(styles, /\.message__image-item\s*\{[^}]*align-self:\s*flex-start;/);

const imageExtensionSource = chat.match(/function imageDownloadExtension\(src: string\): string \{[\s\S]*?\n\}/)?.[0];
assert.ok(imageExtensionSource, 'image download extension helper exists');
const helperJs = ts.transpileModule(imageExtensionSource, {
  compilerOptions: { target: ts.ScriptTarget.ES2022 },
}).outputText;
const imageDownloadExtension = Function(`${helperJs}\nreturn imageDownloadExtension;`)();
assert.equal(imageDownloadExtension('data:image/png;base64,AA=='), 'png');
assert.equal(imageDownloadExtension('data:image/jpeg;base64,AA=='), 'jpg');
assert.equal(imageDownloadExtension('data:image/webp;base64,AA=='), 'webp');
assert.equal(imageDownloadExtension('data:image/svg+xml;base64,AA=='), 'svg');
assert.equal(imageDownloadExtension('https://example.org/image.gif?cache=1'), 'gif');

console.log('Chat message actions regression checks passed.');
