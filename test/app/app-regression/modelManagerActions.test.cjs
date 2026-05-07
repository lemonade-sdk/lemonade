const assert = require('node:assert/strict');
const {
  readSource,
  normalizeWhitespace,
  assertIncludes,
  assertMatches,
  countMatches,
} = require('./helpers/source.cjs');

const MODEL_MANAGER = 'src/app/src/renderer/ModelManager.tsx';

const tests = [
  {
    name: 'normal model options button sets target model before opening modal',
    run() {
      const source = normalizeWhitespace(readSource(MODEL_MANAGER));
      assertMatches(
        source,
        /renderLoadOptionsButton[\s\S]*?setOptionsModel\(modelName\)[\s\S]*?setShowModelOptionsModal\(true\)/,
        'The normal settings button must set optionsModel before opening ModelOptionsModal.',
      );
    },
  },
  {
    name: 'normal model options are still rendered for non-collection models',
    run() {
      const source = normalizeWhitespace(readSource(MODEL_MANAGER));
      assertMatches(
        source,
        /!isCollection && renderLoadOptionsButton\(modelName\)/,
        'Non-collection rows must keep the normal Model Options button.',
      );
    },
  },
  {
    name: 'collection-specific options do not replace normal model options',
    run() {
      const source = normalizeWhitespace(readSource(MODEL_MANAGER));
      if (!source.includes('renderCustomCollectionOptionsButton')) return { skip: true, reason: 'custom collection UI is not present on this branch' };
      assertMatches(
        source,
        /isCustomCollectionId\(modelName\) && renderCustomCollectionOptionsButton\(modelName\)[\s\S]*?!isCollection && renderLoadOptionsButton\(modelName\)/,
        'Custom collection options may be added, but normal concrete model options must remain available.',
      );
    },
  },
  {
    name: 'exactly one ModelOptionsModal owner is mounted in ModelManager',
    run() {
      const source = readSource(MODEL_MANAGER);
      assert.equal(countMatches(source, /<ModelOptionsModal\b/), 1, 'ModelManager should own one normal ModelOptionsModal instance.');
    },
  },
  {
    name: 'normal options cancel path closes and clears stale model id',
    run() {
      const source = normalizeWhitespace(readSource(MODEL_MANAGER));
      assertMatches(
        source,
        /onCancel=\{\(\) => \{[\s\S]*?setShowModelOptionsModal\(false\)[\s\S]*?setOptionsModel\(null\)/,
        'Closing ModelOptionsModal should clear both open state and stale optionsModel.',
      );
    },
  },
  {
    name: 'normal options submit path still loads with recipe options',
    run() {
      const source = normalizeWhitespace(readSource(MODEL_MANAGER));
      const submitStart = source.indexOf('onSubmit={(modelName, options) => {');
      assert.notEqual(submitStart, -1, 'ModelOptionsModal submit should receive the selected model name and option state.');

      const submitBlock = source.slice(submitStart, submitStart + 1200);
      assertIncludes(submitBlock, 'setShowModelOptionsModal(false)', 'Submitting normal Model Options should close the modal.');
      assertMatches(
        submitBlock,
        /modelName[\s\S]{0,700}(?:options|recipeOptionsToApi)|(?:options|recipeOptionsToApi)[\s\S]{0,700}modelName/,
        'Submitting normal Model Options should keep both the selected model name and submitted options in the load path.',
      );
      assertIncludes(source, 'recipeOptionsToApi', 'Submitted frontend options should still be converted to API format somewhere in ModelManager.');
    },
  },
  {
    name: 'row actions keep load, delete, and options separated',
    run() {
      const source = normalizeWhitespace(readSource(MODEL_MANAGER));
      assertMatches(
        source,
        /handle(?:Load|Download)Model/,
        'Model rows should keep a concrete load/download action.',
      );
      assertIncludes(source, 'renderDeleteButton', 'Model rows should keep a delete action.');
      assertIncludes(source, 'renderLoadOptionsButton', 'Model rows should keep a normal options action.');
      assertMatches(
        source,
        /e\.stopPropagation\(\)[\s\S]*?setOptionsModel\(modelName\)/,
        'Clicking the options action should stop row propagation before opening options.',
      );
    },
  },
];

module.exports = { tests };
