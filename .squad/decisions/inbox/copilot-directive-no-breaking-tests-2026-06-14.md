### 2026-06-14: Tests must pass — non-negotiable
**By:** Kyle (kpoin) (via Copilot)
**What:** Anyone making code changes is responsible for keeping existing tests passing. If a change affects a test (renamed UI label, changed selector, altered behavior), the test is updated in the SAME commit/PR — never deferred. Broken tests ship nothing.
**Why:** Kyle: "No breaking tests ... This is non-negotiable — broken tests ship nothing."
**Scope:** All code changes in this repository. Applies to every agent and contributor.
**Definition of done addition:**
- Run the relevant test suite before pushing (e.g. `npm run test:a11y`, `npx playwright test`, Python integration tests under `test/`, C++ tests where applicable)
- Update tests in the same commit as the code change that broke them
- If a test cannot be fixed within the scope of the change (e.g. flaky for unrelated reason), explicitly call it out — do not silently disable
**Enforcement:** Reviewer (Lovell) blocks merges with failing tests. Self-check is expected before requesting review.
