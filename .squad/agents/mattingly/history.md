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

### 2026-06-22: GUI3 Chat Rail + Account Menu A11y — feat/gui3-chat-account-a11y

**Task:** #2346 #2348 — conversation rail missing listbox keyboard nav; account menu declared dialog but missing aria-modal, focus trap, Escape, and focus restore. Branch `feat/gui3-chat-account-a11y`. Tests went from 50 → 58 passed, 0 failed.

**ChatView.tsx conversation rail — key facts:**
- `<ul role="listbox" aria-label="Conversations">` already existed with correct role/label. Only `aria-selected`, `tabIndex`, and keyboard handlers were missing.
- Roving tabindex pattern: selected conversation gets `tabIndex=0`; if no active conversation, the first item gets `tabIndex=0`; all others get `tabIndex=-1`.
- `handleRailKeyDown` uses `e.currentTarget` (the `<ul>`) to find `[role="option"]` children — no extra ref needed.
- `handleSheetKeyDown` is a separate callback (same logic but also calls `closeMobileSheet()` on Enter/Space) defined AFTER `closeMobileSheet` to avoid a stale closure on first render.
- Delete button: `tabIndex={-1}` removes it from Tab order within the listbox; `aria-label=\`Delete conversation: ${title}\`` qualifies it. NVDA browse mode can still reach and activate it.
- CSS `.rail__item:focus-within .rail__item-delete { opacity: 1 }` shows the delete button on keyboard focus (not just hover).
- Both desktop rail (`id="rail-conv-{id}"`) and mobile sheet (`id="sheet-conv-{id}"`) lists get the same treatment with separate IDs to avoid DOM duplicates.

**AccountMenu.tsx — modal dialog decision:**
- Component already had `role="dialog"` and `aria-haspopup="dialog"`. Going MODAL was the right call (multi-mode forms, complex interactions).
- `useFocusTrap(panelRef, open)` — existing hook, zero new deps. Focuses first focusable element (× button) on open, wraps Tab within the panel.
- `closePanel()` callback: `setOpen(false)` + `requestAnimationFrame(() => triggerRef.current?.focus())` for Escape and × button paths. Trigger click does NOT call `closePanel()` (focus is already on trigger after click).
- `autoFocus` on form inputs (sign-in, create modes) fires on mode-switch re-render AFTER the trap has focused the × button — works correctly, no conflict.
- Escape handler uses `e.stopPropagation()` to avoid bubbling to the bottom sheet Escape handler.

**Test pattern for conversation rail:**
- Use `page.addInitScript()` to inject `lemonade:guest:shared:conversations`, `lemonade:guest:shared:active_conversation`, and `lemonade:guest:shared:persist_conversations` into localStorage BEFORE page load.
- Conversation IDs must be stable (e.g., `rc1`, `rc2`) to use `#rail-conv-rc1` locators in tests.
- After `page.goto('/')`, `await page.waitForSelector('.rail__list')` before asserting.
- For ArrowDown test: call `.focus()` on the first option element, then keyboard.press('ArrowDown'), then check `document.activeElement.id`.

**Test count:** 50 → 58 passed. New tests: A35–A42 (8 tests, 2 describe blocks).

**Files changed:**
- `prototype/ui-redesign/src/components/ChatView.tsx`
- `prototype/ui-redesign/src/features/accounts/AccountMenu.tsx`
- `prototype/ui-redesign/src/styles/styles.css`
- `prototype/ui-redesign/tests/a11y.spec.ts`
- `prototype/ui-redesign/ACCESSIBILITY.md`

---

### 2026-06-22: GUI3 Download Progress A11y — feat/gui3-download-a11y

**Task:** #2342 — download progress was visual-only (bare `aria-label="NN%"` on a `<div>`). Branch `feat/gui3-download-a11y`. Tests went from 50 → 54 passed, 0 failed.

**DownloadManager.tsx structure key facts:**
- `DownloadManager` always has its hooks run (state/effects never skip) but returns `null` when `isVisible=false` — the "conditional return after hooks" pattern. This means the `prevStatusRef` and `statusAnnouncement` state persist across open/close cycles.
- `downloadStore` is a MODULE-LEVEL SINGLETON instantiated at import time. Its constructor calls `readStored()` which reads from `localStorage` immediately. Pre-populating `localStorage` via `page.addInitScript()` BEFORE page load is the reliable way to inject a download item in tests — no API timing required.
- The store calls `api.downloads()` only when `api.isConnected === true`. First poll on `start()` often runs before the health check returns; the store schedules `POLL_MS=1000` retry. For tests that need real API data, wait for `page.waitForResponse('/api/v1/downloads')`.
- `mergeDownloads([], true)` (empty server response + `includeStored=true`) does NOT clear existing active items — it re-reads localStorage AND in-memory state. Safe to mock downloads with empty response while keeping localStorage-injected items.

**A11y patterns used:**
1. **`role="progressbar"`** with `aria-valuenow={Math.round(percent)}`, `aria-valuemin={0}`, `aria-valuemax={100}`, and `aria-label` including the model name. Visual `<span>` gets `aria-hidden="true"`.
2. **Status live region** — `role="status" aria-live="polite" aria-atomic="true"` sr-only div always present inside the panel (same pattern as BackendManager toasts). `prevStatusRef` tracks previous statuses; the effect only sets announcement on status TRANSITIONS, not on percent updates. Cleared on panel close via `useEffect([isVisible])`.

