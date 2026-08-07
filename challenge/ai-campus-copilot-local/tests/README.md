# Tests

## What is mocked and what is not

| Suite | Lemonade Server | What it proves |
|-------|-----------------|----------------|
| `tests/unit/*.test.ts` (Vitest) | **Mocked** — `fetch` is replaced with a stub returning payloads copied from the parent repository's API docs | Chunking, cosine similarity, retrieval ranking, schema validation, SSE stream parsing, error classification, IndexedDB storage |
| `tests/e2e/setup-flow.spec.ts` (Playwright) | **Mocked** — every `/api/lemonade/**` route is intercepted | Connection states, dynamic model discovery and filtering, persistence of model choices across reloads |
| Live-server checklist (below) | **Real** — a running `lemond` | That real inference actually works end to end |

**No automated test in this repository talks to a real Lemonade Server.** The mocked
suites verify this application's side of the integration — request shape, response
parsing, error handling and UI state. They cannot prove that a given model produces
good answers, and they are not benchmarks.

## Running the automated tests

```bash
npm run test        # Vitest unit tests
npm run test:e2e    # Playwright (builds the app first)
```

Playwright needs its browser once per machine:

```bash
npx playwright install chromium
```

## Live-server checklist

Run this manually against a real Lemonade Server before claiming the integration
works. It is not automated because it depends on which models you have downloaded.

1. Start Lemonade Server and confirm `curl http://127.0.0.1:13305/api/v1/health` returns `{"status":"ok",...}`.
2. Start this app with `npm run dev` and open `/setup`. The status must read
   **Connected**, and both model lists must be populated from your own install.
3. Select a chat model and an embedding model.
4. Upload `samples/semester-examination-notice.md` on `/documents`. Watch the
   embedding progress bar reach 100% and the document reach status `embedded`.
5. On `/chat`, ask *"When do the semester examinations begin?"*. The answer must
   cite a page, and the retrieved-sources panel must show non-zero similarity scores.
6. Ask something the document does not cover, e.g. *"What is the hostel wifi password?"*.
   The answer must say the information was not found rather than inventing one.
7. On `/deadlines`, press **Extract deadlines**. Entries must appear with evidence
   quotes; any ambiguous date must show *not normalised*.
8. On `/performance`, press **Run benchmark**. Every row must show measured
   timings, and tokens/sec must either show a number from `/api/v1/stats` or `—`.
