# Contributing

Contributions to **AI Campus Copilot Local** are welcome.

## Scope

This file covers `challenge/ai-campus-copilot-local/` only.

To contribute to **Lemonade Server itself**, go upstream:
<https://github.com/lemonade-sdk/lemonade> and its
[contribution guide](https://github.com/lemonade-sdk/lemonade/blob/main/docs/dev/contribute.md).
Please do not send Lemonade changes here — this is a fork whose upstream tree is kept
unmodified on purpose.

## Ground rules for this fork

1. **Do not modify anything outside `challenge/ai-campus-copilot-local/`**, with the sole
   existing exception of the pointer section in the root `README.md`. The upstream C++
   server, router, backends, model registry, build system, generated documentation and
   desktop app must stay untouched.
2. **Do not vendor Lemonade.** This app is a client. It talks to Lemonade over HTTP and
   must not embed, copy or reimplement it.
3. **Never invent endpoint behaviour.** If you are unsure how a Lemonade endpoint
   responds, read `docs/api/openai.md`, `docs/api/lemonade.md` or the endpoint tests in
   `test/` at the repository root. Cite the source in your PR.
4. **Preserve attribution.** Do not remove or weaken the attribution in `README.md`,
   `LICENSE` or the About page.

## Setup

```bash
cd challenge/ai-campus-copilot-local
cp .env.example .env.local
npm install
npm run dev
```

You need Node.js 20+ and a running Lemonade Server with at least one chat model and one
embedding model.

## Before opening a pull request

```bash
npm run check    # lint + typecheck + test + build
```

All four must pass. For UI changes, also run `npm run test:e2e` (needs
`npx playwright install chromium` once).

## Code style

Matches the parent repository's conventions where they apply.

- **TypeScript strict mode.** `any` is an ESLint error. `noUncheckedIndexedAccess` is on,
  so index access yields `T | undefined` — handle it rather than asserting past it.
- **Comments explain WHY, not WHAT.** Default to no comment. Add one only for a hidden
  constraint, a subtle invariant, or behaviour that would surprise a reader. Never write
  comments that restate the code, and never reference the current task or PR
  ("added for X", "fixes #123") — that belongs in the PR description.
- **Small, reusable components.** Shared primitives live in `src/components/ui/`.
- **Imports** use the `@/` alias for anything under `src/`.
- **Never use `dangerouslySetInnerHTML` for model output.** `react/no-danger` is an
  ESLint error; the single existing exception is the fixed-literal theme bootstrap.

## Testing expectations

- Pure logic — chunking, similarity, retrieval, schemas, parsing — needs deterministic
  unit tests.
- Anything touching Lemonade must be tested against **mocked** responses shaped from the
  documented contracts. See `tests/unit/lemonade-client.test.ts`.
- **Be explicit about what is mocked.** Never describe a mocked test as verifying that a
  live Lemonade Server works. `tests/README.md` documents this split and holds the manual
  live-server checklist — extend it if you add a feature it should cover.

## Honesty requirements

This project's credibility rests on not overclaiming, so these are hard rules:

- **Never commit fabricated benchmark data.** `docs/BENCHMARKS.md` deliberately contains
  no results table. If you add measurements, state the hardware, models, Lemonade version
  and methodology.
- **Never display an estimated value as a measured one.** If Lemonade does not report a
  metric, render `—`. Do not derive tokens/sec from character counts.
- **Never let the UI show a connection or readiness state that has not been observed.**
- **Do not normalise ambiguous model output.** A guessed deadline is worse than a missing
  one.

## Reporting bugs

Open an issue at <https://github.com/JEROME-PRAKASH-L/lemonade/issues>, prefixing the
title with `ai-campus-copilot-local:`. Include your OS, Node version, Lemonade version,
the chat and embedding models in use, and what you expected versus what happened.

For security issues see [`SECURITY.md`](SECURITY.md).

## Licence

Contributions are accepted under Apache-2.0, matching this project and the parent
repository.
