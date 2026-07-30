# Swigert presets/a11y revision

Date: 2026-07-30T10:34:29.051-06:00

## Results

- Revised only `src/app/tests/a11y.spec.ts` for this assignment.
- A76 now navigates through the top-level Apps route and validates the current marketplace search control as `Search apps`.
- A116 and A117 now explicitly select the README tab, assert `aria-selected="true"`, and only then wait for the README panel. Configuration-first remains the tested UI behavior.
- No Apps production change was needed; the accessible-name check passes against the current source.

## Validation

- Focused A76/A116/A117 run: 3 passed.
- Full accessibility suite: 145 passed.
- No blockers remain.