**Key insight on live regions (reiterated):**
- The live region must be always present in the DOM while the panel is open — conditional mounting (`{msg && <div aria-live>}`) does NOT trigger NVDA.
- Since `DownloadManager` is conditionally RETURNED (not conditionally mounted), the live region only exists in the DOM when `isVisible=true`. This is correct — we want status announcements only when the panel is open, and the always-present pattern is satisfied within the panel's render.

**Test pattern for DownloadManager:**
- Use `page.addInitScript((item) => { localStorage.setItem('lemonade_download_manager_items_v1', JSON.stringify([item])); }, MOCK_DOWNLOAD)` to inject a fake download BEFORE page load.
- Mock health (`{ status: 'ok', all_models_loaded: [] }`) to avoid errors; mock downloads (`{ downloads: [] }`) to prevent the store from clearing the localStorage item.
- After `page.goto('/')`, click `.titlebar__download-toggle` to open the panel, then `waitForSelector('.download-item--downloading')` before asserting.
- `Date.now()` timestamps in the mock item are fine — `downloading` status is never pruned by TTL.

**Test count:** 50 → 54 passed. New tests: A35–A38 (4 tests, 1 `describe` block).

**Files changed:**
- `prototype/ui-redesign/src/components/DownloadManager.tsx`
- `prototype/ui-redesign/tests/a11y.spec.ts`
- `prototype/ui-redesign/ACCESSIBILITY.md`

---

### 2026-06-22: GUI3 Backend Manager A11y — feat/gui3-backend-a11y

**Task:** #2343 #2344 #2351 — keyboard-operable matrix cells, qualified action button names, live-region toasts. Branch `feat/gui3-backend-a11y`. Tests went from 50 → 58 passed, 0 failed.

**BackendManager.tsx structure key facts:**
- Matrix is a `<table>` inside `.matrix[data-backends-matrix]`. Each `<td>` can contain multiple `.cell.cell--selectable` divs (one per recipe:backend combo in that device×capability cell).
- Cell key: `backendKey(recipe, backend)` → `"${recipe}:${backend}"` (e.g., `"llamacpp:vulkan"`). Used as `data-cell` attribute.
- `selectedBackendKey` state holds the currently selected cell key. Selection toggles on click.
- `RECIPE_LABELS` maps recipe id → human label (e.g., `llamacpp` → `'llama.cpp'`).
- Action buttons live inside `.cell__actions` div (stopPropagation sibling to the select button).
- `toastMsg` drives the visual toast (conditionally rendered). `presetNotice` drives the context-rail notice.
- Both `toastMsg` and `presetNotice` are set via `setTimeout(null, ...)` — they appear and disappear transiently.

**A11y patterns used:**
1. **Overlay button (`cell__select-btn`)** — same pattern as `recipe-card__overlay-btn` in PresetManager. `position: absolute; inset: 0; z-index: 0`. Cell div needs `position: relative` added to `.cell--selectable`. Action buttons get `position: relative; z-index: 1`. The overlay button has `aria-pressed` + `aria-label`.
2. **`aria-label` on action buttons** — `Install/Update/Uninstall ${RECIPE_LABELS[recipe] || recipe} (${backend})`. Same direct-label approach as ModelManager fix.
3. **Always-present sr-only live region** — one for `toastMsg` (`data-backends-toast-live`), one for `presetNotice` (`data-backends-preset-notice-live`). Visual elements remain conditionally rendered. Content mirrors the state value; empty string when null.

**Key insight on live regions:**
- Conditionally mounting `{msg && <div aria-live="polite">{msg}</div>}` does NOT trigger NVDA — the element must exist in DOM BEFORE content changes.
- Pattern: always render `<div role="status" aria-live="polite" aria-atomic="true" className="sr-only">`, set content to `{msg || ''}`. Visual element stays conditional (animation, visual only).
- Use `role="status"` (maps to implicit `aria-live="polite"`) + explicit `aria-live="polite"` for belt-and-suspenders. Add explicit `aria-atomic="true"` for toast messages.

**Overlay button semantics: button vs grid:**
- A `<button aria-pressed>` was chosen over `role="grid"` + `role="gridcell"` because:
  1. Table already has implicit table semantics. Adding `role="grid"` switches AT to application-mode reading (changes arrow-key behavior for all cells), which would be more disruptive than helpful for a matrix with very few focusable cells.
  2. The overlay-button pattern is already established in the codebase (PresetManager).
  3. `aria-pressed` is semantically correct: the cell toggles a selection state.
  4. Arrow-key navigation not warranted here — the matrix is small (≤4 columns × ≤5 rows) and Tab navigation is sufficient.
- Decision recorded in `.squad/decisions/inbox/mattingly-gui3-backend-a11y.md`.

**Test mocking for BackendManager:**
- Mock `/api/v1/system-info**` (not health — BackendManager calls `api.health().catch(() => null)` and proceeds regardless).
- Wait for `[data-backends-matrix]` after navigation (not `[data-backends-matrix-empty]` which appears when no data).
- Mock needs `devices.cpu.available: true` and at least one recipe with backends in `installable` and `installed` states to get visible Install/Uninstall buttons.
- Cell key in mock: `llamacpp:vulkan` (installable → Install button), `llamacpp:cpu` (installed, can_uninstall: true → Uninstall button).
- `BACKEND_DEVICE['vulkan'] = 'gpu'` (referenced but not detected) and `BACKEND_DEVICE['cpu'] = 'cpu'` (detected). Both appear in matrixRows.

