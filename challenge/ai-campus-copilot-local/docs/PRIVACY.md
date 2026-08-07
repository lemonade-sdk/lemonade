# Privacy

## Summary

AI Campus Copilot Local processes documents on your machine and sends AI requests only
to a Lemonade Server you run yourself. No cloud AI provider is involved, and the app
collects no telemetry.

## Where data actually travels

```
Your file        →  browser memory only (never transmitted)
Extracted text   →  browser  →  localhost Next.js  →  localhost Lemonade
Embeddings       →  stored in browser IndexedDB
Your questions   →  browser  →  localhost Next.js  →  localhost Lemonade
Answers          →  stored in browser IndexedDB
```

Every hop is on your own machine.

### About the Next.js hop

Requests do not go straight from the browser to Lemonade. They pass through this app's
own Next.js process first. That process runs locally — it is the `npm run dev` or
`npm run start` you launched.

It exists so `LEMONADE_SERVER_URL` and `LEMONADE_API_KEY` stay server-side and never
enter the browser bundle, and so every request is validated against a schema and an
endpoint allowlist before reaching Lemonade. It is not a remote service and it does not
persist anything.

## The uploaded file itself is never transmitted

PDF parsing happens in the browser using a bundled copy of PDF.js. Text and Markdown are
read with the browser's own file APIs. The bytes of your file are never sent to the
Next.js process, to Lemonade, or anywhere else.

What *is* sent to Lemonade is extracted text: chunks to be embedded, and the specific
retrieved passages needed to answer a question.

## What is stored, and where

Everything lives in one IndexedDB database in your browser, named
`ai-campus-copilot-local`:

| Table | Contents |
|-------|----------|
| `documents` | Filename, size, page and chunk counts, processing timings, warnings |
| `pages` | Extracted text per page or section |
| `chunks` | Chunk text and embedding vectors |
| `chats` | Your questions, the answers, citations and per-turn timings |
| `settings` | Selected chat and embedding model, active document, theme |
| `extractions` | Deadline, opportunity and summary results |
| `benchmarks` | Benchmark run results |

Nothing is written outside the browser. There is no server-side database, no file
written to disk by this app, and no sync of any kind.

## Deleting your data

- **One document:** Documents page → Delete. Cascades to its pages, chunks, chats,
  extractions and benchmarks.
- **Everything:** Privacy page → Delete all local data, or Documents page → Delete all
  local data. This also clears your model selections.
- **Browser-level:** clearing site data for `localhost:3000` removes the database.

## Models and the internet

Lemonade downloads model weights from Hugging Face or ModelScope the first time you pull
a model. That is Lemonade's behaviour, governed by Lemonade's own privacy policy, and it
happens before you use this app. Once your models are downloaded, this application needs
no internet connection at all.

## No cloud AI providers

There is no OpenAI, Anthropic, Google, Voyage AI or other cloud model provider in this
project — no dependency, no API key handling, no code path. The main AI workflow cannot
reach a cloud provider because no such client exists in the codebase.

(Lemonade Server itself has an optional experimental cloud-offload feature. This app
does not enable, configure or require it. If *you* have configured cloud providers in
your own Lemonade install, cloud-routed models appear in `GET /api/v1/models` with
`recipe: "cloud"` and could be selected on the Setup page. That would be your choice in
your Lemonade configuration, not something this app does.)

## No telemetry

- No analytics, no crash reporting, no usage pings.
- No remote fonts — the UI uses system font stacks only.
- No remote scripts, stylesheets or images.
- Errors are logged to your browser console and nowhere else.

## Your responsibility

Only upload documents you have permission to process. College notices often contain
other students' names, roll numbers or contact details. Local processing protects that
information from third parties, but it does not give you permission to process it —
check your institution's rules.

## Security

For the security posture — file validation, prompt-injection handling, output rendering
and the endpoint allowlist — see [`../SECURITY.md`](../SECURITY.md).
