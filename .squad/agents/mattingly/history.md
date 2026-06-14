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

### 2026-06-13: Mobile API/logs connectivity + choice feedback (branch kpoin/ui-mobile-layout)

**Task 1 — Mobile URL fix (api.ts `baseUrl` getter, lines ~341-355)**

Root cause: `DEFAULT_BASE_URL = 'http://localhost:13305'` — on a phone, `localhost` resolves to the phone itself, not Kyle's PC. Fix: in the `baseUrl` getter, after normalizing the stored/default URL, check if the hostname is `localhost` or `127.0.0.1`; if so, substitute `window.location.hostname`. On desktop this is a no-op (`window.location.hostname === 'localhost'`). On mobile hitting the dev server at 192.168.3.35:8080, it resolves to `192.168.3.35` and all API calls (fetch + WebSocket) work correctly. This single-point fix propagates to every `_fetch` call and `_buildWebSocketUrl` call — no per-site patches needed.

**Task 1 — WebSocket fallback (`connectLogStream`, lines ~827-870)**

Replaced the incorrect comment that discouraged using `websocket_port` (it *does* serve `/logs/stream` — confirmed in diagnosis). `connectLogStream` now: (1) tries main-port WebSocket first, (2) on failure calls `tryFallback()` which reads `this._healthData?.websocket_port` and retries using `_buildWebSocketUrl('/logs/stream', fallbackPort)`. This mirrors the pattern already established in `connectRealtimeTranscription` (line 872). The fallback handles stale-binary deployments where the main port lacks WebSocket upgrade support.

**Task 2 — Choice selection feedback (`ToolCallsDisplay`, ChatView.tsx ~2281)**

`ToolCallsDisplay` gained `useState<Map<number, string>>` keyed by call index `i` for per-call selection tracking. When a choice is selected:
- `handleSelect(choice)` sets the Map entry and calls `onOptionSelect` (triggers AI continuation)
- Guards against double-selection (`if (selectedChoice) return`)
- Selected button: `options-block__btn--selected` class (filled `--accent`), `aria-pressed="true"`, `✓ ` prefix glyph
- Other buttons: `disabled={!!selectedChoice && selectedChoice !== choice}` + `opacity: 0.4` via CSS
- Confirmation line: `.options-block__confirmation` (`✓ You chose: <choice>`) in `--text-muted`
- Custom input hidden after selection (`{!selectedChoice && allowCustom && ...}`)

CSS additions in `styles.css`: `.options-block__btn--selected`, `.options-block__btn:disabled`, `.options-block__confirmation`.

**Files changed:**
- `prototype/ui-redesign/src/api.ts` — `baseUrl` getter substitution; `connectLogStream` fallback logic
- `prototype/ui-redesign/src/components/ChatView.tsx` — `ToolCallsDisplay` selection state
- `prototype/ui-redesign/src/styles/styles.css` — selected/disabled/confirmation CSS



Kyle tested the prototype on his phone (192.168.3.35:8080) and found the UI squished. Audited all responsive issues and fixed the top 10 in one pass. Build exits 0 — only pre-existing bundle-size warnings.

**Breakpoints established:**
- `--breakpoint-mobile: 480px` (phone) — **new**, added to `tokens.css`
- `--breakpoint-tablet: 768px` (tablet) — already in use; now documented as a token
- `--breakpoint-desktop: 1024px` — documented for future use

Note: CSS custom properties can't be used in `@media` expressions; tokens are for documentation only; raw `px` values appear in the actual `@media` queries.

**Root cause of "squished" rendering:** The viewport meta was already correct. The real problem was the 7-button titlebar nav overflowing its container on 360–414px phone screens — there was no 480px breakpoint and no overflow handling on the nav pill.

