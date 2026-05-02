# App regression tests

Run from the repository root:

```bash
node test/app/run-app-regression-tests.cjs
```

These tests are intentionally dependency-free: no Jest, no jsdom, no React
runtime, and no TypeScript package are required. They use source-level guards
for renderer regressions that are easy to introduce while changing Model
Manager, Model Options, and OmniRouter collection logic.

The suite is meant to be cheap enough for a precommit hook. Tests that cover
feature-branch-only custom collection files skip cleanly on `main` when those
files are not present.
