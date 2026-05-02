const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const repoRoot = path.resolve(__dirname, '..', '..', '..', '..');

function filePath(relativePath) {
  return path.join(repoRoot, relativePath);
}

function hasFile(relativePath) {
  return fs.existsSync(filePath(relativePath));
}

function readSource(relativePath) {
  const fullPath = filePath(relativePath);
  assert.ok(fs.existsSync(fullPath), `Expected source file to exist: ${relativePath}`);
  return fs.readFileSync(fullPath, 'utf8');
}

function stripComments(source) {
  return source
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/(^|[^:])\/\/.*$/gm, '$1');
}

function normalizeWhitespace(source) {
  return source.replace(/\s+/g, ' ').trim();
}

function assertIncludes(source, needle, message) {
  assert.ok(source.includes(needle), message || `Expected source to include ${needle}`);
}

function assertNotIncludes(source, needle, message) {
  assert.ok(!source.includes(needle), message || `Expected source not to include ${needle}`);
}

function assertMatches(source, pattern, message) {
  assert.ok(pattern.test(source), message || `Expected source to match ${pattern}`);
}

function assertNotMatches(source, pattern, message) {
  assert.ok(!pattern.test(source), message || `Expected source not to match ${pattern}`);
}

function countMatches(source, pattern) {
  const flags = pattern.flags.includes('g') ? pattern.flags : `${pattern.flags}g`;
  return Array.from(source.matchAll(new RegExp(pattern.source, flags))).length;
}

module.exports = {
  repoRoot,
  filePath,
  hasFile,
  readSource,
  stripComments,
  normalizeWhitespace,
  assertIncludes,
  assertNotIncludes,
  assertMatches,
  assertNotMatches,
  countMatches,
};
