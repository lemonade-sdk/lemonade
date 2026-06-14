# Project Context

- **Project:** lemonade
- **User:** Kyle Poineal
- **Created:** 2026-05-15
- **Role:** UI / Frontend — Tauri desktop app + web app

## Core Context

Leading UI POC on `feat/ui-testing`. React stays (ROI analysis showed ~300 LOC savings for 47-file rewrite = inverted ROI). Four explicit framework-change conditions set by Lovell; do not re-litigate without them.

**Critical constraint:** Debian native packaging requires `src/web-app/` to use only `/usr/share/nodejs` modules. Kranz is source of truth.

**Detailed history (2026-05-15 to 2026-05-24):** framework evaluation, ROI re-exam, UI/UX competitive review, static prototype v1.0–v1.1, API wiring to live lemond, v1.3 presets audit + shipped. See `history-archive.md`.

## Active Learnings

### 2026-06-14: Prototype styles.css → 11 order-preserving partials (mirrors PR #2223)

Split `prototype/ui-redesign/src/styles/styles.css` (6,234 lines / 158,455 bytes, CRLF, no BOM, trailing newline) into 11 verbatim partials under `src/styles/partials/`, behind a barrel `src/styles/index.css`. This mirrors merged PR #2223, which did the same to `src/app/styles/styles.css`. Scope was strictly `prototype/ui-redesign/`; `/src` was read-only reference.

**11-partial map (original source order):** base (1–62), titlebar (63–223), chat (224–1207), model-manager (1208–2076), connect (2077–2145), buttons (2146–2254), presets (2255–2758), form-controls (2759–3217), context-rails (3218–3623), responsive (3624–5603), polish (5604–6234). The chat/model-manager cut handles a duplicate divider — both `/* ---------- Model Manager ---------- */` and `/* ========== Model Manager ========== */` (lines 1208–1209) start model-manager.css; chat.css ends on the blank line 1207.

**Order-preservation rationale (the key difference from src/app):** src/app's barrel alphabetizes because its partials are cleanly namespaced and cascade-order-independent. The prototype is NOT — it ends with a ~2,000-line `responsive.css` media-query block and a trailing `polish.css` override layer whose cascade position decides which declarations win. So the barrel imports tokens FIRST, then the 11 partials in ORIGINAL order, explicitly NOT alphabetized. The barrel header documents this so nobody "tidies" it later.

**Byte-identical verification approach (reusable):** the safest split is a deterministic PowerShell slicer, never hand-retyping CSS. Read raw text, split with `-split "(?<=`n)"`(lookbehind keeps each line's`\r\n`attached → byte-exact), slice by 1-indexed line ranges, write each partial with`[System.IO.File]::WriteAllText($p,$c, [UTF8Encoding]::new($false))`(no BOM, no appended newline). Then prove correctness by concatenating the partials' bytes in barrel order and comparing to the original byte-for-byte. Result: 158,455 == 158,455 ✅. Also confirmed per-file brace balance (all`{`==`}`) and a clean `npm run build`(webpack exit 0; css-loader resolved the`@import`chain; only pre-existing bundle-size perf warnings).`src/index.tsx`now imports a single`./styles/index.css`(tokens.css is pulled in by the barrel); grep confirmed nothing else imported`styles.css`/`tokens.css`directly. Original removed via`git rm`.

### 2026-06-05: fl0rianr follow-up audit — tool reliability and Omni wrappers

fl0rianr's recent prototype work generally keeps state client-side, but the risky pattern is optimistic UI wrapping around behaviors that still need explicit error surfaces. Tool execution has good happy-path schemas/prompts, yet failures are mostly returned to the model or silently swallowed; model/tool-call debugging needs visible logs and bounded error handling.

Omni collections are UI-only metadata, but ModelManager still exposes normal Load/Get & Load controls that call lemond with `collection.omni`. That violates the UI-only intent and fails quietly; collection actions should load/check component models instead of registering/loading the wrapper.

### 2026-06-05: Tools toggle regression — scoped state + enriched capability mismatch

Kyle's "can't turn on tools" report traced to `ChatView.tsx`: the UI renders the tools toggle from scoped local state but disabled it from loaded-model-only chat capability. fl0rianr's account scoping moved `lemonade_use_tools` to `lemonade:<scope>:use_tools`; guest should migrate legacy state, and ChatView should gate tools from the enriched `currentModelSnapshot` so custom/known/virtual model capability fixes apply consistently.

### 2026-06-01: fl0rianr UI prototype review — Omni collections, accounts, custom models

fl0rianr added the next UI POC layer in `prototype/ui-redesign/`: capability-aware composer routing, Omni collections, scoped local users, custom model registration, and persistent download polling. The work keeps the POC side-by-side and client-owned: account data, scoped conversations, presets, custom models, tool toggles, and privacy defaults are localStorage/sessionStorage state rather than lemond state.

Omni mode is implemented as a UI-level composition pattern, not a backend lifecycle change. `collection.omni` custom models reference component model names/roles; `ChatView.tsx` chooses a planner/chat component and `tools/omniTools.ts` exposes image generation/editing, TTS, transcription, and vision analysis as function tools against the selected components. This integrates with the v1.4 capability-keyed preset direction: routing is by capability labels/model snapshots, while presets remain local and staged.