**Key fixes:**
1. **Nav horizontal scroll** — `.titlebar__nav` at 480px gets `overflow-x: auto; scrollbar-width: none; -webkit-overflow-scrolling: touch`. Nav buttons get `flex-shrink: 0` so they stay full-width and remain scrollable rather than squishing. The grid column constraint (`1fr`) limits the nav cell to available space; the pill scrolls internally.
2. **Message content tables** — `display: block; overflow-x: auto` added globally (all viewports) to `.message__content table`. Markdown-rendered tables can be arbitrarily wide and were causing horizontal page push.
3. **Touch targets** — `.composer__send` and `.composer__stop` bumped to 44×44px at 480px (from 36px) to meet iOS HIG / WCAG minimums.
4. **Composer toolbar** — `flex-wrap: wrap; gap: var(--space-2)` at 480px prevents tool/preset labels from overflowing.
5. **Slide-over** — `width: 100vw; max-width: 100vw` at 480px; full-screen panels feel more native on phones.
6. **Panel padding tightening** — Manager head/body, PresetManager, BackendManager, Connect view, chat inner all get tighter padding at 480px to maximize content area.
7. **Chat-with-logs at 480px** — Explicit 480px block collapses the rail column from the chat-with-logs grid layout (rail is already `display: none` below 768px, but the grid column was still allocated).
8. **Log viewer** — Gap and timestamp width tightened at 480px.

**P2 items (not fixed this pass, documented in `.squad/decisions/inbox/mattingly-mobile-layout-fixes.md`):**
- Nav button height still ~21px (below 44px); needs icon-only view or bottom tab bar to fully fix.
- `icon-btn` and `titlebar__theme-toggle` at 32px — secondary controls, below 44px threshold.
- Bottom tab bar / hamburger nav is the ideal end-state for phone; horizontal scroll is the acceptable interim.

**Files changed:**
- `prototype/ui-redesign/src/styles/tokens.css` — breakpoint custom properties added
- `prototype/ui-redesign/src/styles/styles.css` — table fix (global) + three 480px media query blocks

#### Round 2 — 2026-06-13 (this pass)

Kyle re-tested on his 390px iPhone and filed two specific issues. Both fixed. Build exits 0.

**Issue 1 — Nav bar still clipping (Option A chosen)**

**Approach:** Icons-only at 480px. Added `Icon` to each nav button in `App.tsx` and wrapped the text in `<span class="nav-label">`. At `@media (max-width: 480px)`, `.nav-label { display: none }` hides the text; buttons become icon-only squares (~28px wide). 7 × 28px ≈ 196px, comfortably within the nav's `1fr` column — no scrolling needed.

**Why Option A over Option B (hamburger):** All icons were already present in `Icon.tsx` with clear visual meaning. Icon-only nav is a well-understood pattern. Hamburger requires React state + drawer component — far more invasive for a POC, and hides the "7 nav items exist" affordance. Option A is a 2-file change; Option B would be a 5+ file change.

**Icon assignments:**
- chat → `chat` (speech bubble)
- models → `hard-drive` (downloaded model storage)
- presets → `sliders-horizontal` (tuning sliders)
- backends → `box` (backend packages)
- dashboard → `gauge` (performance/stats gauge)
- logs → `logs` (list lines)
- connect → `plug` (connection plug)

**Accessibility:** All buttons already had `aria-label` for the active view; I added explicit `title` + `aria-label` attributes to every nav button so screen readers and tooltip-on-hover work in icon-only mode. `Icon` SVG is `aria-hidden="true"`.

**Also fixed:** `.titlebar__right` got `flex-shrink: 0` (global, not breakpoint-scoped) to ensure the right-side controls cluster (AccountMenu, theme toggle, status dot) never compresses when nav expands.

**Issue 2 — Model cards overlapping on mobile**

At 480px, `.row__content` switches from `grid (1fr auto)` to `flex-direction: column; align-items: stretch`. Key overrides:
- `.row__main`: `align-items: flex-start; flex-wrap: wrap; gap: var(--space-2)` — badge + icon + text wrap naturally
- `.row__backend-badge`: `width: auto; height: 28px` — badge shrinks to fit content, no longer fixed 70px that squeezes the name
- `.row__text`: `flex: 1 1 0` — text block takes remaining row width, allowing full model-name display
- `.row__name`: `white-space: normal; overflow-wrap: anywhere` — names no longer truncate to single character
- `.row__right`: `width: 100%; justify-content: flex-start; flex-wrap: wrap` — action buttons form a full-width row at bottom
- `.row__action`: `flex: 1 1 auto; justify-content: center` — Load/Delete/etc. grow to fill available space equally

**Bonus check:** `row__content` / `row__main` are only used in `ModelManager.tsx`. Preset cards and backend cards use different class structures — no additional stacking needed.

