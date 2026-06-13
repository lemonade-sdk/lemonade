# Squad Decisions

## Active Decisions


> Entries older than 2026-05-16 archived to `decisions/archive/2026-05-15.md`.

### 2026-05-16T14:30:00Z: UI prototype v1.3 — Recipes → Presets, capability-keyed

**By:** Mattingly (UI / Frontend), requested by Kyle Poineal
**Scope:** `prototype/ui-redesign/` only (branch `feat/ui-testing`, do NOT merge to main)
**Status:** Implemented in prototype; defers production code paths to a future C++ / Tauri-side decision

**What was decided:**

1. **The user-facing UI word is "Preset".** "Recipe" is reserved for the codebase concept (engine id like `llamacpp` / `flm` / `ryzenai-llm`, AND OmniRouter's meta-recipe collection). "Engine" / "backend" is the UI word when we need to refer to the codebase concept (e.g., "Backend hint" field on a chat preset). The prototype prototype updates nav, headings, card copy, popover, slide-over titles, toasts, count text, and chat composer pill text.
2. **Compatibility is capability-keyed, not engine-keyed.** Presets carry `applies_to: [capability labels]` (e.g., `["chat"]`, `["image"]`). Models carry an array of capability labels (`MODEL_LABELS` in the mock). Compatibility is a label-intersection: `preset.applies_to.some(c => model.labels.includes(c))`. This matches OmniRouter's tool-based architecture — the user doesn't pick an engine, they pick a capability, and the runtime picks the backend.
3. **Starter set is 8 presets.** Chat: Balanced, Quality, Fast, Creative, Long Context, Code. Image: Sharp (steps 30 · cfg 8.0), Quick (steps 15 · cfg 7.0). Having BOTH chat and image starters is important — it visually demonstrates that capability-keying actually matters.
4. **Preset detail panel adapts to capability.** Chat presets show Context size + Backend hint (Options) plus Temperature/Top-p/Top-k/Repeat penalty (Sampling). Image presets show Steps + CFG scale (Options · per generation), with chat sampling section hidden. Toggling is keyed on `primaryCap(preset)`. `data-preset-fields="chat"` / `data-preset-fields="image"` are the toggle hooks in markup.
5. **Visual treatment for capability chips is distinct from filter chips.** A `.cap-chip` has a leading colored dot per capability (chat = accent gold, vision = lilac, image = amber, transcription/audio = teal, tts = pink, embed = blue, rerank = orange, edit = warm tan, code = green). Surface stays neutral so multiple chips in a row don't compete. The same chip is used in the preset card (read-only) and in the slide-over "Applies to capabilities" row (where `.is-on`/`.is-off` modifiers indicate state).
6. **CSS class names and most `data-recipe-*` attributes were NOT renamed.** Only the view route key (`data-view-target="presets"` / `data-view="presets"`) and the JS data shape (`engines` → `applies_to`, `config` → `options`, `MODEL_ENGINES` → `MODEL_LABELS`, `engineFor` → `labelsFor`) changed. Class names like `.recipe-card` and ids like `id="recipe-slideover"` were kept. Rationale: zero risk of breaking selectors, much smaller diff, class names are invisible to users.

**Why this matters for production (out of scope for the prototype, flagged for a future decision):**

- The mock `MODEL_LABELS` in `app.js` is hardcoded. In production, capability labels must come from the same registry that powers OmniRouter's tool registration in `lemond` — otherwise the UI and the runtime can disagree about what a model can do. **Open question:** what's the canonical source for a model's capability labels?
- `applies_to` is a flat list of labels in the prototype. **Open question:** should multi-capability presets (e.g., `["chat", "vision"]`) be allowed in production, and if so, do they "fan out" (chat-side settings to chat-only models, vision-side settings to vision-capable models) or are they treated as a single bundle? The prototype treats them as a single bundle.
- The chat preset's "Backend hint" field exists in the prototype. **Open question:** does a production preset hint the backend, or does the runtime always own the backend selection, with the user influencing it only via per-model load preferences (separate from presets)?
- Composition with OmniRouter meta-recipes is undefined. **Open question:** if OmniRouter has a "research" meta-recipe that fans out chat + embedding + reranking, does a user-defined "Long Context" preset also apply when the meta-recipe touches a chat-capable model? Need a layering model.

**Reversibility:** The prototype is on `feat/ui-testing`, not main. The rename is fully reversible by `git revert`. The data-model rekey (`engines` → `applies_to`) is internal to the prototype's mock data — no API / persisted schema is affected.

**Files changed:**
- `prototype/ui-redesign/index.html`
- `prototype/ui-redesign/app.js`
- `prototype/ui-redesign/styles.css`
- `prototype/ui-redesign/tokens.css` — UNCHANGED



### 2026-05-16T20:00:00Z: PR #1914 review-feedback fixes applied (bug-report template)
**By:** Kranz (Build & Release)
**Requested by:** Kyle Poineal
**Branch:** `fix/1885-bug-report-template-commands` (pushed; was `b76482e4`, now `976a8260`)
**PR:** https://github.com/lemonade-sdk/lemonade/pull/1914 — addresses CHANGES_REQUESTED from @jeremyfowers

**What changed in `.github/ISSUE_TEMPLATE/bug-report.yml`:**

1. **"Steps to Reproduce" placeholder (line 75 of pre-fix file)** — Removed the first step `1. Start the server with \`lemond\`` entirely, renumbered the remaining steps. New placeholder now starts at "1. Load model X with `lemonade run <model>`".
2. **"How to collect logs" block (line 103 of pre-fix file)** — Replaced `lemond --log-level debug` (with prose "Start (or restart) the server with debug logging:") with `lemonade config set log_level=debug` (with prose "Set the server to debug logging:").
3. **Diagnostic commands list (line 115 of pre-fix file)** — `lemonade recipes` → `lemonade backends`.

**Verified CLI syntax (cited):**
- `lemonade config set KEY=VALUE` is the canonical runtime config setter. Defined in `src/cpp/cli/main.cpp:1041-1044`. The subcommand uses `allow_extras(true)` to take arbitrary `key=value` tokens.
- Log-level key is `log_level` (snake_case). Confirmed in `src/cpp/server/runtime_config.cpp:351` and `src/cpp/server/config_file.cpp:187`.
- Hot reconfiguration confirmed via `reconfigure_application_logging()` in `src/cpp/server/logging_config.cpp:137` — no server restart needed.
- `lemonade backends` confirmed as current command in `src/cpp/cli/main.cpp:1031`.

**Judgment call on Jeremy's comment 1 (line 75):** Removed the lemond step entirely per Jeremy's lean ("could just remove this"). Did NOT add platform-specific "how to restart lemond on each OS" instructions. Rationale: bug-report template should be lean; restart instructions would clutter it and most users won't need a fresh server to reproduce a load-and-prompt bug. If platform restart docs are needed, that's a follow-up — either a separate FAQ entry in `docs/` or a section in `docs/server/`, NOT another row in the bug-report template.

**Follow-up work (potentially separate PR):**
- If maintainers want platform-restart-lemond instructions discoverable, propose adding them under `docs/server/` (e.g., "Restarting the server" page) and linking from the bug-report template's logs section. NOT required to land this PR.
- No build/CI/packaging implications from this change. The template is consumed only by GitHub's issue form renderer.

**Workflow confirmation:**
- Stashed nothing — working tree had only `prototype/` untracked, as expected (excluded by `.git/info/exclude`).
- Switched to `fix/1885-bug-report-template-commands`, pulled (already up to date), applied edits, validated YAML via `npx js-yaml` (passed).
- Committed only `.github/ISSUE_TEMPLATE/bug-report.yml` (commit `976a8260`).
- Pushed to `origin/fix/1885-bug-report-template-commands`.
- Returned to `feat/ui-testing`; prototype/ files still untracked.
- No `.squad/` files staged or committed on the PR branch (excluded per `.git/info/exclude`).

---

---

# Decision: API wiring architecture for UI prototype

**Author:** Mattingly  
**Date:** 2026-05-23  
**Status:** Implemented  

## Context

The static prototype at `prototype/ui-redesign/` needed to be wired to live lemond
HTTP endpoints. The prototype is vanilla JS (no build step) and must degrade
gracefully when the server is offline.

## Decision

Created a separate `api.js` file that exposes a `window.LemonadeAPI` singleton with:
- Connection management (connect, poll, status tracking)
- All endpoint wrappers (health, models, load, unload, pull, chat/completions, system-info)
- SSE streaming via `fetch()` + `ReadableStream` (not `EventSource`, which only supports GET)
- Base URL and API key persisted in `localStorage`

`app.js` consumes the singleton and dynamically renders model zones, chat messages,
status indicators, and the model selector from live data.

## Key constraints honored

1. **Presets stay client-side** — no preset API calls (invariant #11).
2. **No files outside `prototype/ui-redesign/` touched.**
3. **Graceful degradation** — disconnected state shows clean messages, not errors.
4. **No build step** — both files are vanilla JS IIFEs loaded via `<script>` tags.

## Endpoints used

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/api/v1/health` | GET | Connection test, loaded models |
| `/api/v1/models?show_all=true` | GET | Full model registry |
| `/api/v1/load` | POST | Load a model |
| `/api/v1/unload` | POST | Unload a model |
| `/api/v1/pull` | POST | Download with SSE progress |
| `/api/v1/chat/completions` | POST | Streaming chat |
| `/api/v1/system-info` | GET | Backend/device info |

## Impact on other agents

None — all changes are contained in `prototype/ui-redesign/`. No server, backend,
or packaging files were modified.
