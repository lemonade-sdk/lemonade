# Submission description

Ready to paste into the AMD Lemonade Developer Challenge submission form.
Replace the demo-video placeholder before submitting.

---

## Project title

**AI Campus Copilot Local — a privacy-first student assistant powered by Lemonade local AI**

## Problem

College notices are dense and consequential. A single circular routinely carries an
examination timetable, a fee deadline with a separate late window, an attendance rule
with a condonation exception, and a revaluation cut-off — in formal English, often as a
PDF forwarded through a group chat. Students miss deadlines not because the information
is unavailable but because it is unreadable at a glance.

The obvious fix is to paste the notice into a cloud chatbot. That is a poor trade:
these documents carry roll numbers, fee amounts, eligibility criteria and internal
contacts, and many students are on metered or intermittent connections. Sending a
college circular to a third-party server to find out when an exam starts is more
exposure than the task warrants.

## Solution

AI Campus Copilot Local is a document-grounded assistant that runs entirely on the
student's own machine. Upload a notice, circular, timetable or course PDF; the app
extracts it page by page in the browser, chunks it, embeds it through Lemonade Server,
and answers questions from it with page-level citations.

Beyond Q&A it produces six kinds of summary (including Tamil), extracts deadlines as
validated structured data, and pulls internship and placement details out of recruitment
circulars into readable cards.

The design commitment is that the app never fabricates. Answers outside the document's
content are refused explicitly. Dates the source states ambiguously are stored
un-normalised rather than guessed. Model output that fails schema validation is counted
and reported, never silently repaired.

## Lemonade integration

Lemonade Server does all inference. The app ships a typed client
(`src/lib/lemonade/client.ts`) with request timeouts, `AbortController` cancellation,
typed errors, per-call timing, and Zod validation of every response.

Endpoints used, all confirmed against the Lemonade repository's own API documentation:

- `GET /api/v1/health` — connection state, server version, loaded models
- `GET /api/v1/models` — dynamic model discovery
- `GET /api/v1/system-info` — OS, processor and memory for the performance page
- `GET /api/v1/system-stats` — host resource usage
- `GET /api/v1/stats` — tokens/sec and time to first token
- `POST /api/v1/embeddings` — batched chunk and question embeddings
- `POST /api/v1/chat/completions` — SSE-streamed answers, summaries, structured extraction

**No model name is hardcoded.** The app reads the user's installed models and classifies
them by Lemonade's own label and recipe vocabulary — text-generation models are offered
as chat models, models labelled `embeddings` on the `llamacpp` or `flm` recipes as
embedding models. If nothing qualifies, the setup screen says exactly which `lemonade
pull` command to run instead of failing silently. Connection status is derived strictly
from observed request outcomes, so the UI never shows a false "connected".

## Local-AI impact

This is a workload local inference genuinely suits. The documents are short, the
questions are factual, and the latency budget is a student glancing at their laptop —
all well within what a small quantised model on a consumer CPU, GPU or Ryzen AI NPU
handles. There is no quality ceiling being worked around here; retrieval does the heavy
lifting and the model only has to read five retrieved passages carefully.

It is also a workload where local is a *feature*, not a compromise. Once models are
downloaded, the app works with no internet connection.

## Community usefulness

The pattern — grounded document Q&A with citations, structured extraction, and honest
measurement — generalises well beyond campus notices. The Lemonade client, the
page-aware chunker, the deterministic retrieval layer and the JSON-recovery-plus-Zod
extraction pipeline are all independent modules under Apache-2.0 that another Lemonade
app can lift directly.

For students specifically it is immediately usable: clone, `npm install`, point it at a
running Lemonade Server, upload a notice.

## Technical depth

- **Page-aware chunking** — 500-word windows with 80-word overlap that never span a page
  boundary, which is what makes every citation exact. Covered by deterministic unit tests.
- **Deterministic retrieval** — cosine similarity with stable tie-breaking, a relevance
  threshold, graceful handling of zero-magnitude and dimension-mismatched vectors.
