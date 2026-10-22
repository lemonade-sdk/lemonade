'use strict';

// GUI3's app-regression runner selects only gui3.test.cjs on merged branches.
// Run the shared Model Manager route contract through GUI3's auto-discovered
// runtime suite without editing either test runner.
const { tests } = require('../../../test/app/app-regression/modelManagerGui3Compat.test.cjs');

(async () => {
  let passed = 0;
  for (const test of tests) {
    const result = await test.run();
    if (result?.skip) {
      throw new Error(`${test.name}: unexpected skip on GUI3 (${result.reason})`);
    }
    console.log(`PASS ${test.name}`);
    passed += 1;
  }
  console.log(`Model Manager GUI3 route contracts: ${passed} passed.`);
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
