#!/usr/bin/env node
const fs = require('node:fs');
const path = require('node:path');

const suiteDir = path.join(__dirname, 'app-regression');
const files = fs.readdirSync(suiteDir)
  .filter((name) => name.endsWith('.test.cjs'))
  .sort();

let passed = 0;
let skipped = 0;
let failed = 0;

async function main() {
  for (const file of files) {
    const mod = require(path.join(suiteDir, file));
    const tests = Array.isArray(mod.tests) ? mod.tests : [];
    for (const test of tests) {
      const label = `${file} :: ${test.name}`;
      try {
        const result = await test.run();
        if (result && result.skip) {
          skipped += 1;
          console.log(`SKIP ${label} - ${result.reason || 'skipped'}`);
        } else {
          passed += 1;
          console.log(`PASS ${label}`);
        }
      } catch (error) {
        failed += 1;
        console.error(`FAIL ${label}`);
        console.error(error && error.stack ? error.stack : error);
      }
    }
  }

  console.log(`\nApp regression tests: ${passed} passed, ${skipped} skipped, ${failed} failed.`);
  process.exit(failed === 0 ? 0 : 1);
}

main();