- **Hierarchical summarisation** — long documents are summarised in stages so no single
  request carries the whole text.
- **Structured extraction** — balanced-bracket JSON recovery from prose-wrapped replies,
  then Zod validation. `normalizedDate` must survive an ISO round-trip, so `2027-02-31`
  is rejected to null rather than rolling over to 3 March and becoming a plausible-looking
  hallucinated deadline.
- **Prompt-injection resistance** — document text is fenced and labelled untrusted, with
  the ignore-instructions rule restated after the untrusted block; model output is
  rendered as React text nodes only, and extracted links are rejected unless they parse
  as `http(s)`. Asserted by test.
- **Honest measurement** — tokens/sec comes from Lemonade's `/stats` endpoint and is
  never estimated; unavailable values render as `—`.
- **Server-side proxy with an endpoint allowlist** — keeps the Lemonade URL and any API
  key out of the browser bundle, and makes destructive Lemonade endpoints unreachable
  from client code.

TypeScript strict mode with `noUncheckedIndexedAccess`. 107 unit tests and a Playwright
end-to-end test, all passing.

## Privacy benefits

- Files are parsed in the browser and never transmitted.
- Extracted text reaches only `localhost`: browser → local Next.js → local Lemonade.
- Chunks, embeddings, chat history and benchmarks live in browser IndexedDB.
- No cloud AI provider dependency, API-key handling or code path exists in the project.
- No telemetry, no analytics, no remote fonts or scripts.
- One button deletes everything.

## GitHub

https://github.com/JEROME-PRAKASH-L/lemonade/tree/main/challenge/ai-campus-copilot-local

This repository is a fork of the official
[lemonade-sdk/lemonade](https://github.com/lemonade-sdk/lemonade) repository. Lemonade
Server is maintained by the Lemonade community and AMD contributors and is not my work.
My contribution is the application in `challenge/ai-campus-copilot-local/`, licensed
Apache-2.0 to match upstream. Upstream source, licence, attribution and Git history are
unmodified apart from one short pointer section added to the root README.

## Demo video

<!-- TODO: paste the demo video URL here before submitting -->
`[demo video link to be added]`

Recording script: [`docs/DEMO_SCRIPT.md`](DEMO_SCRIPT.md)

## Known limitations

Stated plainly, because overclaiming would undercut the point of the project.

- **Scanned PDFs are not supported.** Extraction is text-layer only. Image-only pages are
  detected and the app says OCR would be needed rather than returning nothing silently.
  Tesseract.js is the obvious next step and is not implemented.
- **No published benchmark numbers.** The benchmark runner works and records genuine
  timings, but I have not run a controlled comparison across hardware or models, so this
  submission claims no throughput figures. `docs/BENCHMARKS.md` explains what the runner
  measures and deliberately contains no results table.
- **Answer quality tracks the chosen model.** A 0.6B model on CPU is noticeably weaker
  than a larger model on GPU or NPU. Grounding, citation and refusal behaviour are the
  app's contribution; fluency is the model's.
- **Tamil output depends on the model's Tamil coverage.** The prompt requests Tamil and
  the UI renders it correctly, but a model weak in Tamil will produce weak Tamil.
- **Structured extraction is best-effort.** Sections whose output fails Zod validation
  are skipped and counted in the UI, not repaired by guessing.
- **Retrieval is single-document.** No cross-document search.
- **The relevance threshold is a fixed 0.2 cosine constant.** Embedding models differ in
  score distribution; per-model calibration would be better.
- **No automated test uses a live Lemonade Server.** Unit and end-to-end tests use mocked
  responses shaped from Lemonade's documented contracts; they verify this app's side of
  the integration, not model quality. A manual live-server checklist is in
  `tests/README.md`.
- **Tested on one machine.** Windows 11 is the documented primary environment. The stack
  is cross-platform but I have not verified it broadly.
