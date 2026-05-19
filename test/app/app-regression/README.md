# App regression tests

Run from the repository root:

```bash
node test/app/run-app-regression-tests.cjs
```

These tests are intentionally dependency-free: no Jest, no jsdom, no React
runtime, and no TypeScript package are required. They use focused source-level
regression guards for renderer paths that are easy to break while changing
Model Manager, Model Options, and OmniRouter collection logic.

The suite is cheap enough for CI and local pre-commit use. It is wired into the
`Docs And Style` GitHub Actions workflow, and can still be run directly with
plain Node for fast local checks.

Some tests are feature-aware. Custom-collection-specific checks skip cleanly on
`main` when the feature files are not present, then become active on branches
that add those files.