**Files changed (Round 2):**
- `prototype/ui-redesign/src/App.tsx` — nav buttons now render `<Icon> + <span class="nav-label">` with title/aria-label; lines ~262–280
- `prototype/ui-redesign/src/styles/styles.css` — `.titlebar__nav button` gets `display: inline-flex; align-items: center; gap`; `.titlebar__right` gets `flex-shrink: 0`; 480px block updated with nav icon-only rules + model card vertical stack; lines ~103–126, ~3674–3730

#### Round 3 — 2026-06-13 (visual verification + fixes)

Kyle reported "chat page still looks the same as before any fix." Used Playwright (headless Chromium, 390×844 viewport, 2× DPR) to take actual screenshots and visually verify.

**Findings:** Round 1 and Round 2 fixes ARE applied. Screenshots prove:
- Nav is icons-only at 390px ✓
- Model cards stack vertically with readable names ✓
- No horizontal overflow or squishing ✓

**Root cause of Kyle's report:** Stale browser cache on his phone. The webpack dev server uses `style-loader` (CSS bundled into JS via HMR WebSocket). When a phone disconnects from HMR (screen off, tab backgrounded, network hop), the browser serves its cached JS bundle containing old CSS. No `Cache-Control` header was set, so the phone browser had no reason to revalidate.

**Fixes applied this round:**
1. **Cache-busting headers** — Added `headers: { 'Cache-Control': 'no-store' }` to `webpack.config.js` `devServer` block. Forces phone browsers to always revalidate, preventing stale CSS from being served.
2. **Right-edge padding** — Titlebar at 480px now uses asymmetric padding (`0 var(--space-3) 0 var(--space-2)`) so the status dot's 3px box-shadow glow doesn't clip at the viewport edge.
3. **Tighter right controls** — `.titlebar__right` gap reduced to `var(--space-1)` at 480px.
4. **Active model cards** — Padding and font tightened in hero's `.active-card` at 480px.
5. **Hero title/subtitle** — Smaller font at 480px for more content density.
6. **Section labels** — Smaller font and reduced margin at 480px.
7. **Composer toolbar pills** — Smaller font/padding at 480px to reduce bottom-bar chrome.
8. **Model selector cap** — `max-width: 50vw` (was 55vw) at 480px.

**Lesson learned:** CSS-first verification without screenshots is risky. Rounds 1 and 2 were correct but I had no proof. Always take a Playwright screenshot to confirm the browser actually renders what you expect. Added `scripts/screenshot-mobile.mjs` for future verification.

**Files changed (Round 3):**
- `prototype/ui-redesign/src/styles/styles.css` — titlebar padding, right controls, active-card compacting, hero font, section labels, composer toolbar pills, model-selector cap; lines ~3692, ~3767–3791, ~6296
- `prototype/ui-redesign/webpack.config.js` — `Cache-Control: no-store` header in devServer; line ~71
- `prototype/ui-redesign/scripts/screenshot-mobile.mjs` — new; Playwright screenshot helper (accepts prefix arg)


Four P0/P1 fixes from the earlier audit were implemented in one pass on branch `kpoin/ui-audit-fixes`. Build exits 0 with no new errors (only pre-existing size warnings).

**Fix #1 (P0) — onToolCalls hang guard** (`src/hooks/useChatStreaming.ts`, lines ~186–283): Wrapped the entire `onToolCalls` async callback body in try/catch. On catch: marks all still-running `ToolCallEntry` items as `status: 'error'`, clears the token buffer, calls the outer `onError` callback, calls `cleanup(convoId)`, and calls `resolve()`. This ensures the Promise returned by `runCompletion` always settles even if `execute()` throws an uncaught exception. The inner `try/catch` for the recursive `runCompletion()` call was preserved inside the outer try — its `reject(err)` path does not re-throw so it won't double-fire the outer catch.

**Fix #2 (P1) — ModelManager load errors surfaced** (`src/components/ModelManager.tsx`, `src/styles/styles.css`): Added `loadError: { modelName: string; message: string } | null` state. Both `handleLoad` and `handlePullAndLoad.onComplete` now set `loadError` on catch using `friendlyErrorMessage(err)`, auto-clear after 6 s, and clear at the start of a new load attempt. `renderModelRow` renders a `.row__load-error` div below the row content when `loadError.modelName` matches. CSS class added to `styles.css` using `var(--danger)` / `var(--danger-soft)` consistent with `hf-zone__empty--error`.

