// Standalone runner for the router collection tests.
//
// The single source of truth for these tests is
// test/app/app-regression/routerCollections.test.cjs (the suite CI runs).
// This wrapper only exists so `npm run test:router-collections` and direct
// `node test/app/routerCollections.test.cjs` invocations keep working locally.

const path = require('path');

const { tests } = require(
  path.join(__dirname, 'app-regression', 'routerCollections.test.cjs'),
);

async function main() {
  let passed = 0;
  let skipped = 0;
  const failures = [];

  for (const test of tests) {
    try {
      const result = await test.run();
      if (result && result.skip) {
        skipped += 1;
        console.log(`SKIP  ${test.name} - ${result.reason || 'skipped'}`);
      } else {
        passed += 1;
        console.log(`PASS  ${test.name}`);
      }
    } catch (error) {
      failures.push(test.name);
      console.error(`FAIL  ${test.name}`);
      console.error(error && error.stack ? error.stack : error);
    }
  }

  console.log('');
  if (failures.length === 0) {
    console.log(`All router collection tests passed (${passed} passed, ${skipped} skipped).`);
  } else {
    console.error(`${failures.length} test(s) failed:`);
    for (const name of failures) console.error(`  • ${name}`);
    process.exit(1);
  }
}

main();