**Test count:** 50 → 58 passed. New tests: A35–A42 (8 tests, 1 `describe` block).

**Files changed:**
- `prototype/ui-redesign/src/components/BackendManager.tsx`
- `prototype/ui-redesign/src/styles/styles.css`
- `prototype/ui-redesign/tests/a11y.spec.ts`
- `prototype/ui-redesign/ACCESSIBILITY.md`



**Task:** #2341 — model row action buttons had generic accessible names colliding across rows (Load/Load/Load…). Branch `feat/gui3-models-a11y`. Tests went from 50 → 55 passed, 0 failed.

**ModelManager.tsx structure key facts:**
- Three render functions produce row action buttons: `renderRunningModel` (loaded), `renderModelRow` (downloaded/registry), `renderHfRow` (HuggingFace search results).
- `renderModelRow(m, isDownloaded)` — `isDownloaded: true` shows Load+Delete; `false` shows Download+Get&Load.
- `downloaded: true` on a model in the API response puts it in `dl` (downloaded zone); omitting it puts it in `av` (registry/available zone).
- `renderPinAndSpeechControl(name, isPinned, capability)` — renders Pin (all models) and TTS speech toggle (TTS-capability models only). `name` is the first arg.
- CopyInlineButton uses its `title` prop as `aria-label`; pass `title={`Copy model ID: ${name}`}` at the call site.
- HF variant buttons are inside `renderHfRow` expanded section; `r.id` is the repo identifier, `v.name` is the variant filename.

**A11y patterns used:**
1. **`aria-label` directly on `<button>`** — for all action buttons. Computed from the in-scope `name` / `m.model_name` / `r.id` variable. No restructuring needed.
2. **`title` prop on CopyInlineButton call sites** — the component forwards `title` to `aria-label`; pass a qualified title at each call site rather than changing the component.
3. **Icon-only buttons** — Delete and Cancel-download already had a `title` attribute for tooltip; added `aria-label` alongside (`title` stays for pointer users, `aria-label` for AT).

**Test pattern:**
- Mock both `/api/v1/health` (returns `{ status: 'ok', all_models_loaded: [] }`) AND `/api/v1/models**` (returns test models with `downloaded: true/false`) in `test.beforeEach`.
- Health mock is required: `refresh()` in ModelManager checks `api.isConnected` (set by the health call) before fetching models. Without the health mock, the models mock never fires.
- After navigating to Models, use `page.waitForSelector('.row', { timeout: 5000 }).catch(() => {})` instead of a fixed timeout.
- `page.getByRole('button', { name: /Load Llama-3\.1-8B/ })` matches against the computed accessible name (aria-label takes priority over text content).

**Gotchas:**
- `renderRunningModel` uses a *second* `info` variable inside the expanded `{expandedModel === m.model_name && (() => { ... })()}` IIFE — different scope from the `info` at the function top. The delete button's `title` uses the outer `info`, so `aria-label` on the same button can reference `m.model_name` directly without ambiguity.
- The `downloaded` field in the API response is checked via `Boolean((m as any).downloaded)` — must be explicitly `true`; omitting the field or passing `false` routes to the registry zone.
- `gh pr create --body` breaks on markdown with backticks and regex patterns; always use `--body-file` with a temp file.

**Test count:** 50 → 55 passed. New tests: A35–A39 (5 tests, 1 `describe` block).

**Files changed:**
- `prototype/ui-redesign/src/components/ModelManager.tsx`
- `prototype/ui-redesign/tests/a11y.spec.ts`
- `prototype/ui-redesign/ACCESSIBILITY.md`

### 2026-06-22: GUI3 Preset Accessibility — feat/gui3-presets-a11y

