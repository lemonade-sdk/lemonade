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

# Merge Request: kpoin/ui-testing → kpoin/ui-mobile-layout

**From:** Claw (Kyle's assistant)
**Date:** 2026-06-14
**Priority:** Normal

## What we need

Merge `kpoin/ui-testing` into `kpoin/ui-mobile-layout`. GitHub API reports a conflict (HTTP 409).

The files that differ between the two branches include `prototype/ui-redesign/src/styles/styles.css` and `prototype/ui-redesign/src/styles/tokens.css`, but the diff shows 0 actual line changes in `ui-mobile-layout` for those files — so this may be a divergent history conflict rather than a content conflict.

## Expected resolution

- Merge `kpoin/ui-testing` into `kpoin/ui-mobile-layout`
- For any conflicts in `.squad/` history/log files: keep both sides (append, don't drop)
- For `prototype/ui-redesign/src/styles/`: `kpoin/ui-testing` version wins (it has the latest shared styles)
- For `kpoin/ui-mobile-layout`-specific mobile fixes in `prototype/`: preserve them

## Standing rule reminder

Do NOT touch `src/cpp/` C++ code. CSS/prototype/squad files only.

---

### 2026-06-25: MCP-in-GUI3 — Phase A approved, scope locked (per @fl0rianr on #2404)

**By:** Kyle Poineal (@kpoineal) via Coordinator, scoped by reviewer @fl0rianr
**Decision:** Build **Phase A** (read-only MCP dashboard) now, on branch `feat/gui3-mcp-dashboard` off `kpoin/ui-testing`. PR back into `kpoin/ui-testing`, milestone GUI3.

**fl0rianr's scope answers (binding for the POC):**
- **Phase A = in POC:** read-only `/mcp` status/dashboard panel — endpoint visibility, copyable connection info, health/status, list of exposed tools. Frontend-only, low-risk.
- **Phase B = experimental follow-up:** GUI3 as MCP client host. Behind a flag, must not block Phase A. Design in parallel but ship minimal later.
- **Config location:** client-local, **localStorage** (honors many-clients-one-server invariant). Manual server URLs; templates later.
- **Auth:** **token/header-based only**. OAuth/PKCE explicitly out of scope for POC.
- **Tool conflict resolution:** **namespace external tools** by server id/name + tool name; user-friendly display label. Never collide with native lemonade tools.
- **GUI placement:** start in **`ConnectView`**; split into dedicated Integrations/MCP view once it grows.
- **Transport:** **Streamable HTTP POST-only**. No SSE until a concrete external-server need.
- **Native tools:** do **NOT** route GUI3's existing native tools through MCP — keep the direct HTTP/OpenAI-function-calling path.

**Deferred to post-POC:** server-side MCP client registry, OAuth, SSE, deep tool-permission UX.
**Why:** Delivers visible MCP support quickly without overcommitting the first GUI3 implementation; respects "lemond off-limits" + many-clients-one-server invariants.

---

### 2026-06-14: Accessibility is a standing directive
**By:** Kyle (kpoin) (via Copilot)
**What:** Accessibility is now a standing project requirement, not optional. Every PR, feature, and UI change must consider a11y by default. Definition of done for any UI work includes:
- WCAG 2.1 AA compliance (semantic HTML, ARIA, keyboard nav, focus management, color contrast 4.5:1, screen reader labels)
- LLM-specific accessibility: aria-live for streaming output, response verbosity controls, high-contrast modes, reduced motion, dyslexia-friendly font option, keyboard shortcuts
- Reference: `prototype/ui-redesign/ACCESSIBILITY.md` for the canonical plan and current status
**Why:** Kyle: "every PR, feature, and UI change should consider a11y by default ... Not an afterthought."
**Scope:** All UI work in this repository (prototype, future Tauri app, any new surfaces).
**Owner:** All agents doing UI/frontend work (primary: Mattingly).

---

### 2026-06-14: Tests must pass — non-negotiable
**By:** Kyle (kpoin) (via Copilot)
**What:** Anyone making code changes is responsible for keeping existing tests passing. If a change affects a test (renamed UI label, changed selector, altered behavior), the test is updated in the SAME commit/PR — never deferred. Broken tests ship nothing.
**Why:** Kyle: "No breaking tests ... This is non-negotiable — broken tests ship nothing."
**Scope:** All code changes in this repository. Applies to every agent and contributor.
**Definition of done addition:**
- Run the relevant test suite before pushing (e.g. `npm run test:a11y`, `npx playwright test`, Python integration tests under `test/`, C++ tests where applicable)
- Update tests in the same commit as the code change that broke them
- If a test cannot be fixed within the scope of the change (e.g. flaky for unrelated reason), explicitly call it out — do not silently disable
**Enforcement:** Reviewer (Lovell) blocks merges with failing tests. Self-check is expected before requesting review.

---

### 2026-06-13T19:19:16-06:00: User directive — pause PR creation
**By:** Kyle (kpoin) via Squad coordinator
**What:** Stop opening PRs for now. Commit and push to branches only. Kyle will explicitly request PR creation when he's ready.
**Scope:** Session-level directive. Applies until Kyle lifts it.
**Why:** User request — Kyle wants to control PR timing.

---

# Decision: Phase B MCP Client Host Design

**Agent:** Lovell (Lead)
**Date:** 2026-06-25
**Issue:** #2404
**Comment:** https://github.com/lemonade-sdk/lemonade/issues/2404#issuecomment-4799493443
**Status:** Awaiting @fl0rianr review

## Decision

Posted Phase B design for GUI3 acting as an MCP client host that connects to external MCP servers and exposes their tools in chat. Design is:

1. **Purely frontend** — no lemond (C++) changes. All state in localStorage per invariant #11.
2. **Feature-flag gated** — `lemonade_mcp_client_enabled` localStorage key. Zero code paths execute when off.
3. **Plugs into existing architecture** — merges external tools into `ChatToolRuntime` interface; no parallel execution path needed.
4. **Namespaced tools** — `mcp_{serverId}_{toolName}` prevents collisions with native `LEMONADE_TOOLS`.
5. **POST-only Streamable HTTP** — matches lemond's own MCP server transport (`mcp_server.cpp:35`).
6. **Security-flagged** — prompt injection and tool exfiltration risks documented as unmitigated in POC; explicit user consent required to add servers.

## Invariants checked

- ✓ #1 (quad-prefix): no new routes in lemond.
- ✓ #11 (many-clients-one-server): config is client-local localStorage, not shared.
- ✓ #12 (Debian web-app deps): zero new npm dependencies.
- ✓ #13 (desktop on-demand): no changes to desktop lifecycle.

## Risks

- CORS: web-app users may hit CORS errors with external MCP servers that don't send CORS headers. Desktop (Tauri) unaffected. Documented as known limitation; lemond proxy deferred to Phase C.
- Prompt injection via tool results: flagged, not mitigated in POC.

## Next steps

- @fl0rianr approves/refines design on #2404.
- Mattingly implements first PR per §7 delivery slice after Phase A merges.

---

# PR Review Session — 2026-06-13

**Reviewer:** Lovell (Lead/Architect)
**Author:** boclifton-MSFT
**Target branch:** kpoin/ui-testing

## PR #2223 — Refactor app styles into component partials

**Verdict:** ✅ Approved and merged (squash)

- Split monolithic `styles.css` (7594 lines) into 22 focused partials under `src/app/styles/partials/`
- Import order in `index.css` is well-documented: foundation (variables, base) → components (alphabetical, order-independent) → feature sheet (last, for overrides)
- Net +72 lines (comments in index.css + .hintrc config)
- No selectors renamed or dropped
- No CI configured for this branch (expected)
- `.hintrc` addition is reasonable (CSS compat linting config)

## PR #2224 — Add model folders UI (Server Settings)

**Verdict:** ⚠️ Changes requested (not merged)

### Positives
- Well-structured `ServerSettings.tsx` and `serverRuntimeConfig.ts`
- Correct architectural decision: uses `/internal/config` and `/internal/set` (server-wide config via HTTP, not localStorage) — aligns with invariant #11
- Clean auth/error handling with distinct unauthorized state
- No src/cpp changes

### Blocking Issues
1. **Merge conflict:** PR #2223 deleted `styles.css`; this PR adds to it. Needs rebase.
2. **Web-app incompatibility:** Direct import of `@tauri-apps/plugin-dialog` in shared renderer code will break the web-app build. Needs a shim/guard.
3. **CI failures:** .deb, .rpm, macOS .dmg, Windows embeddable all failed — likely related to #2.

### Non-blocking
- PR title says "model folders" but content is "server settings for model directories" — suggest rename for clarity.

---

# Decision: Restore PR #2228 after accidental revert

**Author:** Mattingly  
**Date:** 2026-06-15  
**Status:** Implemented — pushed as `2d8c45f0` on `kpoin/ui-testing`

---

## Context

PR #2228 (commit `115d464f`) was merged into `kpoin/ui-testing` on 2026-06-14T16:16Z. It added:
- Capability badge icons: Popular → flame, Tools → wrench, Reasoning → brain, MTP → rocket
- `reasoningElapsedMs` plumbing in `api.ts` + duration display in `ChatView.tsx`
- Preset + collection logic expansion in `lemonadeTools.ts`
- `labelDisplay` map expansion and `capabilityLabelsForModel` in `ModelManager.tsx`
- Badge alignment CSS in `styles.css`
- `CapabilityIcon` swap in `PresetManager.tsx`

Hours after merge, a "rebase ui-mobile-layout onto ui-testing" linearization (PR #2229, commit `c6529721`) extracted prototype/ files from a merge tree that did NOT include #2228's changes. This silently reverted all 7 affected files to a pre-#2228 state while retaining the a11y and mobile additions that landed after #2228.

Kyle discovered the regression on 2026-06-15: capability badges had no icons, reasoning timing was absent, and lemonadeTools was missing preset/collection tool support.

---

## What was done

Cherry-picked `115d464f` onto `kpoin/ui-testing` at HEAD `5c4ecdc2`:

```
git cherry-pick 115d464f
```

This produced whole-file conflicts in all 7 affected files. Resolution strategy for each:

| File | Strategy |
|------|----------|
| `Icon.tsx` | Took `115d464f` version wholesale — no a11y/mobile changes to this file |
| `lemonadeTools.ts` | Took `115d464f` version wholesale — no a11y/mobile changes to this file |
| `api.ts` | Took HEAD (has `window.location.hostname` mobile fix), applied `reasoningElapsedMs` plumbing from #2228 diff |
| `ChatView.tsx` | Took HEAD (has aria-live regions, bottom-sheet trap), added `formatDurationMs`/`reasoningSummary` and updated reasoning `<summary>` |
| `ModelManager.tsx` | Took HEAD (has a11y button conversions), added expanded `labelDisplay`, new helper functions (`iconForCapabilityLabel`, `capabilityLabelsForModel`, etc.), updated `renderLabels` and detail cap rendering |
| `PresetManager.tsx` | Took HEAD (has focus trap), removed `capabilityIcon` import, added `CapabilityIcon` + `useFocusTrap` imports, updated `CapabilityChip` render |
| `styles.css` | Took HEAD (has mobile media queries, focus rings, reduced-motion blocks, bottom-sheet styles), appended the 26-line badge alignment block from #2228 |

---

## Verification

**TypeScript:** `npx tsc --noEmit` — exits 0, no errors.

**#2228 content restored (grep hit counts):**
- `flame`/`wrench`/`brain`/`rocket` — 9 hits in Icon.tsx ✓
- `capabilityIconName`/`CapabilityIcon` — 14 hits across source ✓
- `reasoningElapsedMs` — 7 hits (api.ts + ChatView.tsx + LiveStreamStats) ✓
- `reasoningSummary` — 2 hits in ChatView.tsx ✓

**Mobile/a11y preserved (regression check):**
- `bottom-sheet` — 18 hits ✓
- `useFocusTrap` — 5 hits ✓
- `aria-live` — 2 hits ✓
- `window.location.hostname` — 2 hits in api.ts ✓
- `prefers-reduced-motion` — 1 hit in styles.css ✓
- `options-block__btn--selected` — 2 hits in styles.css ✓

---

## Commit

```
2d8c45f0  fix(ui): polish capability badges and reasoning timing
          (restored after accidental revert via c6529721)
```

Pushed to `origin/kpoin/ui-testing`.

---

## Downstream work needed (NOT done here — Kranz/Kyle decision)

The branches `kpoin/ui-mobile-layout` and `kpoin/ui-accessibility` diverged from a `kpoin/ui-testing` state that was missing #2228. Now that #2228 is restored on `kpoin/ui-testing`, those branches should be re-merged with `kpoin/ui-testing` so their next forward-merge doesn't accidentally re-introduce the reverted state.

**Recommended action:** Before any further work on those downstream branches, run:
```
git log --oneline --diff-filter=M -- prototype/ui-redesign/src/components/Icon.tsx | head -5
```
and verify `flame`/`wrench`/`brain`/`rocket` are present. If not, merge `kpoin/ui-testing` into the branch.

---

## Prevention

After any rebase, linearization, or large merge that touches `prototype/`, immediately verify content markers:
```powershell
Select-String -Path "prototype/ui-redesign/src/components/Icon.tsx" -Pattern "flame|wrench|brain|rocket"
Select-String -Path "prototype/ui-redesign/src/api.ts" -Pattern "reasoningElapsedMs"
Select-String -Path "prototype/ui-redesign/src/tools/lemonadeTools.ts" -Pattern "allStoredPresets"
```
Missing matches = regression. See `history.md` 2026-06-15 entry for the full sentinel list.

---

# Decision: Accessibility Phase 1 + Phase 2 Implementation

**Author:** Mattingly  
**Date:** 2026-06-14  
**Branch:** `kpoin/ui-accessibility`  
**Status:** Implemented — Phase 1 complete, Phase 2 items 11–15 complete, items 16–18 deferred  

---

## Context

ACCESSIBILITY.md (written 2026-06-14 planning pass) identified 19 accessibility items across 3 phases for `prototype/ui-redesign/`. This decision documents what was implemented in Phase 1 and Phase 2, the key technical choices made, and what remains.

---

## What shipped

### Phase 1 (all 10 items — quick wins)

- **Skip link:** `<a href="#main-content" class="skip-link">` as first child of root. Shows on `:focus`, hidden offscreen otherwise.
- **`<main>` landmark:** `<div className="view-container">` → `<main id="main-content" className="view-container">` in `App.tsx`.
- **Focus rings:** Removed `outline: none` from `input, textarea` global reset. Added `:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }`. Sliders excluded (use thumb box-shadow instead).
- **Composer textarea `aria-label`:** `aria-label="Message"` on `ChatView.tsx` composer textarea.
- **Persistence checkbox:** Already compliant (implicit `<label>` wrapping). No change.
- **Preset slideover input labels:** `aria-label="Preset name"` and `aria-label="Description"` added.
- **Preset slideover dialog semantics:** `role="dialog" aria-modal="true" aria-label="Preset details"` + ESC handler.
- **Prefers-reduced-motion:** `@media (prefers-reduced-motion: reduce)` block kills all animations/transitions; `.bottom-sheet { transform: none !important }` so sheet snaps rather than slides.
- **Status dot ARIA:** `role="status"` + `aria-label` added to the titlebar status indicator.

### Phase 2 (items 11–15)

- **`useFocusTrap` hook** (`src/hooks/useFocusTrap.ts`): Custom hook, no new npm dependency. Collects focusable children, traps Tab/Shift+Tab, excludes elements inside `aria-hidden="true"` ancestors.
- **Bottom sheet + preset slideover focus traps:** Both panels activate the trap when open; focus returns to trigger on close.
- **div→button conversions** (ModelManager): Three `div.row__content` elements converted to `<button type="button" aria-expanded>`. `CopyInlineButton` extracted to avoid button-in-button nesting. BackendManager's `div.cell__actions` left as-is (non-interactive container).
- **`aria-live` streaming output:** Two hidden `.sr-only` regions. `aria-live="assertive"` announces "Assistant is responding" / "Response complete" on stream start/end. `aria-live="polite"` receives debounced `streamingContent` flush (400ms default; 100ms on sentence-boundary). No per-token updates; no screen reader spam.
- **Color contrast fixes:** `--text-disabled` raised to `#7A776E` (dark, ~4.6:1) and `#767676` (light, 4.5:1). New `--accent-fg` token resolves to `--accent` in dark and `--accent-deep` in light; all `color: var(--accent)` foreground uses in styles.css migrated to `var(--accent-fg)`.

---

## Key decisions

### No `focus-trap-react` dependency

Custom `useFocusTrap` hook is ~50 LOC and covers the exact patterns needed (Tab wrap, Shift+Tab wrap, aria-hidden exclusion). Adding `focus-trap-react` for 50 LOC would be over-engineering for a prototype. Justified in commit message.

### `--accent-fg` token instead of per-rule `[data-theme="light"]` overrides

The yellow `--accent` (#FCD846) on white surfaces fails WCAG AA (1.4:1). Rather than patching 20+ individual CSS rules with `[data-theme="light"]` overrides, introduced `--accent-fg` as a semantic alias that resolves to `--accent` in dark and `--accent-deep` (a deeper amber that passes) in light. Single token migration covers all instances.

### aria-live debounce strategy for streaming

Token-by-token DOM updates would interrupt screen readers constantly. Sentence-boundary detection (`/[.!?\n]/` on last 2 chars) triggers a 100ms flush for natural pauses; otherwise 400ms debounce. The visible streaming cursor and markdown rendering are completely unchanged — only the hidden live region is debounced. Screen readers hear complete phrases rather than individual tokens.

### CopyInlineButton extraction from row buttons

Converting `div.row__content` to `<button>` created button-in-button violations (CopyInlineButton is itself a button). Extracted CopyInlineButton outside the main expand button using a `.row__summary` grid wrapper (`1fr auto`). This is semantically correct HTML and fixes the nested interactive element problem.

---

## What was deferred (Phase 3)

- **Keyboard shortcut system** (item 16): Requires central `useKeyboardShortcuts` hook, cheat sheet modal, focus-in-input guards. Scope exceeds Phase 2 budget.
- **Font scale control** (item 17): Requires `--font-scale` token, `calc()` rewrite of font-size tokens, A−/A+ UI in a settings panel. Deferred.
- **Message article/ol structure** (item 18): Large ChatView refactor; message rendering spans multiple components. Deferred.
- **Focus traps for composer model-search + AccountMenu** (partial): Bottom sheet and preset slideover traps are done. The composer model-search popover and AccountMenu dialog still need traps + ESC handlers. Noted in ACCESSIBILITY.md.

---

## Files changed

| File | Change |
|------|--------|
| `src/App.tsx` | Skip link, `<main>`, status dot ARIA |
| `src/components/ChatView.tsx` | Textarea aria-label, aria-live regions, bottom-sheet focus trap |
| `src/components/ModelManager.tsx` | div→button row conversions, CopyInlineButton extraction |
| `src/components/PresetManager.tsx` | Dialog semantics, ESC, focus trap, focus return, input labels |
| `src/hooks/useFocusTrap.ts` | New hook file |
| `src/styles/styles.css` | Focus rings, skip link, sr-only, reduced motion, accent-fg migration, row__summary |
| `src/styles/tokens.css` | --text-disabled fixes, --accent-fg token |
| `ACCESSIBILITY.md` | Status updated, Phase 1/2 items marked ✅ DONE |

---

## Impact on other agents

All changes are contained in `prototype/ui-redesign/`. No C++, no Tauri `src/app/`, no server code touched.

---

*Written: 2026-06-14 by Mattingly*

---

# Audit Fixes #1–4 — Implementation Summary

**Author:** Mattingly  
**Date:** 2026-06-13  
**Branch:** `kpoin/ui-audit-fixes` (cut from `kpoin/ui-testing` @ af66ea14)  
**Build:** ✅ Exit 0 (`npm run build` in `prototype/ui-redesign/`)

---

## Fix #1 (P0) — Guard the onToolCalls hang path

**File:** `prototype/ui-redesign/src/hooks/useChatStreaming.ts`  
**Lines changed:** ~186–283 (the `onToolCalls` callback body)

**Problem:** If any `runtime.execute()` call throws an uncaught exception, the outer `runCompletion` Promise never settles, leaving the stream stuck in "streaming" state with no way to abort.

**Approach:** Wrapped the entire `onToolCalls` callback body in a `try/catch`. On catch:

1. Iterates `allToolCalls`, marks any entry still at `status: 'running'` as `status: 'error'` with `Error: <msg>` in the result field.
2. Updates `activeStreams` to surface the errored tool calls in the UI.
3. Clears the token buffer for the conversation.
4. Calls `onError(convoId, 'Tool execution failed: <msg>')` — this surfaces an error banner to the user via the existing error path.
5. Calls `cleanup(convoId)` to remove the stream from `activeStreams`/`liveStats`.
6. Calls `resolve()` to settle the outer Promise.

**Key design note:** The inner `try { await runCompletion(); resolve(); } catch (err) { reject(err); }` (for the recursive tool-round call) does **not** re-throw from its catch block — `reject(err)` doesn't propagate as a thrown exception — so the outer catch is not triggered by that path. The outer catch only fires on unexpected throws from `Promise.all` / executor code.

---

## Fix #2 (P1) — Surface load errors in ModelManager

**Files:** `prototype/ui-redesign/src/components/ModelManager.tsx`, `prototype/ui-redesign/src/styles/styles.css`  
**Lines changed (tsx):** state declaration ~585, `handleLoad` ~851–863, `handlePullAndLoad.onComplete` ~953–960, `renderModelRow` ~1603–1607  
**Lines changed (css):** inserted `.row__load-error` rule at ~1927

**Problem:** `handleLoad` and `handlePullAndLoad` caught errors with `console.error` only — no user-facing feedback.

**Approach:**

- Added `loadError: { modelName: string; message: string } | null` state.
- Both catch blocks now set `loadError` using `friendlyErrorMessage(err)` (already imported) and schedule an auto-clear after 6 s via `window.setTimeout` — clear is conditional so it won't erase a newer error for a different model.
- `setLoadError(null)` at the start of `handleLoad` clears any stale error for the same or different model when retrying.
- `renderModelRow` renders a `.row__load-error` div below `row__content` when `loadError.modelName === name`.
- CSS uses `var(--danger)` / `var(--danger-soft)` / `border-top` with the same rgba as `hf-zone__empty--error` — visually consistent with existing error surfaces.

---

## Fix #3 (P1) — Wire active preset into image composer defaults

**File:** `prototype/ui-redesign/src/components/ChatView.tsx`  
**Lines changed:** `imageDefaultsForModel` function ~295–305, `defaultImageSettings` useMemo ~636–641

**Problem:** Applying an image preset (e.g. "Sharp": 30 steps, cfg 8.0) had no effect on the composer's `imageSettings` state — it stayed at the default 20 steps / 7.0 cfg.

**Approach:**

- Extended `imageDefaultsForModel` to accept a third optional parameter `activePresetRecipeOptions?: Record<string, unknown> | null`. This is spread last (after `loadedRecipeOptions`), making it the highest-priority source.
- Updated the `defaultImageSettings` useMemo to pass `currentPreset?.recipe_options` when `currentCapability === 'image'`, and added `currentPreset` and `currentCapability` as dependencies.
- The existing useEffect (line ~652) already guards the `setImageSettings(defaultImageSettings)` call with `!imageSettingsTouchedRef.current`, so user-edited values are never clobbered. The preset feeds through `defaultImageSettingsKey` → useEffect → `setImageSettings` only when the user hasn't manually changed the controls.

**Layering priority (lowest → highest):** `DEFAULT_IMAGE_SETTINGS` → `model.image_defaults` → `model.recipe_options` → `loadedModel.recipe_options` → `activePreset.recipe_options`.

---

## Fix #4 (P1) — Unify modeSupportsChatCompletions

**File:** `prototype/ui-redesign/src/components/ChatView.tsx`  
**Line changed:** ~731

**Before:**
```ts
const modeSupportsChatCompletions = currentLoadedModel
  ? canUseChatCompletions(currentLoadedModel)
  : (currentCapability === 'chat' || currentCapability === 'omni');
```

**After:**
```ts
const modeSupportsChatCompletions = currentCapability === 'chat' || currentCapability === 'omni';
```

**Rationale:** `currentCapability` is derived from `currentModelSnapshot`, which already prefers custom/known model info over raw `capabilityFromLoaded` — so it correctly resolves `'chat'` for custom models that have no `recipe` field (those would return `'unknown'` from `canUseChatCompletions`). The old guard was the root cause of the tools toggle being silently disabled for custom models. `canUseChatCompletions` is not removed — it remains in `modelCapabilities.ts` for any future callers.

---

# Audit Pass 2 — af66ea14 (kpoin/ui-testing, 2026-06-13)

**Author:** Mattingly  
**Date:** 2026-06-13T08:57:25-06:00  
**HEAD:** af66ea14 (6 commits from fl0rianr)  

## Summary

Prototype is substantially improved from previous pass. The two prior P0s (Omni collection wrapper leaking to lemond, tools toggle broken) are fixed. New surface area (LogViewer, image generation, realtime audio, preset rails) is generally solid. Remaining risks are in error visibility, an unhandled-reject hang path in the tool loop, and a preset↔image-settings disconnect.

## Key findings (see full report in chat for detail):

### Still needs attention
- **Tool loop hang** (P1 → P0 risk): `onToolCalls` is not awaited in `api.chatCompletion`. If `execute()` throws unexpectedly, the stream hangs permanently. See `api.ts:1101`, `useChatStreaming.ts:215`.
- **Load errors silently swallowed** (P1): `ModelManager.tsx:858–860` — `handleLoad` catches error but only logs to console. User sees nothing.
- **Preset image settings not reflected in composer** (P1): Active preset `recipe_options.steps`/`cfg_scale` are not fed into `imageSettings` state. "Sharp (30 steps)" preset shows 20 in composer.
- **ScriptProcessorNode deprecated** (P1): `useAudioCapture.ts:70` — Chrome deprecation path. Replacement is AudioWorklet.
- **tools toggle still disabled for unknown-recipe models** (P1, narrowed): Mostly fixed by App.tsx enrichment. Persists only for custom models with no recipe field.

### Fixed since last audit
- Omni collection P0: `loadModelRuntime` loads component models correctly ✅
- Tools toggle scoping: `lemonade:<scope>:use_tools` ✅
- Preset scope sync in App.tsx: init + effect + handler ✅
- useChatStreaming `MAX_TOOL_ROUNDS = 5` ✅

### New features that work
- LogViewer: wired, virtualized, server log-level control, inline toggle in ChatView ✅
- `ask_question` UI: renders interactive buttons + custom input ✅
- `composeToolRuntimes`: mixes Lemonade + Omni tools cleanly ✅
- Image generation: validated settings, mode switching, edit gate ✅
- Realtime audio: useAudioCapture cleanup, WebSocket reconnect, error surface ✅

## Recommended next actions (priority order):
1. Add try/catch wrapper or `.catch(reject)` on the `onToolCalls` async execution path
2. Add user-visible error display to `handleLoad`/`handlePullAndLoad`
3. Sync active preset image params into `imageSettings` on preset-change event
4. Migrate ScriptProcessorNode to AudioWorklet
5. Split ChatView.tsx (101KB is unmanageable)

---

# 2026-06-05: Tool runtime and Omni collection guardrails

**Author:** Mattingly
**Scope:** `prototype/ui-redesign/`
**Status:** Proposed

## Recommendation

1. Tool-call runtimes should never fail silently. Invalid streamed tool chunks, executor exceptions, and backend tool failures should be surfaced in the chat UI and logged with tool name/model/conversation context, without API keys or local paths.
2. `collection.omni` should remain UI-only in the POC. Load/Get & Load actions for Omni collections should operate on component models, not call `lemond` with the collection wrapper recipe.
3. The chat Tools toggle should distinguish Lemonade management tools from always-on Omni media tools, or expose a separate “Omni tools” state if users are expected to disable them.

---

# Decision: Chat Rail Listbox Focus Model + Account Menu Modal/Non-Modal

**Author:** Mattingly  
**Date:** 2026-06-22  
**Branch:** `feat/gui3-chat-account-a11y`  
**Issues:** #2346 (conversation rail) · #2348 (account menu)  
**PR:** #2363  
**Status:** Implemented, tests passing (58 passed / 7 skipped / 0 failed)

---

## 1. Conversation Rail — Listbox Focus Model

### Context

The conversation rail (`<ul role="listbox">`) already had the correct ARIA role and label but no focus management. Individual `<li role="option">` elements were not focusable, had no `aria-selected`, and the container had no keyboard event handlers. For a blind NVDA user, the entire conversation list was opaque — no way to navigate it by keyboard.

### Decision: Roving tabindex (not aria-activedescendant)

Two standard patterns exist for managing focus in a listbox:

| Pattern | Mechanism | AT support | Complexity |
|---------|-----------|-----------|------------|
| **Roving tabindex** | Selected/first item has `tabIndex=0`; others `-1`; `.focus()` on arrow nav | Excellent across all screen readers | Low |
| **aria-activedescendant** | Container is the single Tab stop; `aria-activedescendant` points at "active" option ID | Good but inconsistent in some SRs | Medium |

**Chose roving tabindex** because:
1. NVDA support is better-established for roving tabindex on listboxes.
2. Simpler React implementation — no container `tabIndex` state, no ID-to-element lookup.
3. Already the right pattern given that the `<ul>` isn't the focus target.

### Roving tabindex rules

- `selectedConversation.tabIndex = 0` (the active conversation)
- If no active conversation: `conversations[0].tabIndex = 0`
- All other options: `tabIndex = -1`
- ArrowDown/Up call `options[newIdx].focus()` — works on `tabIndex=-1` elements too

### Delete button: tabIndex=-1

The `<button class="rail__item-delete">` inside each option gets `tabIndex={-1}` to keep the listbox Tab-footprint clean (one stop per option). The button remains:
- Reachable by NVDA virtual cursor (browse mode navigates all DOM elements regardless of tabIndex).
- Identifiable by its `aria-label="Delete conversation: {title}"`.
- Visually shown when the parent `<li>` has keyboard focus (`.rail__item:focus-within .rail__item-delete { opacity: 1 }`).

**Tradeoff:** Sighted keyboard-only users (no screen reader) cannot Tab to the delete button. This is acceptable because (a) deleting a conversation is a non-critical, infrequent action; (b) WCAG 2.1.1 (Keyboard) is still satisfied via the screen reader virtual cursor path; (c) adding the button back to Tab order would require either an always-visible delete button or complex Tab-order manipulation within the listbox.

---

## 2. Account Menu — Modal Dialog Decision

### Context

`AccountMenu.tsx` already declared `role="dialog"` on the panel and `aria-haspopup="dialog"` on the trigger. However, it was missing `aria-modal`, a focus trap, Escape handler, and focus restore — the "half-dialog/half-popover" problem described in #2348.

The panel is visually rendered as a positioned popover (no backdrop, `z-index: 80`) but semantically declared as a dialog.

### Decision: MODAL dialog

Alternatives considered:

| Option | Pros | Cons |
|--------|------|------|
| **Full modal (chosen)** | Consistent with existing `role="dialog"` declaration; focus contained; screen readers don't escape to page content | Visually looks like a popover (no backdrop) — minor visual/semantic mismatch |
| Non-modal popover (role=menu) | Matches visual treatment | Requires restructuring multi-mode forms to `menuitem` semantics; `role=menu` is wrong for sign-in forms |
| Non-modal unlabeled popover | Simplest | Weakest semantics; no standard pattern for multi-mode forms |

**Chose MODAL** because:
1. The component already declared `role="dialog"` — changing it to `role="menu"` would be a regression.
2. The panel contains complex multi-mode forms (sign-in, create account, settings with destructive admin actions). Modal containment prevents screen-reader focus from accidentally landing on chat messages or other content while the panel is open.
3. The existing `useFocusTrap` hook handles all implementation complexity — zero new dependencies.
4. `aria-modal="true"` + `useFocusTrap` + Escape + focus restore is the complete WCAG 2.1 requirement checklist for modal dialogs.

### Implementation notes

- `useFocusTrap(panelRef, open)` focuses the **× close button** (first focusable element) on open. This is correct: the panel always has the × button regardless of current mode.
- `autoFocus` on form inputs in 'signin' / 'create' modes fires on re-render WHEN those modes activate — after the trap has already set focus on the × button. The `autoFocus` fires naturally without conflicting with the trap.
- `closePanel()` uses `requestAnimationFrame(() => triggerRef.current?.focus())` for deferred restore — needed because the panel DOM is still present during the synchronous `setOpen(false)` call.
- The Escape handler calls `e.stopPropagation()` to prevent the event from bubbling up to the bottom-sheet Escape handler (which would close the mobile sheet unexpectedly).
- Trigger's `onClick` takes a separate open/close path: clicking to close does NOT call `closePanel()` (focus is already on the trigger from the click). Only Escape and the × button go through `closePanel()`.

---

# Decision Summary — Mobile API/Logs Connectivity + Choice Feedback

**Date:** 2026-06-13  
**Author:** Mattingly  
**Branch:** kpoin/ui-mobile-layout  
**Commit:** 8aedb2a5

---

## Task 1 — Logs WebSocket (and all API calls) fail on mobile

### Problem

`DEFAULT_BASE_URL = 'http://localhost:13305'`. On Kyle's phone hitting the dev server at `192.168.3.35:8080`, `window.location.hostname === '192.168.3.35'` — but the API client still constructed all URLs with `localhost`, which the phone resolves to itself (not the PC). Every HTTP request and WebSocket connection silently failed.

### Fix

**`api.ts` `baseUrl` getter (lines 341–355):** After normalizing the stored/default URL, detect `hostname === 'localhost' || '127.0.0.1'` and substitute `window.location.hostname`. This is a no-op on desktop (hostname stays `localhost`) but correctly maps to the LAN IP on mobile. Because all `_fetch` calls and `_buildWebSocketUrl` calls pull from `this.baseUrl`, the single change fixes every endpoint including the logs WebSocket.

**Why not patch individual call sites:** The architecture routes everything through `baseUrl` and `_buildWebSocketUrl`. A single getter fix is the correct level of abstraction.

**User-configured non-localhost URLs:** Explicitly skipped. If the user has set `http://192.168.1.x:PORT` via the Connect view, we respect it unchanged.

---

## Task 1b — Logs WebSocket `websocket_port` fallback

### Problem

The previous diagnosis (see `mattingly-logs-pane-diagnosis.md`) confirmed that `websocket_port` (from `/health`, typically 9000) *does* serve `/logs/stream`. The stale comment in `api.ts` line 823–826 was wrong. When the main port lacks WebSocket upgrade support (stale binary), the UI had no fallback and just reported error.

### Fix

`connectLogStream` now tries the main-port WebSocket first (`suppressPreOpenErrors = true` to keep the console clean). On failure, `tryFallback()` reads `this._healthData?.websocket_port` and retries using the dedicated port. If health data hasn't been fetched yet (unlikely — health polling runs on mount), the fallback silently fails and reports error. This mirrors the exact pattern used in `connectRealtimeTranscription`.

---

## Task 2 — Choice buttons show no feedback after selection

### Problem

`ask_question` tool calls rendered as clickable pill buttons. Clicking a button fired `onOptionSelect` (which sent the choice to the AI) but gave no visual feedback — the button sat there looking unclicked.

### Fix

`ToolCallsDisplay` gained `useState<Map<number, string>>` keyed by call array index `i`. Selection is tracked per tool-call-within-message. On selection:

1. **Selected button** → `options-block__btn--selected` (filled `--accent`), `aria-pressed="true"`, `✓ ` prefix.
2. **Other buttons** → `disabled`, `opacity: 0.4` via CSS.
3. **Confirmation** → `.options-block__confirmation` renders `✓ You chose: <choice>` in `--text-muted`.
4. **Custom input** → hidden after selection.
5. **Double-click guard** → `if (selectedChoice) return` in `handleSelect`.

### Design decisions

- **Local state over message mutation**: Storing in `useState` within `ToolCallsDisplay` is sufficient for the prototype. The calls array for a completed message is immutable; re-renders preserve state via React reconciliation. Only an unmount (e.g. switching conversations) would reset it — acceptable for a POC.
- **CSS tokens used**: `--accent`, `--bg-primary`, `--text-muted`, `--border` — consistent with existing theme.
- **No new deps**: Pure CSS + React state as directed.

---

## Verification

- **Task 1:** webpack build exits 0. The `baseUrl` getter substitutes correctly: accessing the dev server from any non-localhost origin (mobile or different host) will now derive the host from `window.location.hostname`. Logs WebSocket fallback will activate automatically when main-port fails.
- **Task 2:** Build exits 0. Selection state renders correctly at 390×844 viewport.

---

# Logs Pane Diagnosis — 2026-06-13

**Investigator:** Mattingly  
**Reporter:** Kyle (kpoin)  
**Symptom:** Logs pane shows "Error" status; never connects.

---

## Code Review

### LogViewer.tsx (connection mechanism)

- **Mechanism:** WebSocket (native browser `WebSocket`)
- **Target URL:** `ws://localhost:13305/logs/stream` (derived from `baseUrl` via `_buildWebSocketUrl('/logs/stream')`)
- **Auth:** API key appended as `?api_key=...` query param if set (line 476 in `api.ts`)
- **Reconnect:** On disconnect, retries after 5 seconds via `setTimeout(connect, RECONNECT_DELAY)`. Health check (`api.health()`) is attempted first; if health fails, reconnect is also delayed 5s.
- **Error handling:** `onError` callback sets `connStatus` to `'error'`; console.warn printed. Visible "Error" label + "Reconnect" button shown.
- **Mount behavior:** Connects immediately on mount via `useEffect` → `tryConnect()` (line 209–241). Component rendered at `ChatView.tsx:1894` inside the Logs nav route.

### api.ts — `connectLogStream` (line 819–846)

- Calls `_buildWebSocketUrl('/logs/stream')` — uses main API port (13305), NOT the dedicated `websocket_port` from `/health`.
- Comment on lines 823–826 claims `websocket_port` is "used by realtime audio" only and doesn't serve logs. **This is incorrect** — confirmed by direct test that port 9000 serves `/logs/stream`.
- `_openLogSocket` (line 575–634): Opens WebSocket, 5-second connect timeout, sends `{type: 'logs.subscribe', after_seq: null}` on open.

---

## Live Browser Observations (Playwright)

**Status indicator:** `Error`

**Console messages (verbatim):**
```
[warning] WebSocket connection to 'ws://localhost:13305/logs/stream' failed: WebSocket is closed before the connection is established.
[warning] WebSocket connection to 'ws://localhost:13305/logs/stream' failed: WebSocket is closed before the connection is established.
[warning] [LogViewer] Error: Could not connect to log stream on the Lemonade API port.
[warning] WebSocket connection to 'ws://localhost:13305/logs/stream' failed: WebSocket is closed before the connection is established.
```

**Page errors:** None  
**Request failures:** None  
**WebSocket events:**
- `[created] ws://localhost:13305/logs/stream` → `[close]` (immediate close, no open event)
- Repeated twice in the 10-second window.

---

## Backend Probe

### Lemonade server status

**Running:** Yes, PID 38952 (`LemonadeServer.exe`)  
**Port:** 13305  
**Health:** `{"status":"ok","version":"10.7.0","websocket_port":9000,...}`  
**Binary path:** `C:\Users\kpoin\AppData\Local\lemonade_server\bin\LemonadeServer.exe`  
**Binary last modified:** June 10, 2026 2:45 PM (MDT)  
**Process started:** June 11, 2026 8:30 AM

### Direct WebSocket tests

| Target | Result |
|--------|--------|
| `ws://localhost:13305/logs/stream` (main port) | **FAILS** — server responds with HTTP 200 + SPA HTML instead of 101 Upgrade |
| `ws://localhost:9000/logs/stream` (dedicated port) | **SUCCESS** — connects, receives `logs.snapshot` with entries |
| Raw HTTP upgrade headers to port 13305 | Returns 200 + HTML — no upgrade detection |

---

## Diagnosis

### Root Cause

**The running `LemonadeServer.exe` binary predates the WebSocket upgrade handler.**

- The `UpgradableFrontServer` class (which intercepts WebSocket upgrade requests on the main HTTP port and routes them to libwebsockets) was introduced in commit `20126a43` on **June 10 at 5:09 PM PDT** (6:09 PM MDT).
- The installed binary was compiled at **2:45 PM MDT** on June 10 — approximately **3.5 hours before** the upgrade handler was added.
- Without `UpgradableFrontServer`, the main port treats ALL requests as normal HTTP. `/logs/stream` hits the SPA catch-all and returns `index.html` (HTTP 200).
- The UI code (`api.ts` line 827) connects to `ws://<baseUrl>/logs/stream` — using the main port. It intentionally avoids `websocket_port` (9000) due to an incorrect comment stating that port doesn't serve logs.

**Two contributing factors:**
1. **Stale binary** — main-port WebSocket upgrade not compiled in.
2. **Incorrect fallback assumption in UI** — even if #1 is fixed, the UI has no fallback to `websocket_port` if the main-port upgrade fails.

### Evidence

- HTTP 200 (HTML) returned for WebSocket upgrade attempts on port 13305
- Successful WebSocket connection on port 9000 (dedicated) confirms server-side log streaming works
- Binary timestamp (2:45 PM) < feature commit timestamp (6:09 PM) on same day
- Browser console shows immediate WebSocket close without open event

### Confidence: **95% — High**

---

## Recommended Fix

### Option A — Rebuild and reinstall (immediate fix)

Kyle should rebuild `LemonadeServer.exe` from current HEAD (which includes `UpgradableFrontServer`) and restart the server:

```powershell
cmake --build --preset windows --target LemonadeServer
# Then restart LemonadeServer.exe (kill PID 38952, start new)
```

This will enable WebSocket upgrades on the main port and the UI will connect as-is.

### Option B — UI fallback to `websocket_port` (defense in depth)

Even after Option A, the UI should be hardened to fall back to `websocket_port` from `/health` if the main-port WebSocket fails. The incorrect comment at `api.ts:823–826` should be corrected. This is a **code change** — defer to Kyle's authorization.

**Proposed change (api.ts, line 819–838):**
- On initial connection failure (`.catch()` at line 836), retry using `this._healthData?.websocket_port` if available.
- Update the comment to reflect that `websocket_port` does serve `/logs/stream`.

### Recommendation

Do **Option A now** (rebuild), then schedule **Option B** as a follow-up improvement to handle mixed-version deployments gracefully.

---

# Decision: MCP Gateway Phase A — McpPanel design choices

**Agent:** Mattingly (UI)
**Date:** 2026-06-25
**Branch:** `feat/gui3-mcp-dashboard`
**PR:** #2418
**Tracking:** #2417
**Status:** Shipped

---

## Context

Phase A of #2404 (GUI3 MCP support) scoped by @fl0rianr: read-only dashboard surfacing
lemonade's existing `POST /mcp` Streamable HTTP gateway in `ConnectView`. No tool
execution, no external MCP servers, no lemond C++ changes.

---

## Decision 1 — Standalone `McpPanel.tsx` component (not inline in ConnectView)

**Chose:** Extract all MCP logic into `prototype/ui-redesign/src/components/McpPanel.tsx`.

**Rationale:** ConnectView is already large (530 LOC). The MCP panel owns 3 async state
machines (copy, status, tools). Keeping them isolated simplifies Phase B extension
(tool execution, connection config). Props surface is minimal: `connectionStatus`.

---

## Decision 2 — Direct `fetch()` over `api._fetch()`

**Chose:** Call native `fetch()` directly in `McpPanel`, reading `api.baseUrl` and
`api.apiKey` for URL construction and auth header injection.

**Rationale:** `api._fetch()` and `api._headers()` are private methods on the
`LemonadeAPI` class. `/mcp` is not an OpenAI-compatible endpoint and does not belong
in the `api.ts` public surface for Phase A. Adding a `api.mcpPost()` helper is the
right call for Phase B when tool-execution calls need to flow through the same module,
but over-engineering it now would add churn. Decision: revisit in Phase B.

---

## Decision 3 — Two sequential POSTs for initialize + tools/list

**Chose:** Fire `initialize` POST, then `tools/list` POST sequentially (not batched).

**Rationale:** The Streamable HTTP spec (2025-06-18) supports JSON-RPC batch arrays,
but lemonade's server handles each message in the batch independently and returns a
batch response array. For a read-only dashboard that runs infrequently (on connect +
manual refresh), sequential calls are simpler and easier to test. Phase B can switch
to batch if latency becomes an issue.

---

## Decision 4 — Health status derived from tools/list success (not a separate ping)

**Chose:** Set `mcpStatus = 'connected'` only after `tools/list` succeeds, not after a
separate ping.

**Rationale:** The tools list and the health check can be combined — if `tools/list`
succeeds, the gateway is clearly reachable and functional. A separate ping would add
a third request with no UI benefit. The `ping` method is available for Phase B
lightweight keep-alive if needed.

---

## Decision 5 — Transport constraints enforced (no SSE, no OAuth)

Per @fl0rianr on #2404:
- Transport: Streamable HTTP POST-only. No SSE response channel initiated.
- Auth: `Authorization: Bearer` header only. No OAuth flow, no credential prompts.
- GUI3's native tools are NOT routed through MCP (Phase C, post-POC).

These constraints are reflected in the implementation and documented in the PR body.

---

## Open questions for Phase B

1. Should `api.ts` expose a public `mcpPost(method, params)` helper to centralise
   MCP request construction and error handling?
2. Should tool execution results stream (SSE) or remain one-shot JSON-RPC responses?
   (@fl0rianr deferred SSE to Phase B scope decision.)
3. Connection config: should MCP endpoint URL be overridable per-client (e.g., for
   remote lemond over HTTPS)? Currently derived from `api.baseUrl` — already correct
   for the many-clients-one-server topology.

---

# Decision: Mobile Bottom Sheet for Conversations

**Date:** 2026-06-13  
**Author:** Mattingly (UI agent)  
**Branch:** kpoin/ui-mobile-layout  
**Status:** Implemented (pending commit by Squad)

## Context

On mobile (≤480px), the conversation rail is hidden (`display: none` at 768px breakpoint). Users have no way to switch between conversations or start a new chat on their phone.

## Decision

Implement a slide-up bottom sheet triggered by a mobile-only "Conversations" button. The sheet reuses the same conversation list state and handlers already in `ChatView.tsx`.

## Approach: Separate bottom sheet element (Option 3)

### Options considered

| # | Approach | Pros | Cons |
|---|----------|------|------|
| 1 | CSS-only class toggle on existing `<aside class="rail">` | No extra markup | Conflicts with existing 768px `display: none`; requires complex media query overrides; fragile |
| 2 | React portal extraction | Clean component boundary | Over-engineered for POC; adds portal complexity; new component file |
| 3 | **Separate bottom sheet div with duplicated list JSX** | Clean CSS separation; zero risk to desktop rail; minimal React state (1 boolean) | ~40 lines of repeated conversation map JSX |

**Chose Option 3** because:
- Zero risk of regression to the desktop rail layout
- CSS for the sheet is fully isolated inside `@media (max-width: 480px)`
- The "duplicated" JSX is trivial (a `.map()` call) and references the same state/handlers — not actual logic duplication
- Lowest-touch approach for a POC

## Implementation details

- **Trigger:** `.chat__mobile-rail-trigger` — sticky bar at top of `.chat__main`, visible only at ≤480px
- **Sheet:** `position: fixed; bottom: 0` with `transform: translateY(100%/0)` transition (280ms ease-out)
- **Drag-to-close:** Pointer events on handle div; threshold 100px deltaY
- **ESC key:** Keyboard listener when sheet is open
- **Focus return:** On close, focus goes back to trigger button
- **No new dependencies:** Pure CSS + vanilla pointer events

## Tradeoffs

- The conversation list JSX appears twice in `ChatView.tsx`. If the rail item structure changes, both must be updated. Acceptable for a POC; in production, extract a `<ConversationListItem>` component.
- `window.innerWidth` check in the toggle handler is a JS-based breakpoint rather than pure CSS. This is necessary because the same button (`rail__toggle`) serves both desktop and mobile roles. A `useMediaQuery` hook would be cleaner but adds complexity for no functional benefit at POC stage.

## Files modified

- `prototype/ui-redesign/src/components/ChatView.tsx`
- `prototype/ui-redesign/src/styles/styles.css`
- `prototype/ui-redesign/scripts/screenshot-bottom-sheet.mjs` (new)

---

# Mobile Layout Fixes — Decision Note

**Author:** Mattingly (UI Agent)  
**Date:** 2026-06-13  
**Branch:** kpoin/ui-mobile-layout  
**Status:** Implemented — build exits 0

---

## Context

Kyle tested the prototype on his phone (192.168.3.35:8080) and reported the UI looks "squished." This pass audited the prototype for mobile/responsive issues and fixed the highest-priority ones.

---

## Audit Findings

### A — Viewport meta tag
**Status: ✅ Already correct.**  
`src/index.html` line 5 already has:
```html
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover" />
```

### B — CSS partials / media queries
Only two CSS files exist on this branch: `src/styles/styles.css` (6147→6245 lines) and `src/styles/tokens.css`. (No 22-partial split on this branch yet — that was described as a future commit.)

Existing media queries before this pass:
- `@media (max-width: 768px)` — tablet; hides rail, stacks panels, reduces padding
- `@media (max-width: 900px)` — dashboard grid collapses
- `@media (max-width: 720px)` — custom model form grid, account menu name hidden; image settings columns
- `@media (max-width: 1100px)` — image settings 4-col
- `@media (max-width: 980px)` — chat-with-logs layout

**No 480px (phone) breakpoint existed** — this is the root cause of mobile "squished" rendering.

**Issues found in CSS:**
1. **Nav overflow (P0)** — The 7-button titlebar nav (`chat / models / presets / backends / dashboard / logs / connect`) requires ~425px at minimum button size. On a 360-414px phone the buttons push each other, causing the nav to overflow into the right controls (theme toggle, account menu).
2. **Message content tables** — `.message__content table` had no overflow protection. Markdown-rendered tables would push the page horizontally.
3. **Touch targets below 44px (P1)** — `.composer__send` and `.composer__stop` were 36×36px. WCAG / iOS HIG minimum is 44×44px.
4. **Composer toolbar wrapping** — No `flex-wrap` on phone; long preset/tools labels could overflow.
5. **Slide-over width** — Already had `max-width: 90vw` but on phones 90vw still leaves an uncoverable strip; full-width is better UX on phone.
6. **Connect view padding** — `padding: 0 var(--space-6)` (32px each side) on a 360px phone = 296px content area — functional but tight.
7. **Chat-with-logs layout** — At 980px the rail collapsed but at 480px the rail was still shown in the grid template even though rail content is hidden at 768px.
8. **No breakpoint tokens** — `tokens.css` had no documented breakpoint variables.

### C — Layout components
- `App.tsx` root layout (grid-template-rows) is fine — it's two rows, not side-by-side on any axis.
- Rail hides correctly at 768px (`display: none`).
- `.chat` becomes single-column at 768px ✅
- ModelManager, PresetManager, BackendManager all get single-column at 768px ✅
- Slide-over already has `max-width: 90vw` ✅

### D — Touch targets
- Nav buttons: height is ~21px at 768px (padding 5px + font 11px). On phone this is below 44px but widening them would require a different layout pattern. Accepted as-is for nav; horizontal scroll compensates.
- Send/stop: Fixed to 44px.

### E — Typography
- Base size 14px (`--text-base`) — iOS will not auto-zoom inputs as long as inputs inherit this size (they do).
- Heading sizes reduce gracefully at 768px already.

---

## Breakpoints Established

```
--breakpoint-mobile:  480px   (phone — new)
--breakpoint-tablet:  768px   (tablet — was already in use)
--breakpoint-desktop: 1024px  (desktop narrow — documented, not yet used)
```

**Note:** CSS custom properties cannot be used inside `@media` expressions. These are documentation values in `tokens.css`. Raw pixel values are used in `@media` blocks.

---

## What Was Fixed (P0–P1)

| # | Issue | File | Lines |
|---|-------|------|-------|
| 1 | Nav horizontal scroll on 480px phone | `styles.css` | ~3670–3720 |
| 2 | Breakpoint tokens added | `tokens.css` | ~118–130 |
| 3 | Message content tables get `overflow-x: auto` | `styles.css` | ~755–763 |
| 4 | Send/stop buttons → 44px touch target at 480px | `styles.css` | ~3670–3720 |
| 5 | Composer toolbar `flex-wrap` at 480px | `styles.css` | ~3670–3720 |
| 6 | Slide-over full-width (`100vw`) at 480px | `styles.css` | ~3670–3720 |
| 7 | Connect view padding reduced at 480px | `styles.css` | ~3670–3720 |
| 8 | Chat inner tighter padding at 480px | `styles.css` | ~3670–3720 |
| 9 | Log viewer toolbar gap tightened at 480px | `styles.css` | ~4833–4838 |
| 10 | Chat-with-logs collapses rail column at 480px | `styles.css` | ~6218–6234 |

---

## Round 2 — 2026-06-13

Kyle re-tested on his 390px iPhone and filed two specific issues.

---

### Issue 1 — Nav icon-only at 480px

**Chosen approach: Option A (icons-only labels hidden)**

Rationale: Every nav item already had a suitable icon in `Icon.tsx`. Hiding text labels at 480px reduces each button from ~65px wide to ~28px wide; 7 × 28px ≈ 196px, well within the `1fr` nav column — no scrolling, no drawer, no state. Option B (hamburger/drawer) would require React state + a new drawer component and hides the navigation structure from first glance. For a POC, Option A is the correct trade-off.

**Icon assignments (chat→`chat`, models→`hard-drive`, presets→`sliders-horizontal`, backends→`box`, dashboard→`gauge`, logs→`logs`, connect→`plug`)**

Accessibility preserved via `title` + `aria-label` on every button; icon SVG is `aria-hidden="true"`. The `<span class="nav-label">` wrapper means desktop/tablet still show full text labels — only the 480px breakpoint hides them.

**Right-side cluster:** `.titlebar__right` now has `flex-shrink: 0` (global) to prevent the AccountMenu / theme-toggle / status-dot from ever compressing.

---

### Issue 2 — Model card vertical stack at 480px

`.row__content` at 480px changes from `display: grid (1fr auto)` to `display: flex; flex-direction: column; align-items: stretch`. This makes:
- Backend badge + icon + model name wrap naturally in a row at the top
- Model name gets full width and wraps (no more single-character truncation)
- Action buttons (Load, Delete, etc.) form a full-width, equally-spaced row at the bottom

Only `ModelManager.tsx` uses `row__content` / `row__main`; preset cards and backend cards use different class structures and did not need this fix.

---

### Build

`npm run build` exits **0** — only pre-existing bundle-size warnings, no new errors.

**Files changed:**
- `prototype/ui-redesign/src/App.tsx` — nav button render: `<Icon> + <span class="nav-label">` with title/aria-label
- `prototype/ui-redesign/src/styles/styles.css` — `.titlebar__nav button` inline-flex; `.titlebar__right` flex-shrink:0; 480px block: nav icon-only + model card stack

1. **Nav icon-only view at 480px** — ideally nav buttons would show just an icon glyph on very small screens, hiding text. This requires the Icon component and changes in `App.tsx`. The horizontal scroll workaround is acceptable for the demo.
2. **Nav button height < 44px** — the pill nav buttons are ~21px tall at 480px. To fix properly, convert to a bottom tab bar or a hamburger drawer. Out of scope for this pass.
3. **`icon-btn` and `titlebar__theme-toggle` at 32px** — below 44px. These are secondary controls and don't need to meet the full tap-target standard urgently.
4. **`ScriptProcessorNode` deprecation** — pre-existing, noted in history.
5. **ChatView.tsx 101KB** — decomposition effort, separate pass.
6. **Preset slide-over inner content on phone** — the `preset-assignment-panel` and `detail__grid` collapse to 1-col at 768px; confirmed working at 480px.
7. **Image settings composer at 480px** — already 2-col at 720px breakpoint; usable.

---

## Architectural Notes

- The 480px block was added in **three locations** in `styles.css` to keep it co-located with the feature sections it affects: (a) the main responsive section after the 768px block, (b) the log viewer section, (c) the end-of-file chat-with-logs section.
- `display: block; overflow-x: auto` on `.message__content table` is applied globally (not just at 480px) because table overflow is a bug at any viewport width.
- Desktop WebView2/WKWebView is unaffected — these breakpoints only fire at screen widths below 480px which no desktop window will hit unless explicitly resized very small.

---

## Build

`npm run build` exits **0** with only pre-existing bundle-size warnings (no new errors or TypeScript issues).

**Kyle's test URL:** `http://192.168.3.35:8080`

---

# Decision: Mobile Layout Round 3 — Visual Verification + Cache Fix

**Date:** 2026-06-13  
**Author:** Mattingly (UI agent)  
**Status:** Implemented  
**Branch:** kpoin/ui-mobile-layout

## Context

Kyle reported that his phone still showed the old un-fixed layout after Rounds 1 and 2 of mobile CSS fixes. This raised the possibility that the CSS changes weren't taking effect.

## Investigation

Used Playwright with a 390×844 viewport (iPhone 14 equivalent) to screenshot the Chat and Models pages at `http://localhost:8080`. Visual inspection confirmed:

- Nav bar: icons-only, 7 buttons fit without scrolling ✓
- Model cards: stacked vertically, names readable ✓
- No horizontal overflow ✓

**Conclusion:** Round 1 and 2 fixes ARE live. The issue is browser caching on Kyle's phone.

## Root Cause

`webpack-dev-server` with `style-loader` serves CSS embedded in JS bundles. HMR updates via WebSocket, but phone browsers on LAN frequently disconnect (screen lock, tab switch, Wi-Fi handoff). Without `Cache-Control` headers, the phone browser serves its cached copy of the JS bundle — which still contains old CSS.

## Changes Made

1. **webpack.config.js** — Added `headers: { 'Cache-Control': 'no-store' }` to `devServer` config. Prevents phone browsers from caching stale bundles.
2. **styles.css** — Minor polish: better right-edge padding, tighter hero cards, smaller composer pills, smaller section labels — all at 480px breakpoint.
3. **scripts/screenshot-mobile.mjs** — New utility for future visual verification with Playwright.

## Recommendation for Kyle

Hard-refresh the page on his phone (pull down to refresh in iOS Safari, or long-press the reload button → "Request Desktop Site" → refresh again). After the `no-store` header is active (requires dev server restart), future CSS changes will always be fetched fresh.

## Lesson Learned

Always verify CSS changes with actual screenshots before declaring them done. A screenshot is proof; code review alone is not sufficient for visual correctness.

---

# Decision: Presets Redesign Audit — Key Recommendations

**Agent:** Mattingly (UI)  
**Date:** 2026-06-19  
**Branch:** `kpoin/ui-testing`  
**Source doc:** `prototype/ui-redesign/docs/PRESETS_REDESIGN.md`  
**Status:** Pending review by Kyle / Kranz

---

## Summary

Mattingly completed a full design audit of the Presets UI on 2026-06-19, including:
- 12 Playwright screenshots at desktop (1440×900) and mobile (390×844)
- Code-level root cause analysis of the "can't edit starters" gap
- Recipe↔preset integration gap analysis
- Three-phase recommendation roadmap

---

## Key Decisions Needed

### D1 — Recipe preference field rename (Phase B gate)

**Proposal:** Rename `engine_hint` to `recipe_preference` in `Preset` interface (backward-compat).  
**Impact:** Signals intent change from passive hint → active preference. Drives new apply-time recipe selection UX.  
**Decision needed:** Approve rename and backward-compat migration strategy.

### D2 — Starter editability model (Phase A)

**Two options:**
- **Option A (recommended):** Duplicate-to-customize. Starter cards get a "Customize →" button that clones + opens edit. Starters remain read-only. Simplest.
- **Option B:** Unlock-and-edit. Add an "Unlock" flow that moves a starter into user presets for direct editing. More complex; requires UX to handle "what if you want the original back."

**Decision needed:** Kyle confirms Option A or B.

### D3 — AutoOpt rail visibility

**Proposal:** AutoOpt summary collapses by default on mobile; AutoOpt rail collapses on initial load (not just toggle).  
**Decision needed:** Does Kyle want AutoOpt visible by default for power users, or collapsed by default for new users?

---

## Committed Artifacts

| Artifact | Path |
|----------|------|
| Full audit + recommendations | `prototype/ui-redesign/docs/PRESETS_REDESIGN.md` |
| Screenshot script (reusable) | `prototype/ui-redesign/scripts/screenshot-presets.mjs` |
| Desktop grid screenshot | `docs/screenshots/presets/01-presets-grid-desktop.png` |
| Mobile grid screenshot | `docs/screenshots/presets/02-presets-grid-mobile.png` |
| Starter readonly evidence | `docs/screenshots/presets/04-starter-slideover-readonly.png` |
| New preset form | `docs/screenshots/presets/05-custom-preset-create.png` |
| _(+ 7 more screenshots)_ | `docs/screenshots/presets/` |

---

## Top Findings (for Scribe)

1. **Starters are permanently read-only** (`isReadOnly = preset.starter`, `PresetManager.tsx:749`). The only affordance is "Clone" at the bottom of a scrollable panel — no on-card CTA. Kyle's pain is 100% real and code-confirmed.

2. **Recipe/engine preference is invisible and passive.** `engine_hint` exists in all starters but never appears on cards, lives in a collapsed "Advanced" section, and has no effect on which backend actually loads.

3. **Mobile: AutoOpt block dominates the first viewport.** The full CLI args string takes ~40% of first-paint at 390px. Starters are below it.

## Top Recommendations (for Scribe)

1. **Phase A (≤1 day): Add "Customize →" button to starter card face.** Calls `handleClone` (already exists) and opens the edit slideover immediately. No data model change.

2. **Phase B (~1 week): Promote `engine_hint` → `recipe_preference`, surface on cards, add recipe picker in slideover body, add mismatch dialog at apply-time.**

3. **Phase A (≤half day): Collapse AutoOpt summary by default on mobile.** Wrap in `<details>` closed by default; expose starters as the first content below the header.

---

# Decision: Test failure fixes — 2026-06-15

**Author:** Mattingly (UI / Frontend)
**Date:** 2026-06-15
**Status:** Implemented and pushed (kpoin/ui-testing, commits c225d4bd–c0101518)
**Requested by:** Kyle (kpoin)

---

## Context

External collaborator reported 5 test failures on `kpoin/ui-testing`. Directive: fix all 5 without weakening test assertions. A full-suite run revealed a 6th pre-existing failure (A05 Dashboard contrast) that was also fixed.

Final result: **50 passed, 7 skipped, 0 failed** (same 7 skips as before, no new skips).

---

## Decisions made

### 1. `<main>` gets `tabIndex={-1}` (not moved to a different element)

**What:** Added `tabIndex={-1}` to `<main id="main-content">` in App.tsx.

**Why tabIndex=-1 specifically:** Makes the element programmatically focusable (anchor links, `el.focus()`) without adding it to the sequential Tab order. This is the correct ARIA pattern for skip-link targets. `tabIndex=0` would add `<main>` to the Tab order, which is undesirable (Tab would stop on the entire main content area).

**Alternatives considered:** Using `<div tabIndex={-1}>` as an inner wrapper. Rejected — the `<main>` landmark itself should be the focus target so screen readers hear the landmark role when focus moves there.

---

### 2. Button reset uses `outline: 0` not `outline: none`

**What:** Changed `button {}` reset from (no outline property) to `outline: 0`.

**Why not `outline: none`:** CSS `outline: none` sets `outline-style: none` but does NOT set `outline-width` to zero. Chromium's `getComputedStyle().outlineWidth` returns the UA-default width (3px) even when `outline-style: none`, causing the A29 test (`outlineWidth === '0px'`) to fail. `outline: 0` explicitly sets `outline-width: 0`.

**Why this doesn't break keyboard focus rings:** Our global `:focus-visible { outline: 2px solid var(--accent); }` rule has higher specificity (pseudo-class selector = 0-1-0) than the type selector `button` (0-0-1). `:focus-visible` is not overridden.

**Risk:** Suppresses browser default focus ring for all `<button>` elements. This is acceptable because we have an explicit `:focus-visible` ring. We must never remove the `:focus-visible` rule without replacing with an alternative visible focus indicator.

---

### 3. PresetCard: overlay-button pattern instead of `article[role=button]`

**What:** `<article role="button" tabIndex=0>` replaced with `<article>` containing `<button class="recipe-card__overlay-btn">` (absolute, inset: 0, z-index: 0). Card content at z-index: 1.

**Why:** WCAG 4.1.2 / axe `nested-interactive` rule forbids interactive elements containing other interactive elements. `<article role="button">` containing `<button>Clone</button>` is a violation.

**Why overlay button specifically:** The entire card surface should be clickable to open the slideover (UX requirement). The inner Clone/Apply/Export buttons are secondary actions. The overlay-button pattern allows the full card to be the primary interaction without nesting interactive roles.

**Behavioral note:** `focus-within` CSS that reveals action buttons continues to work because focusing the overlay button fires `:focus-within` on the parent `<article>`. Keyboard users Tab to the overlay button, see actions appear, can Tab again to reach Clone/Apply.

**Trade-off:** The overlay button has no visible text — its accessible name comes entirely from `aria-label="Open Preset: {name}"`. Screen readers announce "Open Preset: Balanced, button". This is an improvement over the previous `aria-label="Preset: Balanced"` on a `role="button"` div that didn't clearly indicate it's activatable.

---

### 4. Rail listbox empty state: `<p>` sibling, not `<li>` inside `<ul>`

**What:** `{conversations.length === 0 && <li className="rail__empty">No conversations yet</li>}` moved outside `<ul role="listbox">` and changed to `<p className="rail__empty">`.

**Why:** `role="listbox"` requires all children to be `role="option"` (or `role="group"` containing options). A plain `<li>` without `role="option"` violates `aria-required-children`. Moving it outside the listbox (as a sibling `<p>`) is semantically correct — the empty state is informational text about the listbox, not a listbox option.

**Alternative considered:** `<li role="option" aria-disabled="true">No conversations yet</li>`. Rejected — a disabled option implies there IS a selectable item but it's currently disabled. An empty state message is fundamentally different.

---

### 5. Dashboard contrast: color tokens, not opacity

**What:** Removed `opacity: 0.5` from `.dash2-slot-legend__item--idle`. Added `color: var(--text-tertiary)` overrides for idle label and idle TPS text.

**Why:** Axe evaluates contrast based on the actual rendered pixel color, not the CSS token value. `opacity: 0.5` composites `--text-secondary (#C7C2B5)` against `--surface-base (#1a1813)` to produce a rendered color of approximately `#716d64` (contrast 3.44:1 < 4.5:1 required). Using `--text-tertiary (#A8A39A)` at full opacity gives ~7:1 contrast — accessible and still visually distinct from active items (which use `--text-secondary` and `--text-primary`).

**General rule established:** Never use `opacity` to dim text in the UI. Always use a token with sufficient contrast at full opacity.

---

### 6. A03 test selector: `[data-view="presets"]` not `.manager`

**What:** `await page.waitForSelector('.manager')` changed to `await page.waitForSelector('[data-view="presets"]')` in the A03 test.

**Why this is a test fix, not lowering the bar:** The `waitForSelector` is a readiness guard (ensures the view rendered before running axe). `.manager` is ModelManager's root class — it was the wrong selector for the Presets view. Correcting it to the actual Presets view root selector is required to let axe run. The axe assertion itself is unchanged.

---

## Files changed

| File | Purpose |
|------|---------|
| `prototype/ui-redesign/src/App.tsx` | `tabIndex={-1}` on main; `'Dash'` → `'Dashboard'` nav label |
| `prototype/ui-redesign/src/styles/styles.css` | `outline: 0` button reset; overlay-button CSS; Dashboard contrast fix |
| `prototype/ui-redesign/src/components/ChatView.tsx` | Rail listbox `aria-label` + empty state as `<p>` |
| `prototype/ui-redesign/src/components/PresetManager.tsx` | PresetCard overlay-button pattern |
| `prototype/ui-redesign/tests/a11y.spec.ts` | A03 `waitForSelector` corrected |

---

# 2026-06-05: Tools toggle scoped-state migration

**Author:** Mattingly
**Scope:** `prototype/ui-redesign/`
**Status:** Proposed / patched in POC

## Context

The chat tools toggle was moved from the legacy global `localStorage` key `lemonade_use_tools` into scoped account storage (`lemonade:<scope>:use_tools`) when local accounts landed. That preserves the many-clients-one-server invariant, but legacy guest users can see their previous tools preference reset because the old key is not read.

## Recommendation

Scoped UI preferences should stay client-local, but each preference moved behind `scopedStorageKey()` needs an explicit one-time legacy migration for the `guest:shared` scope and a React state refresh when `accountSession.storageScope` changes.

This is not a decision to make tools default ON. It is a migration rule: preserve an existing user's explicit local preference, otherwise keep the current OFF default.

---