**Fix #3 (P1) — Preset wired to image composer** (`src/components/ChatView.tsx`, lines ~295–305, ~636–641): Extended `imageDefaultsForModel` to accept an optional third parameter `activePresetRecipeOptions?: Record<string, unknown> | null`, spread into the defaults after `loadedRecipeOptions` (highest priority). Updated the `defaultImageSettings` useMemo to pass `currentPreset?.recipe_options` (only when `currentCapability === 'image'`) and added `currentPreset` and `currentCapability` as dependencies. The existing useEffect at line ~652 already guards with `!imageSettingsTouchedRef.current`, so user-edited values are never clobbered.

**Fix #4 (P1) — modeSupportsChatCompletions unified** (`src/components/ChatView.tsx`, line ~731): Replaced the conditional `currentLoadedModel ? canUseChatCompletions(currentLoadedModel) : (currentCapability === 'chat' || currentCapability === 'omni')` with simply `currentCapability === 'chat' || currentCapability === 'omni'`. `currentCapability` is already derived from `currentModelSnapshot`, which prefers custom/known model info over the raw loaded-model capability — so it correctly reflects 'chat' for custom models with no recipe field. The `canUseChatCompletions` guard was redundant and was the root cause of the tools toggle silently disabling for custom models.

**Architectural notes:**
- Fix #3 layering order (model image_defaults → model recipe_options → loaded recipe_options → preset recipe_options) means a preset can override model-baked defaults, which is the intended behavior for "Sharp" / "Quick" image presets.
- Fix #4 removes the last reference to `canUseChatCompletions` in the rendering path; the function remains in `modelCapabilities.ts` and may still be useful for other callers.

### 2026-06-13: Audit pass — HEAD af66ea14 (6 commits from fl0rianr absorbed into kpoin/ui-testing)

Six commits since last audit substantially rewrote the prototype. Key learnings:

**What's genuinely fixed:** The Omni collection P0 (sending `collection.omni` to lemond) is properly resolved — `loadModelRuntime` in ModelManager now correctly recurses into component models, only calling `api.loadModel` on leaf models, not on the collection wrapper. Tools toggle scoping is fixed — `use_tools` is now stored under `lemonade:<scope>:use_tools` and reads correctly. Preset store scope is properly synchronized in App.tsx across init, effect, and handler. useAudioCapture cleanup, realtime handle cleanup, and LogViewer WebSocket cleanup are all correct.

**What's new and working well:** LogViewer is fully wired — it streams from `/logs/stream` WebSocket, has level filter + search, server log-level control, virtual scroll, and inline toggle in ChatView. This directly addresses Kyle's "is there a log I can view?" question. The tool-call display via ToolCallsDisplay correctly renders `ask_question` as interactive buttons with a custom input fallback. `MAX_TOOL_ROUNDS = 5` prevents runaway loops. The compose-runtime plumbing (`composeToolRuntimes`) for mixing Lemonade + Omni tools is clean.

**Key risk areas at HEAD:** (1) `onToolCalls` in `api.chatCompletion` is fired without await — if any `execute()` throws unexpectedly (vs returning an error result), the runCompletion Promise never resolves and the stream hangs. In practice executors guard with try/catch, so this is unlikely but architecturally fragile. (2) `handleLoad` / `handlePullAndLoad` in ModelManager swallow errors to `console.error` only — users see nothing if loading fails. (3) Active image preset's `steps`/`cfg_scale` from `recipe_options` are not fed back into the composer's `imageSettings` state — "Sharp" preset says 30 steps but the composer shows 20. (4) `ScriptProcessorNode` is deprecated and will start failing in newer Chrome versions. (5) ChatView.tsx is 101KB — difficult to navigate and reason about.

**Surprises:** The lemonadeTools.ts rewrite (+715 lines) is remarkably clean — well-structured schema definitions, defensive argument parsing, nuanced HF pull flow with variants. The image generation path in ChatView is well-validated (nearestImageSize, partialImageSettingsFromSource). The new Icon system replaces emoji glyphs cleanly.

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

### 2026-06-13: Logs pane investigation

**Symptom:** Logs pane shows "Error" status; WebSocket to `ws://localhost:13305/logs/stream` closes immediately without opening.

