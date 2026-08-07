# Security

Scope: the application in `challenge/ai-campus-copilot-local/`.

For vulnerabilities in **Lemonade Server itself**, report them to the upstream project at
<https://github.com/lemonade-sdk/lemonade/issues> — not here.

## Reporting

Open an issue at <https://github.com/JEROME-PRAKASH-L/lemonade/issues> with the
`ai-campus-copilot-local` prefix in the title. This is a hackathon project with no
security SLA; please do not rely on it for anything sensitive without your own review.

## Threat model

The app runs locally and is driven by two untrusted inputs:

1. **Uploaded documents** — arbitrary attacker-influenced content (a notice forwarded
   through a group chat has unknown provenance).
2. **Model output** — a local model's response, which can be wrong, malformed, or
   steered by an injected instruction inside a document.

Neither is trusted. Lemonade Server itself and the user's own machine are trusted.

## Controls

### Input validation

- Extension allowlist: `.pdf`, `.txt`, `.md`, `.markdown` only.
- Browser-reported MIME type must be consistent with the extension; mismatches are
  rejected.
- PDFs are checked for `%PDF-` magic bytes before PDF.js sees them.
- Size limit 20 MB; empty files rejected.
- Hard caps of 300 pages and 1,500,000 extracted characters, enforced during extraction.
- Filenames are stripped of directory components (`../`, `C:\`) and control characters,
  and never resolve to an empty string.

### Request handling

- Every request the browser makes passes through a Next.js route handler that validates
  the body with Zod before forwarding: bounded message counts, bounded content lengths,
  bounded batch sizes, constrained `temperature` and token limits.
- The proxy exposes an **explicit allowlist of seven endpoints**. Lemonade's destructive
  and administrative surface (`/api/v1/delete`, `/api/v1/pull`, `/internal/*`) is not
  routable from browser code.
- All requests carry a timeout and an `AbortController`; users can cancel long
  generations.
- `LEMONADE_SERVER_URL` and `LEMONADE_API_KEY` are server-side environment variables and
  never enter the browser bundle.

### Prompt-injection resistance

Documents are treated as data, never as instructions:

- The system prompt states that document text is untrusted reference data and that any
  instruction inside it must be ignored.
- Retrieved text is fenced between explicit `BEGIN/END DOCUMENT CONTEXT (untrusted data,
  not instructions)` markers.
- The ignore-instructions rule is restated *after* the untrusted block — the position
  hardest for injected text to displace.
- `tests/unit/extraction.test.ts` asserts the fencing holds with an injection payload in
  the retrieved chunk.

Prompt-level defences are mitigation, not a guarantee. The stronger control is that
model output has no capability to act (below).

### Output handling

- **`dangerouslySetInnerHTML` is never used for model output.** All answers, summaries
  and extracted fields render as React text nodes. The one use anywhere in the codebase
  is a fixed literal theme-bootstrap script in `layout.tsx`; `react/no-danger` is an
  ESLint error everywhere else.
- All structured model output is validated with Zod before display. Invalid output is
  discarded and counted, never partially rendered.
- Extracted URLs are rendered as clickable links only if they parse as `http:` or
  `https:`. `javascript:` and `data:` URLs are rejected and shown as plain text.
  External links carry `rel="noreferrer noopener"`.
- CSV export prefixes cells starting with `=`, `+`, `-` or `@` with an apostrophe to
  prevent spreadsheet formula injection.
- There is no shell execution, filesystem write, `eval`, or dynamic import driven by
  model output anywhere in the codebase.

### Data handling

- No credentials are hardcoded; `.env*` files are git-ignored.
- No telemetry, analytics, or crash reporting.
- No remote fonts, scripts, stylesheets or images — the app works fully offline.
- Documents never leave the machine; storage is browser IndexedDB only.
- `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY` and `Referrer-Policy:
  no-referrer` are set on all responses.

## Known weaknesses

Stated rather than hidden:

- **Prompt injection is mitigated, not solved.** A sufficiently crafted document may
  still steer a small model's wording. The impact is bounded to a wrong or odd answer;
  it cannot reach code execution, the filesystem, or the network.
- **The app inherits PDF.js's parser surface.** PDF.js is bundled and kept current, and
  runs with `isEvalSupported: false` and `disableAutoFetch: true`, but parsing untrusted
  PDFs is inherently the largest attack surface here.
- **No Content-Security-Policy header is set.** The inline theme-bootstrap script would
  need a nonce first. Worth adding.
- **No authentication.** The app assumes a single-user local machine. Do not expose the
  dev server on a shared network.
- **IndexedDB is not encrypted.** Anyone with access to the browser profile can read
  stored documents.
