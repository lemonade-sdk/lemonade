const {
  readSource,
  normalizeWhitespace,
  assertIncludes,
  assertMatches,
  assertNotMatches,
  hasFile,
} = require('./helpers/source.cjs');

const MODAL = 'src/app/src/renderer/ModelOptionsModal.tsx';
const CUSTOM_COLLECTIONS = 'src/app/src/renderer/utils/customCollections.ts';

function beforeReturn(source, needle) {
  const idx = source.indexOf(needle);
  return idx === -1 ? source : source.slice(0, idx);
}

const tests = [
  {
    name: 'closed modal is explicitly allowed to render nothing',
    run() {
      const source = normalizeWhitespace(readSource(MODAL));
      assertMatches(source, /if \(!isOpen[\s\S]*?return null/, 'Closed ModelOptionsModal should render nothing.');
    },
  },
  {
    name: 'opening the modal schedules the model-options fetch before any early return',
    run() {
      const source = normalizeWhitespace(readSource(MODAL));
      const earlyReturnPrefix = beforeReturn(source, 'return null');
      assertIncludes(earlyReturnPrefix, 'useEffect', 'The fetch useEffect must be declared before render returns.');
      assertIncludes(source, 'serverFetch', 'ModelOptionsModal must fetch model details when opened.');
    },
  },
  {
    name: 'model detail fetch uses the selected model variable and encodes it when collections are enabled',
    run() {
      const source = normalizeWhitespace(readSource(MODAL));
      assertMatches(
        source,
        /serverFetch\(`\/models\/\$\{[^}]*model[^}]*\}`\)/,
        'ModelOptionsModal should request details for the selected model variable.',
      );
      if (hasFile(CUSTOM_COLLECTIONS)) {
        assertMatches(
          source,
          /serverFetch\(`\/models\/\$\{encodeURIComponent\(model\)\}`\)/,
          'Collection-enabled branches should encode the model id at the /models/:id path segment.',
        );
      }
    },
  },
  {
    name: 'loading state wraps the model detail request and response conversion',
    run() {
      const source = normalizeWhitespace(readSource(MODAL));
      assertMatches(source, /setIsLoading\(true\)[\s\S]*?serverFetch/, 'Loading state should be set before fetching options.');
      assertMatches(source, /finally \{[\s\S]*?setIsLoading\(false\)/, 'Loading state should be cleared in finally.');
      assertIncludes(source, 'apiToRecipeOptions(recipe, recipeOptions)', 'Model Options should convert API recipe options into frontend option state.');
    },
  },
  {
    name: 'modal keeps explicit cancellation and escape/click-outside handlers',
    run() {
      const source = normalizeWhitespace(readSource(MODAL));
      assertIncludes(source, "event.key === 'Escape'", 'Escape should close ModelOptionsModal.');
      assertMatches(source, /cardRef\.current[\s\S]*?!cardRef\.current\.contains\(event\.target as Node\)[\s\S]*?onCancel\(\)/, 'Click outside should close ModelOptionsModal.');
      assertIncludes(source, 'handleCancel', 'Cancel button should use an explicit handler.');
    },
  },
  {
    name: 'open modal must not be gated on recipe options being already populated when loading-shell fix is present',
    run() {
      const source = normalizeWhitespace(readSource(MODAL));
      const hasOptionsGatedReturn = /if \(!isOpen \|\| !options\) return null/.test(source);
      const hasOpenOnlyReturn = /if \(!isOpen\) return null/.test(source);

      if (hasOptionsGatedReturn && !hasOpenOnlyReturn && !hasFile(CUSTOM_COLLECTIONS)) {
        return { skip: true, reason: 'main has no loading-shell fix yet' };
      }

      assertNotMatches(
        source,
        /if \(!isOpen \|\| !options\) return null/,
        'Regression guard: once loading-shell UI exists, an open modal must not return null just because options are still undefined.',
      );
    },
  },
];

module.exports = { tests };