**Root cause:** The installed `LemonadeServer.exe` (compiled June 10, 2:45 PM) predates the `UpgradableFrontServer` feature (commit `20126a43`, June 10, 6:09 PM). Without that feature, the main HTTP port cannot perform WebSocket upgrades — requests fall through to the SPA catch-all returning HTML.

**Key finding:** The dedicated `websocket_port` (9000) DOES serve `/logs/stream` successfully. The UI code (`api.ts:823-826`) has an incorrect comment claiming it doesn't, and intentionally avoids using it.

**Fix:** Rebuild `LemonadeServer.exe` from current HEAD (Option A). Defense-in-depth: add `websocket_port` fallback in `api.ts` (Option B, deferred to Kyle's authorization).

**Diagnosis written to:** `.squad/decisions/inbox/mattingly-logs-pane-diagnosis.md`  
**Playwright script saved to:** `prototype/ui-redesign/scripts/diagnose-logs.mjs`
- Files: `presetStore.ts`, `PresetManager.tsx`, `api.ts`, `styles.css`, `tests/features.spec.ts`
- Build passes; 2 Playwright tests passing

See `.squad/orchestration-log/` for agent run summaries. See `.squad/decisions/decisions.md` for full decision trail.

**Older learnings (2026-05-15 to 2026-05-24)** archived in `history-archive.md`: framework evaluation, ROI analysis, UI/UX competitive review, static prototype v1.0–v1.1 audit, API wiring, v1.3 presets, HuggingFace integration, UI perf fixes.

---

### Round 3.5 — Fresh Verification After Server Restart (2026-06-13)

**Context:** Dev server was restarted (PID 23392) because the previous instance was running on a stale webpack.config.js that didn't include the `Cache-Control: no-store` header from round 3. This pass is pure verification — no code changes.

**Bundle verification:**
- `nav-label`: ✅ FOUND in served `main.bundle.js`
- `max-width: 480px`: ✅ FOUND in served `main.bundle.js`
- `--breakpoint-mobile`: ✅ FOUND in served `main.bundle.js`
- Conclusion: The server IS serving the latest CSS with all mobile layout changes from rounds 1–3.

**Screenshot observations (390×844 @2x — iPhone 14 viewport):**

Chat page (`current-chat-mobile.png`):
- Nav bar: 7 icon-only buttons visible in a single row, no labels. All fit comfortably at top.
- Right-side controls: avatar circle (green "G"), sun/moon theme toggle, green status dot — all visible, not cramped.
- Welcome area shows "2 models loaded" message with 4 suggestion chips (2×2 grid).
- "LOADED RIGHT NOW" section shows 2 model cards in single-column layout — readable, no horizontal overflow.
- Composer at bottom: model selector dropdown, mode/preset/tools/logs pills, text input with send button. Slightly truncated model name in input placeholder but functional.
- No horizontal scrollbar visible.

Models page (`current-models-mobile.png`):
- Same nav bar at top (icons-only, all 7 fit).
- Page header: "Models" + stats ("2 running · 30 downloaded · 186 available") — fits on one line.
- Search bar, "+ Custom model" / "+ Omni collection" buttons in 2-column layout.
- Filter pills: All, LLM, Omni, Image, Audio, TTS on row 1; Embed wraps to row 2. Acceptable.
- Model cards: single-column, backend badge ("llama.cpp" / "SD.cpp") on LEFT of model name. Names readable — long ones truncated with ellipsis (e.g., "Qwen3.6-35B-A3B-GGUF-UD-Q8_K_...").
- Action buttons (RUNNING/Unload/×/▶) fit within card width.
- No horizontal scroll visible.

**Diagnosis:**
The served bundle contains ALL mobile layout changes. Screenshots confirm the layout renders correctly in Chromium at 390×844. If Kyle's phone still shows the old layout, the issue is **client-side caching in Safari on his phone** — the old bundle was served without `no-store` before the restart, so Safari may have cached it aggressively.

**Recommendation for Kyle:**
1. Hard refresh: Settings → Safari → Clear Website Data (or at minimum "Clear History and Website Data")
2. Or: In Safari address bar, long-press reload → "Request Desktop Site" then back to mobile (forces full re-fetch)
3. Or: Open in a Private/Incognito tab to bypass cache entirely
4. If none work: check if phone is hitting a different host/port (e.g., cached DNS, wrong IP)

### Round 4 — Mobile bottom sheet for conversations (2026-06-13)

**Context:** Kyle needs the conversation rail accessible on mobile (≤480px) as a slide-up bottom sheet. The rail is `display: none` at ≤768px, so conversations are inaccessible on phones.

**Approach chosen: Mobile-only trigger + separate bottom sheet element (CSS class toggle).**

Considered three approaches:
1. **CSS-only class toggle on the existing `<aside class="rail">`** — Would require overriding the 768px `display: none` and completely restyling the rail as a fixed bottom sheet. Too many conflicting CSS contexts; fragile.
2. **Portal-based extraction** — Over-engineered for a POC; adds React portal complexity.
3. **Separate bottom sheet div that repeats the conversation list JSX** — Cleanest separation. The `conversations` state, handlers (`handleNewChat`, `handleSelectConversation`, `handleDeleteConversation`), and `capabilityBadge`/`deriveTitle`/`timeAgo` helpers are already in scope. The JSX is short (~40 lines). This avoids touching the existing rail CSS and desktop behavior entirely.

Chose option 3.

**Implementation:**

1. **State:** Added `mobileSheetOpen` boolean + `sheetHandleRef` + `sheetTriggerRef` to `ChatView.tsx`.
2. **Mobile trigger button:** `.chat__mobile-rail-trigger` renders inside `.chat__main` with a hamburger icon + "Conversations" label. Hidden on desktop (`display: none`), shown at ≤480px as a sticky bar at the top of the chat main area.
3. **Bottom sheet markup:** `<div class="bottom-sheet">` with handle pill, "New Chat" button, and a `<ul class="bottom-sheet__list rail__list">` that maps over `conversations` identically to the sidebar rail. Rendered as a sibling after the `<aside class="rail">`.
4. **Backdrop:** Conditionally rendered `<div class="bottom-sheet-backdrop">` with click-to-close.
5. **CSS transitions:** `transform: translateY(100%)` → `translateY(0)` with 280ms ease-out. Sheet has `border-radius: 16px 16px 0 0`, max-height 80vh, internal scroll on the list.
6. **Drag-to-close:** `pointerdown`/`pointermove`/`pointerup` on the handle div. Tracks deltaY; if >100px on release, closes; else snaps back. No library.
7. **ESC key:** `useEffect` adds keydown listener when sheet is open; cleans up on close.
8. **Focus management:** On close, focus returns to `sheetTriggerRef`.
9. **Desktop unchanged:** The `.bottom-sheet` and `.chat__mobile-rail-trigger` are `display: none` above 480px. The existing rail toggle button still calls `handleRailToggle` which checks `window.innerWidth` — on desktop it toggles `railExpanded` as before.

**Screenshot verification (390×844 @2x, headless Chromium):**
- `sheet-closed-mobile.png`: Chat page with "☰ Conversations" trigger bar at top of main content. No bottom sheet. Layout matches previous rounds (no regression).
- `sheet-open-mobile.png`: Bottom sheet visible at bottom — drag handle pill centered at top, full-width "New Chat" button (accent color), "No conversations yet" placeholder. Semi-transparent dark overlay behind sheet.
- `sheet-desktop-check.png`: 1280×800 desktop — normal sidebar rail visible on left, no bottom sheet or mobile trigger visible. Behavior unchanged.

**Files changed (Round 4):**
- `prototype/ui-redesign/src/components/ChatView.tsx` — `mobileSheetOpen` state, `handleRailToggle` with width check, `closeMobileSheet`, ESC effect, drag-to-close effect, mobile trigger button JSX, bottom sheet JSX with conversation list
- `prototype/ui-redesign/src/styles/styles.css` — `.chat__mobile-rail-trigger` (hidden by default, shown at 480px), `.bottom-sheet-backdrop`, `.bottom-sheet` + `.bottom-sheet--open`, `.bottom-sheet__handle`, `.bottom-sheet__handle-pill`, `.bottom-sheet__new`, `.bottom-sheet__list` (all inside `@media (max-width: 480px)`)
- `prototype/ui-redesign/scripts/screenshot-bottom-sheet.mjs` — new; Playwright script for bottom sheet verification screenshots
