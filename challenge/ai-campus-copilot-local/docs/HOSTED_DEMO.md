# Hosted demo

The app is published as a static site on GitHub Pages:

**https://jerome-prakash-l.github.io/lemonade/**

Read this before relying on it. **The local build is the supported way to run this
app.** The hosted page is a convenience for showing the interface without cloning.

## What is different about it

There is no server behind the hosted page, so the `/api/lemonade/*` proxy does not
exist. The browser calls your Lemonade Server directly at an address you enter on the
Setup page.

| | Local (`npm run dev`) | Hosted (GitHub Pages) |
|---|---|---|
| Lemonade address | `LEMONADE_SERVER_URL`, server-side | Typed into the Setup page, stored in your browser |
| Request path | browser → local Next.js → Lemonade | browser → Lemonade |
| API key support | Yes, via `LEMONADE_API_KEY` | No — there is no server to hold a secret |
| Endpoint allowlist | Yes, enforced by the proxy | No — the browser reaches Lemonade directly |
| Document detail route | `/documents/[id]` | `/documents/view/?id=…` |
| Reliability | Supported | Best-effort, see below |

Everything else is identical: the same extraction, chunking, retrieval and grounding
code ships in both builds.

## It still runs locally

The page is *served* from the internet; the *AI* is not. Your documents are parsed in
your browser and your questions go to the Lemonade Server on your own machine. No cloud
model is involved either way. GitHub serves static files and receives nothing about what
you do with them.

## Why it may not connect

Your browser must permit a page served from `github.io` to contact an address on your own
machine. Browsers restrict this, and the rules keep changing:

- `http://localhost` and `http://127.0.0.1` count as trustworthy origins, so this is
  **not** blocked as mixed content.
- Chromium's Local Network Access / Private Network Access work does gate it. Depending
  on your version you may get a permission prompt, or the request may be refused
  outright. Lemonade does not send the header that would pre-authorise it.
- Some extensions and enterprise policies block local requests regardless.

If the Setup page keeps reporting *not reachable* while Lemonade is definitely running,
this is why. **Run the app locally instead** — it is three commands and has none of these
constraints:

```bash
cd challenge/ai-campus-copilot-local
npm install
npm run dev
```

## Building it yourself

```bash
npm run build:static                       # outputs to out/
PAGES_BASE_PATH=/lemonade npm run build:static   # for a project site under /lemonade
```

`scripts/build-static.mjs` moves `src/app/api` and `src/app/documents/[id]` aside for the
build, because `output: "export"` cannot emit route handlers or un-enumerable dynamic
segments. It restores both afterwards, including on failure. If a build is killed
mid-run, the next invocation restores them before starting.

Deployment happens automatically from `.github/workflows/campus-copilot-pages.yml` on
pushes to `main` that touch `challenge/ai-campus-copilot-local/**`. The workflow runs
lint, type-check and unit tests before publishing.

## Enabling Pages on a fork

Repository **Settings → Pages → Build and deployment → Source: GitHub Actions**. The
workflow cannot publish until that is set.
