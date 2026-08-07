# Changelog

All notable changes to AI Campus Copilot Local are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0] — 2027-04-06

First release, built for the AMD Lemonade Developer Challenge.

### Added

**Lemonade integration**
- Typed Lemonade client with health check, model listing, chat completion, SSE streaming
  chat, embeddings, system info, system stats and performance stats.
- Request timeouts, `AbortController` cancellation, typed errors distinguishing
  server-unavailable, model-unavailable, timeout, cancellation, unauthorized, bad request
  and invalid response.
- Zod validation of every Lemonade response.
- Per-call timing on every client method.
- Server-side proxy route with a seven-endpoint allowlist and Zod request validation,
  keeping `LEMONADE_SERVER_URL` and `LEMONADE_API_KEY` out of the browser bundle.

**Setup and model discovery**
- Connection setup screen with checking, connected, server-unavailable,
  no-models-installed and request-failed states. Status is never optimistic.
- Dynamic model discovery from `GET /api/v1/models`, classified into chat-capable and
  embedding-capable using Lemonade's own label and recipe vocabulary.
- Model selections persisted locally in IndexedDB.
- Actionable guidance when the server is down or no compatible model is installed.

**Document pipeline**
- Drag-and-drop and file-picker upload for PDF, TXT and Markdown.
- Extension, MIME, magic-byte, size and filename validation; 20 MB, 300-page and
  1.5M-character limits.
- Page-aware PDF extraction via bundled PDF.js, with scanned-page detection and an
  OCR-required warning.
- Heading-aware Markdown extraction preserving section titles.
- Page-aware chunking at 500 words with 80-word overlap; chunks never span pages.
- Batched embedding with live progress, ordered by response `index`.
- IndexedDB storage via Dexie across seven tables, with cascading deletes, per-document
  delete, delete-all and embedding rebuild.

**Retrieval and chat**
- Cosine similarity with deterministic tie-breaking and safe handling of zero-magnitude
  and dimension-mismatched vectors.
- Top-k retrieval with a relevance threshold.
- Grounded streaming chat with page citations, an expandable sources panel showing page,
  chunk number, similarity score and excerpt, stop generation, copy answer and clear
  history.
- Prompt-injection resistance: untrusted-data fencing with the ignore-instructions rule
  restated after the untrusted block.
- English and Tamil supported.

**Student features**
- Six summary kinds — short, detailed, key points, student-friendly, English and Tamil —
  with hierarchical summarisation for long documents.
- Deadline extraction into Zod-validated JSON across eight categories, with list,
  timeline and category-filtered views. Ambiguous dates are left un-normalised.
- Internship and placement extraction into validated cards.
- Balanced-bracket JSON recovery from prose-wrapped model replies.

**Dashboard, performance and privacy**
- Dashboard with connection state, model selections, document and chunk counts,
  questions asked, deadlines and opportunities detected, response timings and recent
  activity.
- Performance page reporting only genuine measurements, with a five-question benchmark
  runner and JSON/CSV export.
- Privacy page with a data-flow explanation, live network-activity indicator and
  delete-all control.

**UI**
- Twelve routes with a desktop sidebar, mobile navigation, dark and light themes,
  loading skeletons, toast notifications, empty states, error boundaries, a local-AI
  badge and a Lemonade status indicator.
- System font stacks only — no remote fonts, scripts or images.

**Quality**
- 107 unit tests (Vitest) covering chunking, cosine similarity, retrieval, schema
  validation, SSE stream parsing, error classification, file validation, prompt
  construction and IndexedDB storage.
- One Playwright end-to-end test covering connection states, model discovery and
  filtering, and persistence across reloads.
- TypeScript strict mode with `noUncheckedIndexedAccess`; ESLint with `no-explicit-any`
  and `react/no-danger` as errors.

**Documentation**
- README with Mermaid architecture diagram, Windows-first setup and attribution.
- `docs/ARCHITECTURE.md`, `docs/DEMO_SCRIPT.md`, `docs/BENCHMARKS.md`,
  `docs/PRIVACY.md`, `docs/SUBMISSION.md`.
- `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, `SECURITY.md`, Apache-2.0 `LICENSE`.
- Three fictional sample documents.

### Known limitations

Scanned PDFs are not supported (text-layer extraction only). No benchmark numbers are
published — the runner works but no controlled cross-hardware comparison has been run.
Retrieval is single-document. The relevance threshold is a fixed constant. No automated
test uses a live Lemonade Server. See `README.md` and `docs/SUBMISSION.md` for the full
list.

[0.1.0]: https://github.com/JEROME-PRAKASH-L/lemonade/tree/main/challenge/ai-campus-copilot-local