**Task:** Address 5 NVDA screen-reader issues on PresetManager.tsx (issues #2338 #2339 #2345 #2350 #2352). Branch `feat/gui3-presets-a11y`. All 5 items shipped in one pass; tests went from 50 → 61 passed, 0 failed.

**PresetManager.tsx structure key facts:**
- `DEFAULT_PRESET` is always the first card; its slideover hides the Behavior fields (`isDefaultEmptyPreset` guard at line ~858).
- Use `page.locator('.recipe-card').nth(1)` in tests to open the first STARTER (chat capability, shows Behavior fields).
- `STARTERS` array order: Balanced, Quality, Fast, Creative, Long Context, Code (chat), Sharp, Quick (image).
- `CAPABILITIES = ['all', 'chat', 'image', 'tts']` (4 options, always rendered in capability section).
- `toggleCap(cap)` always sets `appliesTo = [cap]` (single-select, not multi-select toggle) — radio semantics are correct, not aria-pressed.
- `AUTO_OPT_RUNS` has 3 items; first is selected by default via `useState(AUTO_OPT_RUNS[0]?.id)`.
- Advanced fields (`llamacpp_backend`, `llamacpp_device`, etc.) are inside a `<details class="preset-advanced">` — tests must open it first with `.preset-advanced summary` click.
- The context-size slider is a "logical index" slider (maps to `ctxOptions[idx]`), not a direct value slider.

**A11y patterns used:**
1. **`aria-describedby` + `sr-only` span** — for the card button metadata (#2345). Description is hand-authored from `paramsPreviewLines`, `promptDisplayText`, `toolsDisplayText`. Kept `aria-hidden` on visual params/behavior divs to avoid duplication.
2. **`role="radiogroup"` + `role="radio"` + `aria-checked`** — for capability chips (#2350). Correct because toggleCap is single-select.
3. **`aria-pressed`** — for AutoOpt run buttons (#2352). Preferred over radiogroup because the run cards are structurally complex (article + button + details); adding radiogroup semantics would require DOM restructuring.
4. **`htmlFor`/`id`** — for all field labels (#2338). Used static IDs since only one preset slideover is open at a time. Exception: the "Image width × height" shared visual label — used `aria-label` directly on each of the two inputs.
5. **`<input list=> + <datalist>`** — for llamacpp_backend and llamacpp_device (#2339). Chosen over `<select>` because these accept arbitrary free-text, not just the listed values. Constants `LLAMACPP_BACKENDS` and `LLAMACPP_DEVICES` added near top of file.

**Gotchas:**
- `<button role="radio">` is valid per ARIA in HTML spec (button allows role override to radio). axe-core accepts it as long as `aria-checked` is present and a `role="radiogroup"` parent exists.
- `aria-pressed` on AutoOpt buttons uses `{selectedAutoRunId === run.id}` — a boolean expression, which React serializes as `"true"` or `"false"` string (both valid aria-pressed values).
- The `PresetCard` was an arrow function with implicit return `(...)`. Converting to function body required changing `) =>` to `) => {` and adding `return (`, and changing the closing `);` to `  );\n};`.
- datalist IDs must be globally unique on the page. Used `preset-llamacpp-backends` and `preset-llamacpp-devices` (non-suffixed) because only one slideover is open at a time.

**Test count:** 50 → 61 passed. New tests: A35–A45 (11 tests across 5 issues).

**Files changed:**
- `prototype/ui-redesign/src/components/PresetManager.tsx`
- `prototype/ui-redesign/tests/a11y.spec.ts`
- `prototype/ui-redesign/ACCESSIBILITY.md`

### 2026-06-19: Presets redesign audit — screenshots + findings

**Task:** Design audit of the Presets UI in response to Kyle's stakeholder feedback: "I can't even figure out how to edit the default starters" and the preset↔recipe integration gap.

**Deliverables committed:**
- `prototype/ui-redesign/docs/PRESETS_REDESIGN.md` — full audit + three-part recommendations
- `prototype/ui-redesign/scripts/screenshot-presets.mjs` — reusable Playwright screenshot script
- `prototype/ui-redesign/docs/screenshots/presets/*.png` — 12 screenshots (desktop + mobile)
- `.squad/decisions/inbox/mattingly-presets-redesign-audit-2026-06-19.md` — Scribe pickup

**Top findings:**
1. `const isReadOnly = preset.starter` (PresetManager.tsx:749) makes all starter fields `disabled`. The only affordance to customize a starter is `Clone` at the bottom of a long slideover — no on-card CTA. Kyle's pain is 100% code-confirmed.
2. `engine_hint` is present in all STARTERS but invisible to users — it never appears on cards and lives in a collapsed `<details>` labeled "Advanced engine options". A preset cannot _drive_ recipe selection.
3. Mobile: the AutoOpt summary block with full CLI args dominates ~40% of first viewport at 390px, pushing starters below the fold.

**Top recommendations:**
1. Phase A: Add "Customize →" on-card CTA for starters (calls existing `handleClone`). S effort.
2. Phase B: Rename `engine_hint` → `recipe_preference`, surface on cards, add recipe picker in slideover body, add mismatch dialog at apply-time.
3. Phase A: Wrap AutoOpt summary in `<details>` collapsed by default on mobile.

**Screenshot script pattern — reusable for future audits:**
The script `scripts/screenshot-presets.mjs` establishes the standard pattern:
- `playwright.chromium.launch()` (headless) — no need to reference `@playwright/test`'s test runner
- Desktop context: `viewport: {width:1440, height:900}`, no isMobile
- Mobile context: `viewport: {width:390, height:844}`, `deviceScaleFactor:2`, `isMobile:true`, **`hasTouch:true`** (required for `.tap()` calls — omitting this throws "page does not support tap")
- Output to `docs/screenshots/<feature>/` with numeric prefixes for sort order
- Use `page.waitForTimeout(600)` after nav clicks; `waitUntil:'networkidle'` on goto

**Files changed this session (read-only audit + docs only):**
- `prototype/ui-redesign/docs/PRESETS_REDESIGN.md` — new
- `prototype/ui-redesign/docs/screenshots/presets/*.png` — 12 new screenshots
- `prototype/ui-redesign/scripts/screenshot-presets.mjs` — new
- `.squad/decisions/inbox/mattingly-presets-redesign-audit-2026-06-19.md` — new

### 2026-06-15: Regression — #2228 accidentally reverted by rebase linearization (c6529721)

**What happened:** Commit `115d464f` (PR #2228) added capability badge icons (flame/wrench/brain/rocket), `reasoningElapsedMs` timing plumbing, and tool polish to `lemonadeTools.ts`. Hours after merge, a "rebase ui-mobile-layout onto ui-testing" linearization extracted prototype/ files from a merge tree that did NOT include #2228 and pushed them back onto `kpoin/ui-testing` via PR #2229 (commit `c6529721`). Result: 7 files silently reverted to a pre-#2228 state while keeping the a11y + mobile additions.

**How it was detected:** Kyle reviewed the UI on 2026-06-15 and noticed capability badges had no icons, reasoning summary showed no timing, and lemonadeTools was missing preset/collection logic added in #2228.

**Resolution:** Cherry-picked `115d464f` onto `kpoin/ui-testing` at `5c4ecdc2`, resolved all 7 whole-file conflicts by taking HEAD (a11y+mobile) as the base and applying each #2228 diff hunk manually. TypeScript passed; all content markers verified. Pushed as `2d8c45f0`.

**How to detect earlier:** After any rebase/linearization or big merge that touches prototype/, immediately grep for known content markers from recent PRs:
- `flame`, `wrench`, `brain`, `rocket` (Icon.tsx capability icons)
- `reasoningElapsedMs` (api.ts reasoning timing)
- `allStoredPresets` (lemonadeTools.ts preset integration)
If any of these are missing, a regression occurred. A git-log check (`git log --oneline --diff-filter=M -- prototype/`) can also surface unexpected large changes to prototype files after a merge.

**Downstream note:** `kpoin/ui-mobile-layout` and `kpoin/ui-accessibility` will need to be re-merged with `kpoin/ui-testing` to pick up the re-applied #2228 content. That merge decision belongs to Kranz/Kyle — not acted on here.



Wrote `prototype/ui-redesign/tests/a11y.spec.ts` — 34 Playwright tests covering all Phase 1 + Phase 2 a11y items shipped earlier on this branch. Also installed `@axe-core/playwright` (not previously in devDeps) and added `npm run test:a11y` script.

**Test groups:**

- **A01–A05 — axe-core WCAG 2.1 AA scans:** `new AxeBuilder({ page }).withTags(['wcag2a','wcag2aa','wcag21a','wcag21aa']).analyze()` on Chat, Models, Presets, Connect, Dashboard. Filters to `critical | serious` violations only — moderate/minor are noise in a POC. Failure message includes the violation ID, description, and impact level for fast triage.

- **A06–A09 — Skip link:** Tab-once-from-load test; off-screen bounding box check (top: -40px → `box.y + box.height ≤ 0`); focus ring present when tabbed; Enter moves focus to `#main-content`. One subtle gotcha: `.skip-link:focus { top: var(--space-2) }` uses `:focus` not `:focus-visible`, so both keyboard and (theoretical) mouse focus make it visible — intentional for a skip link.

- **A10–A13 — Landmarks:** Simple count/attribute assertions. These are the cheapest tests and catch regressions from DOM refactors.

- **A14–A16 — Keyboard nav:** Tab counting is inherently fragile so I used generous thresholds (12 Tabs for nav, 40 for textarea) rather than exact counts. This avoids test rot when new interactive elements are added upstream.

- **A17–A20 — Bottom sheet focus trap (mobile):** Requires `page.setViewportSize({ width: 390, height: 844 })` because `.bottom-sheet { display: none }` at desktop widths. Uses `.evaluateAll()` with the same FOCUSABLE selector as `useFocusTrap.ts` to count trapped elements, excluding `aria-hidden="true"` ancestors. In the empty state there's only 1 focusable element (`bottom-sheet__new`), so the Tab-wrap test does 1 press and confirms focus stays inside (wraps to itself). The ESC + focus-return test relies on `sheetTriggerRef.current?.focus()` in `closeMobileSheet` — I verified this in the source before writing the test.

- **A21–A24 — Preset slideover focus trap:** Opened by clicking `.recipe-card`. The `triggerRef` in PresetManager stores `document.activeElement` at open time (the clicked article element), then calls `requestAnimationFrame(() => triggerRef.current?.focus())` on close. A24 waits 200ms for the rAF. Closeness check: `el.classList.contains('recipe-card') || el.closest('.recipe-card') !== null` to handle both the article and any focused child inside it.

- **A25–A27 — aria-live regions:** Both regions exist in DOM at page load (ChatView renders them unconditionally outside of any conditional branch). Both are `.sr-only` (1×1 px bounding box). **Streaming content verification (debounced content in polite region, 'Assistant is responding' in assertive) is a TODO** — it needs a `page.route()` SSE mock for `POST /api/v1/chat/completions`. That mock is non-trivial and was left for a follow-up task.

- **A28–A30 — :focus-visible rings:** Key insight: `:focus-visible` fires on keyboard Tab for `<button>` but NOT on `page.mouse.click()`. After removing the global `outline: none` reset in Phase 1, the only outline source is the `:focus-visible { outline: 2px solid var(--accent) }` rule. Mouse-click on a button → `outlineWidth === '0px'`. Tab to same button → `outlineWidth === '2px'`.

- **A31–A34 — prefers-reduced-motion:** `page.emulateMedia({ reducedMotion: 'reduce' })` before `goto`. `getComputedStyle` on `.bottom-sheet` for `transitionDuration` at 480px viewport — expect `< 0.01s` (the rule sets `0.01ms !important`). Normal mode expects `> 0.1s` (280ms = 0.28s). A34 checks `transform: none` on the sheet (the explicit bottom-sheet rule in the reduce block, separate from the wildcard transition rule). Normalised duration helper handles both `'0.28s'` and `'280ms'` string formats from `getComputedStyle`.

**Commits:**
1. `79db3bd5` — dep + script (package.json only; package-lock.json is gitignored in this repo)
2. `f6f6b75a` — a11y.spec.ts + ACCESSIBILITY.md "Running the tests" section

**Files changed:**
- `prototype/ui-redesign/tests/a11y.spec.ts` — new, 34 tests
- `prototype/ui-redesign/package.json` — @axe-core/playwright devDep + test:a11y script
- `prototype/ui-redesign/ACCESSIBILITY.md` — "Running the Accessibility Tests" section appended

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

### 2026-06-14: Phase 1 + Phase 2 accessibility implemented (branch kpoin/ui-accessibility)

**All Phase 1 items shipped. Phase 2 items 11–15 shipped; items 16–18 deferred to Phase 3.**

#### Phase 1 — what shipped

1. **Skip link** (`App.tsx`, `styles.css`) — `<a href="#main-content" class="skip-link">` as first child of root; visible on `:focus` via `top: var(--space-2)` transition; hidden otherwise (`top: -40px`).
2. **`<main>` landmark** (`App.tsx`) — `<div className="view-container">` replaced with `<main id="main-content" className="view-container">`. App return now wrapped in fragment to accommodate the skip link sibling.
3. **Focus rings** (`styles.css`) — Removed `outline: none` from the `input, textarea` reset block. Added global `:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }`. Added `.slider:focus-visible { outline: none; }` exception since sliders use thumb box-shadow instead.
4. **Composer textarea `aria-label`** (`ChatView.tsx`) — `aria-label="Message"` on the main textarea.
5. **Persistence checkbox** — Already had implicit `<label>` wrapping (`<label className="rail__privacy-toggle">` wraps both the `<input>` and the `<span>` text). No change needed; confirmed compliant.
6. **Preset slideover input labels** (`PresetManager.tsx`) — `aria-label="Preset name"` on title input, `aria-label="Description"` on description textarea.
7. **Preset slideover dialog semantics** (`PresetManager.tsx`) — Added `role="dialog" aria-modal="true" aria-label="Preset details"` to the `<aside>`. ESC closes via `useEffect` keydown listener that fires when `selectedPreset` is truthy.
8. **Prefers-reduced-motion** (`styles.css`) — `@media (prefers-reduced-motion: reduce)` block at end of file: sets all `animation-duration`, `animation-iteration-count`, `transition-duration` to `0.01ms !important`; adds `scroll-behavior: auto !important`; adds `.bottom-sheet { transform: none !important }` so sheet snaps open/closed without the slide animation.
9. **Status dot ARIA** (`App.tsx`) — Added `role="status"` and `aria-label` (matching the `title` value) to `<span className="titlebar__status-dot">`.

#### Phase 2 — what shipped

10. **`useFocusTrap` hook** (`src/hooks/useFocusTrap.ts`) — New file. Custom hook; no new npm dep. Collects all focusable children, focuses first on activation, traps Tab/Shift+Tab at boundaries. Filters out children inside `aria-hidden="true"` ancestors to avoid focusing screen-reader-hidden elements.
11. **Bottom sheet focus trap** (`ChatView.tsx`) — `useRef<HTMLDivElement>(null)` on the `.bottom-sheet` div; `useFocusTrap(bottomSheetRef, mobileSheetOpen)` activates when sheet opens.
12. **Preset slideover focus trap + focus return** (`PresetManager.tsx`) — `useRef<HTMLElement>(null)` on the slideover `<aside>`; `useFocusTrap(slideoverRef, !!selectedPreset)`. Added `triggerRef` that captures `document.activeElement` before `setSelectedPreset` is called; `closeSlideover()` calls `requestAnimationFrame(() => triggerRef.current?.focus())` so focus returns to the card that opened the panel.
13. **div→button conversions** (`ModelManager.tsx`) — Three `div.row__content` elements with `onClick` converted to `<button type="button" className="row__content">` with `aria-expanded`. Interactive children (`CopyInlineButton`) moved outside the button to avoid nesting interactive elements. Added `.row__summary` wrapper div and CSS so the copy button renders inline beside the button. BackendManager's `div.cell__actions` left as-is — it is a non-interactive container that only calls `e.stopPropagation()`.
14. **`aria-live` for streaming** (`ChatView.tsx`) — Two hidden `<div aria-live>` regions appended inside the fragment wrapper. `aria-live="assertive"` announces "Assistant is responding" on stream start and "Response complete" on stream end. `aria-live="polite"` receives debounced flush of `streamingContent` (400ms default; 100ms on sentence/clause boundary detection via `[.!?\n]` on last 2 chars). Both use `.sr-only` class for visual hiding. Timer cleaned up on unmount.
15. **Color contrast fixes** (`tokens.css`, `styles.css`) — `--text-disabled` dark: `#5C594F` → `#7A776E` (~4.6:1 on `--surface-base`). `--text-disabled` light: `#999999` → `#767676` (exactly 4.5:1 on white). New `--accent-fg` token: `var(--accent)` in dark (yellow on dark surface = passes), `var(--accent-deep)` in light (avoids yellow-on-white 1.4:1 failure). All `color: var(--accent)` and `border-color: var(--accent)` foreground uses in styles.css migrated to `var(--accent-fg)` to gate light-theme contrast automatically.

#### Phase 2 — what was deferred to Phase 3

- **Item 16 — Keyboard shortcut system** (`2.7`): Scope exceeded Phase 2 budget; requires global hotkey registry, cheat sheet modal, and guard logic for input focus. Deferred.
- **Item 17 — Font scale control** (`2.1`): Requires settings panel + `--font-scale` token + A−/A+ UI. Deferred.
- **Item 18 — Message article/ol structure** (`1.1.3`): Large ChatView refactor affecting message rendering across 5+ components. Deferred.
- **Partial: focus trap scope** (`1.4.2`): Traps implemented for bottom sheet and preset slideover. Composer model-search menu and AccountMenu dialog still lack focus traps — noted as remaining in ACCESSIBILITY.md.
- **Partial: ESC coverage** (`1.3.2`): Preset slideover has ESC. Composer model-search menu and AccountMenu still need ESC handlers.

#### Tricky bits

- **aria-live debounce vs token spam:** Token-by-token `aria-live` updates would interrupt screen readers on every token. The 400ms debounce with sentence-boundary fast-path (100ms) gives a good balance — screen reader hears complete phrases rather than individual tokens or one giant dump at the end.
- **Focus trap + aria-hidden:** The useFocusTrap FOCUSABLE selector alone would match elements inside `aria-hidden="true"` regions (e.g. the backdrop). Added `.filter(element => !element.closest('[aria-hidden="true"]'))` to exclude them.
- **row__content div→button:** Converting the expandable row trigger to a `<button>` required extracting `CopyInlineButton` (itself a button) out of the row content to avoid a button-inside-button HTML violation. Added a `.row__summary` wrapper grid to lay them side by side cleanly.
- **--accent-fg token strategy:** Rather than a `[data-theme="light"]` one-off override on each failing rule, introduced `--accent-fg` as a semantic alias that resolves to `--accent` in dark and `--accent-deep` in light. One token swap, all instances fixed at once.

**Files changed:**
- `prototype/ui-redesign/src/App.tsx` — skip link, `<main>`, status dot ARIA
- `prototype/ui-redesign/src/components/ChatView.tsx` — textarea label, aria-live regions, bottom-sheet focus trap
- `prototype/ui-redesign/src/components/ModelManager.tsx` — div→button row conversions
- `prototype/ui-redesign/src/components/PresetManager.tsx` — dialog semantics, ESC, focus trap, focus return, input labels
- `prototype/ui-redesign/src/hooks/useFocusTrap.ts` — new file
- `prototype/ui-redesign/src/styles/styles.css` — focus rings, skip link, sr-only, reduced motion, accent-fg migration, row__summary
- `prototype/ui-redesign/src/styles/tokens.css` — --text-disabled fixes, --accent-fg token
- `prototype/ui-redesign/ACCESSIBILITY.md` — status updated, Phase 1/2 items marked ✅ DONE

### 2026-06-14: Accessibility plan drafted (branch kpoin/ui-accessibility)

**Document created:** `prototype/ui-redesign/ACCESSIBILITY.md` — planning/roadmap doc, no code changed.

**Current a11y state assessment (as of HEAD on kpoin/ui-accessibility):**

The prototype has a mixed accessibility posture. Several good patterns are in place — `<nav aria-label="Primary">`, `<aside>` for rail/logs panels, nav button `aria-label` (added Round 2), bottom sheet with `role="dialog" aria-modal="true"` and ESC/focus-return, `ConnectView` with explicit `htmlFor` label associations. However there are significant gaps:

- **P0 gaps:** No `<main>` landmark; no skip link; global `outline: none` suppresses all focus rings (except sliders); composer textarea has no `aria-label`; preset slideover lacks `role="dialog"` and focus trap; `div.onClick` elements in AccountMenu, BackendManager, ModelManager, PresetManager have no keyboard activation; **zero `aria-live` regions** — streaming output is completely inaccessible to screen readers; no `@media (prefers-reduced-motion)` guard on any animation or transition.
- **P1 gaps:** `--text-disabled` tokens in both themes estimated below 4.5:1; yellow accent as foreground text on light theme may fail; preset slideover name/desc inputs have no label; persistence checkbox in bottom sheet has no label; LogViewer search input unconfirmed.
- **Good news:** The token system (`tokens.css`) makes a `--font-scale` multiplier straightforward to add. The `[data-theme]` attribute pattern makes a high-contrast theme slot-in easy. Existing motion tokens (`--duration-*`) make `prefers-reduced-motion` a single override block.

**Plan summary (19 items across 3 phases):**
- Phase 1 (S-effort quick wins): 10 items — landmarks, skip link, focus ring, aria-labels, reduced motion block
- Phase 2 (M-effort structural): 8 items — div→button conversion, focus traps, aria-live streaming, contrast audit, shortcuts, font scale
- Phase 3 (L-effort enhancements): 4 items — high-contrast theme, Lexend font, verbosity settings, message role polish


### 2026-06-15: Test failures triage and fix (5 reported + 1 pre-existing)

**Branch:** kpoin/ui-testing (commit range c225d4bd–c0101518)
**Requested by:** Kyle (kpoin)

External collaborator reported 5 test failures on the branch. Investigation found 6 total failures (A05 was also failing but not reported — confirmed by git-stash-and-run before fixing).

#### Failure inventory and root causes

**A01 (tests/a11y.spec.ts:75) — Chat view axe violations (3 violations):**
- ria-input-field-name: <ul role="listbox"> in ChatView had no accessible name
- ria-required-children: <li class="rail__empty"> (empty state, no role) inside listbox
- listitem: <li> inside a listbox container (not a valid list parent)
Root cause: empty-state element was rendered as <li> inside <ul role="listbox">. Listbox requires all children to be ole="option".
Fix: added ria-label="Conversations" to both listbox elements (desktop rail + mobile bottom sheet); moved empty-state from <li> inside <ul> to <p> sibling after </ul>. Applied to both rail and mobile bottom-sheet variants.

**A03 (tests/a11y.spec.ts:103) — Presets view axe violation + test selector bug:**
- Test waitForSelector('.manager') was waiting for ModelManager's .manager div, which is display:none in the Presets view — timed out after 60s.
- After fixing selector to [data-view="presets"], axe found: 
ested-interactive — every <article role="button" tabIndex=0> preset card contained <button> children (Clone/Apply/Export actions). WCAG 4.1.2 forbids interactive controls nested inside other interactive controls.
Fix: Updated waitForSelector to [data-view="presets"]. Replaced ole="button" tabIndex=0 on <article> with .recipe-card__overlay-btn — a real <button> positioned bsolute; inset: 0; z-index: 0 that covers the entire card. Card content + action buttons elevated to z-index: 1 so pointer events reach the correct target. ocus-within behavior (reveals actions) continues to work because focusing the overlay button fires :focus-within on the article parent.

**A05 (tests/a11y.spec.ts:133) — Dashboard view color-contrast (pre-existing, not in report):**
- dash2-slot-legend__item--idle { opacity: 0.5 } was compositing text colors against the dark background, dropping computed contrast to 1.93–3.44:1 (need 4.5:1 for 12px normal text).
Fix: Removed opacity: 0.5. Added explicit color overrides: idle label → --text-tertiary; idle TPS → --text-tertiary; active TPS → --text-primary. --text-tertiary: #A8A39A and --text-secondary: #C7C2B5 both meet 4.5:1 against --surface-base: #1a1813 at full opacity.

**A09 (tests/a11y.spec.ts:201) — Skip link Enter not moving focus to #main-content:**
Root cause: <main id="main-content"> was not focusable. In-page anchor navigation moves focus to the target element only if it has a non-negative tabindex or is natively focusable. <main> is not natively focusable.
Fix: Added 	abIndex={-1} to <main id="main-content"> in App.tsx. 	abIndex=-1 makes the element programmatically focusable without adding it to the tab order.

**A29 (tests/a11y.spec.ts:547) — Mouse-clicked button shows 3px outline:**
Diagnosis: utton {} reset did not include outline. Chromium UA stylesheet has utton:focus { outline: 3px ... } (pre-:focus-visible rule) which shows even on mouse clicks. Our outline: 2px solid var(--accent) was only set on :focus-visible. outline: none doesn't help because CSS sets outline-style: none but leaves outline-width at UA default (3px); getComputedStyle().outlineWidth returns 3px even with style:none.
Fix: Changed button reset to outline: 0 (sets outline-width: 0 via shorthand). Mouse click → outline-width: 0px ✓. Keyboard Tab → :focus-visible { outline: 2px } overrides → 2px ✓.

**features/15 (tests/features.spec.ts:557) — Dashboard nav button not found:**
Root cause: nav label was 'Dash'; test called getByText('Dashboard').
Fix: Changed label from 'Dash' to 'Dashboard' in App.tsx nav items array.

#### Commit sequence
1. c225d4bd — tabIndex=-1 on main + Dashboard nav label
2. 1e14f3a7 — button outline:0 reset + Dashboard contrast + overlay button CSS
3. 7860774d — ChatView rail listbox fixes
4. 62391810 — PresetCard overlay button pattern
5. c0101518 — test selector fix for A03

#### Patterns to watch for
- **opacity on text containers fails axe contrast.** Use explicit accessible color tokens instead. Opacity compositing is computed against the actual background color by axe — the rendered color may differ greatly from the token value.
- **<ul role="listbox"> must only contain ole="option" children.** Empty states, separators, and headings must be outside the listbox element or use ole="presentation".
- **outline: none vs outline: 0 are semantically different.** outline: none sets style to none but leaves width at the UA default; getComputedStyle().outlineWidth still returns 3px. outline: 0 zeroes the width.
- **Interactive elements must not contain other interactive elements.** The "card as button" pattern (<div/article role="button"> with <button> actions inside) is a common and easy-to-miss WCAG 4.1.2 violation. Use the overlay-button pattern instead.
- **	abIndex={-1} is required on non-interactive skip-link targets.** Without it, <main> cannot receive focus from anchor navigation.
- **waitForSelector in tests should target the rendered view's root element, not a sibling view's class.** When Playwright reports locator resolved to hidden, the selector is finding the wrong element.