### 2026-05-31: Prototype README rewritten

Updated `prototype/ui-redesign/README.md` to reflect the actual React 19 + TypeScript + webpack + Playwright stack (not the old v1.1 static HTML demo). Added sections covering prerequisites, install, dev/build/test scripts, Presets v1.4 features, project structure, real-server pointing, and troubleshooting. README now accurately describes the active POC on branch `feat/ui-testing` and links to `.squad/decisions.md` for design rationale.

### 2026-05-16–2026-05-31: Presets journey — v1.3 capability-keying → v1.4 shipped

v1.3 renamed Recipes→Presets, rekeyed from engine-list to capability-list: `preset.applies_to: [chat, image, …]`. Replaced `MODEL_ENGINES` with `MODEL_LABELS` (array of capability labels per model). Compatibility is label-intersection: `preset.applies_to.some(c => model.labels.includes(c))`. Added image presets (Sharp, Quick). 8 starters total (6 chat + 2 image). Backend hint field kept for power users (biases backend choice; Router picks final backend).

Discovered React port `PresetManager.tsx` drifted back toward backend recipes. Audited against v1.3 and identified 8 UX problems: technical terminology, missing compatibility guards, only loaded models first-class, sampling not applied.

**v1.4 shipped per Kyle's answers to 7 open questions:**

- Schema: `applies_to: Capability[]`, optional `engine_hint` (advanced), `recipe_options` for load flags
- Staged bindings: apply stores local binding, shows "Will apply on next load" (does NOT call `api.loadModel()` immediately)
- Sampling wired: `temperature`, `top_p`, `top_k`, `repeat_penalty` merged into `/api/v1/chat/completions`
- Import policy: v1.4 requires `applies_to`; legacy rejected
- Files: `presetStore.ts`, `PresetManager.tsx`, `api.ts`, `styles.css`, `tests/features.spec.ts`
- Build passes; 2 Playwright tests passing

See `.squad/orchestration-log/` for agent run summaries. See `.squad/decisions/decisions.md` for full decision trail.

**Older learnings (2026-05-15 to 2026-05-24)** archived in `history-archive.md`: framework evaluation, ROI analysis, UI/UX competitive review, static prototype v1.0–v1.1 audit, API wiring, v1.3 presets, HuggingFace integration, UI perf fixes.

### 2026-06-14: ui-redesign prototype structure audit (requested by Kyle Poineal)

**Framework:** React 19 + TypeScript 5.3, compiled by webpack 5 via ts-loader. NOT Electron. NOT Vue/Svelte/vanilla TS. Dependencies: react, react-dom, markdown-it, highlight.js, katex, dompurify, mermaid, recharts.

**Entry point:** `prototype/ui-redesign/src/index.tsx` — standard React 19 `ReactDOM.createRoot` mount on `<div id="root">`. HTML shell is `src/index.html` (no preloaded scripts; webpack HtmlWebpackPlugin injects the bundles). App root component: `src/App.tsx`.

**Webpack config:** `target: 'web'`, entry `./src/index.tsx`, output to `dist/`, `HtmlWebpackPlugin` generates `dist/index.html`. Dev server on port 8080 with HMR. Production build splits into `main.bundle.js`, `charts.bundle.js`, `markdown.bundle.js`, `vendors.bundle.js`. tsconfig targets ES2020, `jsx: 'react'`, strict mode, rootDir `./src`.

**Components:** `App.tsx` (shell, nav, connection state), `ChatView.tsx`, `ModelManager.tsx`, `PresetManager.tsx`, `BackendManager.tsx`, `Dashboard.tsx`, `LogViewer.tsx`, `ConnectView.tsx`, `Icon.tsx`, `MarkdownMessage.tsx`. Features in `src/features/accounts/`, `src/features/customModels/`, `src/features/collections/`.

**API layer:** `src/api.ts` — typed fetch-based singleton. All calls hit `/api/v1/...` lemond HTTP endpoints. Base URL stored in localStorage, defaults to `http://localhost:13305`. Streaming via fetch + ReadableStream. **No `window.api`, no Tauri `invoke()`, no tauriShim anywhere in the prototype.** Confirmed: grep for `window.api|tauriShim|invoke\(|tauri` returns zero matches.

**Surface model:** Browser-only right now. The app assumes a browser context — it uses `window.location.hash`, `localStorage`, `fetch()`, `ReadableStream`. It does NOT import from `src/app/src/` and does NOT use the existing renderer's `tauriShim.ts`. It is a fully standalone repo under `prototype/ui-redesign/`.

**Tauri integration path:** To run under Tauri, a Rust host (new `src-tauri/`) would point its `tauri.conf.json` `frontendDist` at `prototype/ui-redesign/dist/` (production) or `http://localhost:8080` (dev). The native webview would load `dist/index.html`, execute the JS bundles, and the React app would run inside the webview exactly as in a browser. Because the app talks to lemond over HTTP, it works without any `window.api` bridge at all for the current feature set. If desktop-specific features are needed later (custom titlebar drag, tray notifications, native file dialogs), a tauriShim-style bridge can be added without restructuring the React code.
