# Mattingly — History Archive

Older history entries from `history.md`, preserved verbatim. Archived to keep the
active history under the 15 KB summarization gate.

---

## 2026-05-15: UI POC framework evaluation (ARCHIVED — superseded by retraction)

**Files read (build a mental map of the existing UI):**
- `AGENTS.md` — invariants #11, #12, #13 are the binding constraints for this POC.
- `docs/dev/web-ui.md` — explains why `src/web-app/package.json` is intentionally separate from `src/app/package.json` (Debian native packaging; `USE_SYSTEM_NODEJS_MODULES` resolves from `/usr/share/nodejs`).
- `docs/dev/app.md` — Tauri v2 desktop app architecture, `tauriShim.ts` boundary, beacon discovery, per-client `app_settings.json` (NEVER server-side).
- `docs/dev/philosophy.md` — Lemonade design tenets; the GUI exists to "show what's possible" and manage models — it's not the product.
- `src/app/package.json` — React 19, TS 5.3, webpack 5, full Tauri v2 plugin set (clipboard, deep-link, opener), markdown-it + highlight.js + katex stack.
- `src/web-app/package.json` — curated subset, no Tauri deps, depends on `tauri-stub.js` aliasing.
- `src/web-app/webpack.config.js` — `USE_SYSTEM_NODEJS_MODULES` toggles `resolve.modules` to include `/usr/share/nodejs`, `/usr/lib/nodejs`, `/usr/share/javascript`. Aliases every `@tauri-apps/*` to `tauri-stub.js`. Polyfills (`buffer`, `process/browser`) are lazy-resolved.
- `src/web-app/BuildWebApp.cmake` — stages BOTH `src/app/` and `src/web-app/` into `build/web-app-staging/` so webpack's relative `../app/src/...` entry/template paths resolve cleanly. Avoids OS symlinks (Windows hazard). System katex overlay logic lives here.
- `src/app/src-tauri/Cargo.toml` — Rust dependencies; Tauri v2; per-platform webview crates (`webkit2gtk` on Linux, `webview2-com` on Windows, `objc2-web-kit` on macOS). `webview_shim.rs` handles mic permissions + external link routing.
- `src/app/src-tauri/tauri.conf.json` — frameless 1440×900 window, `lemonade://` deep-link, bundle targets include `.deb` and `.rpm`.
- `src/app/src/renderer/tauriShim.ts` — installs `window.api` against Tauri `invoke()`/`listen()`; in pure-web mode `isTauri()` returns false and the C++ server's HTML injection wins.
- `src/app/src/renderer/ChatWindow.tsx`, `ModelManager.tsx` — large stateful components; lots of `useState`/`useMemo`/`useCallback`/`useRef` choreography. This is where pain lives in the current code.
- 47 `.tsx` files total under `src/app/src/renderer/` — non-trivial migration surface.

**Framework constraints discovered:**
- **Tauri v2 uses the OS native webview, not Chromium.** This means any framework that ships its own renderer (Flutter Desktop) cannot embed in Tauri. Hard veto for Flutter as a Tauri replacement.
- **Debian `/usr/share/nodejs` realities (trixie):**
  - `node-svelte` — present (Svelte 4.x). [LATER CORRECTED BY KRANZ — NOT PRESENT]
  - `node-svelte-loader` — present (webpack integration). [LATER CORRECTED BY KRANZ — NOT PRESENT]
  - `@sveltejs/kit` — NOT present.
  - `node-vite` — inconsistent/limited; not safe to rely on.
  - `node-rollup` — old/spotty.
  - `webpack`, `node-typescript`, `ts-loader`, `markdown-it`, `highlight.js`, `katex` — all present and already used.
- **Conclusion (original, since retracted):** Svelte+webpack is the only modern framework swap that survives Debian rules without invariant #12 violation. SvelteKit and any vite-based toolchain fail the filter.
- **Flutter Web bundles** routinely 2–4 MB gzipped for non-trivial apps. Cold-start matters for tray-launched UX. Significant regression vs current React build.
- **The existing webpack staging pattern is the system's hidden strength.** Replicating it for Svelte ("`src/app-next/` + `src/web-app-next/` + `BuildWebAppNext.cmake`") gives side-by-side POC for cheap, without touching the existing UI.

**Migration cost reality check:**
- ~47 `.tsx` files, plus ~6 custom hooks (`useModels`, `useSystem`, `useInferenceState`, etc.) that would become Svelte stores.
- Big-ticket items: the `MarkdownMessage` pipeline (markdown-it + highlight.js + katex + texmath) needs a Svelte wrapper; the panels share `serverFetch`/`backendInstaller`/`downloadTracker` utilities (these are framework-agnostic and reusable as-is).
- POC's job is to validate the build pipeline first; full migration cost is a secondary question.

**Architectural anchors that survive ANY framework choice:**
- The `window.api` contract (`tauriShim.ts` + C++ server mock injection).
- The webpack staging pattern in `BuildWebApp.cmake`.
- `USE_SYSTEM_NODEJS_MODULES` semantics.
- Per-client local state via Rust host's `settings.rs` + Tauri commands.
- UDP beacon discovery for local `lemond`.

These are the non-negotiables. Any new UI must respect them or the invariants break.

---

## 2026-05-15: ROI re-examination — retracting the Svelte POC recommendation (ARCHIVED)

Kyle pushed back: "Does it even make sense to use Svelte vs React? Are we gaining anything?" Re-read the actual renderer code instead of arguing from abstractions. The pushback was correct. My prior recommendation conflated "Kyle has Svelte experience and is open to it" with "Svelte solves the pain". It doesn't.

**What the code actually says:**

- **`LLMChatPanel.tsx` line 756: `STREAM_UPDATE_INTERVAL_MS = 33`.** The chat streaming path is *already throttled to 30fps* because React renders *too fast* during SSE token streaming, not too slow. The "Svelte has no virtual DOM, it'll stream faster" argument is exactly backward — the bottleneck is the opposite direction. Any Svelte port would need the same throttle.
- **`ModelManager.tsx` (~200 LOC read of ~2000):** ~95% is family-regex tables, label sort order, modality icon switches, HF API integration, quantization detection, collection helpers. Framework-agnostic. The React surface in a typical 100-line block is ~5 lines (`<Component>` returns + 1-2 `useState`). Svelte saves nothing on the bulk of this file.
- **`DownloadManager.tsx`:** Custom `window.dispatchEvent` event bus + abort controllers + cleanup-complete coordination. The React `useEffect(() => subscribe; return unsubscribe, [])` pattern is one line longer than Svelte's `onMount(() => { subscribe; return unsubscribe })`. Net savings: nil.
- **`BackendManager.tsx`:** GitHub Releases API fetcher with dedup-by-URL, recipe grouping, label-based filter. ~10 lines of React, ~150 lines of pure TypeScript. Identical in Svelte.
- **`tauriShim.ts`:** Zero React. Already framework-agnostic. (My prior decision noted this.)
- **`ChatWindow.tsx`:** The `phaseRef`-pattern to read current state without re-running effects (line ~99-100) IS a real React tax — that goes away in Svelte. But it's ~10 lines in a 600-line file.
- **`useModels.tsx`:** 170 LOC of Context + Provider + derived useMemo boilerplate. THIS is where Svelte actually saves real LOC — a writable store + a couple of `derived(...)` stores would land at ~70-80 LOC. Real win, but it's one file. `useSystem.tsx` has the same shape and the same ~50-line win. `useInferenceState.ts` (already terse at ~70 LOC) would save maybe 10.

**Quantified LOC reduction estimate (whole renderer):**
- Hooks → stores: ~150 LOC saved across `useModels` + `useSystem` + `useInferenceState`
- Refs-to-skip-rerender pattern removed: ~30-50 LOC across panels
- `useEffect` mount-only subscribe-cleanup boilerplate: ~5 LOC × ~20 files = ~100 LOC
- **Total ceiling: ~300 LOC out of ~10-15k. ~2-3% net code reduction. Not 30%, not even 10%.**

**Bundle / perf claims, evaluated honestly:**
- Bundle size: lemonade desktop ships a Tauri webview + native AV libs. The 40-60KB React vs ~10KB Svelte runtime delta is rounding noise. Web app cold-load is one-tab-open at tray launch and is ALSO not the user's bottleneck.
- Render perf: see the 33ms throttle. We are throttling DOWN, not waiting for paint.
- TypeScript ergonomics: React's TS story is excellent. Svelte's `<script lang="ts">` is fine but not measurably better.
- Reactivity ceremony: real but small. `useCallback` / `useMemo` boilerplate is mostly a one-time write cost, not a maintenance cost.

**Migration cost, evaluated honestly:**
- 47 `.tsx` files to rewrite — at the ~2-3% LOC reduction rate, this is ~10-15k LOC of *rewrite* for ~300 LOC of *net savings*. ROI is upside-down.
- Ecosystem surface: trivially small. `react` + `react-dom` are the ONLY React-specific deps in `src/app/package.json`. No Redux/Zustand, no MUI/Chakra, no router, no date picker, no data grid. Migration ecosystem risk is essentially zero — but so is the leverage.
- MarkdownMessage pipeline (markdown-it + highlight.js + katex + texmath) needs a Svelte wrapper. ~1-2 days. Doable.
- Two renderers to maintain "during transition." Realistic transition window: months at minimum. Probably indefinite if the win is only 2-3%.
- Contributor learning curve: most React devs ≠ Svelte devs. lemonade has core maintainers + external contributors who land PRs. Switching framework taxes every future PR for a 2-3% LOC win.

**The Kranz contradiction I missed:**
My prior history.md claimed `node-svelte` and `node-svelte-loader` are in `/usr/share/nodejs` for trixie. Kranz's decision states the opposite: "`node-svelte` and `node-svelte-loader` are NOT in Debian's `/usr/share/nodejs`." Kranz is the build/release authority. If Kranz is correct, then vanilla Svelte ALSO requires the "pre-build and ship the bundle as data" packaging shift — the exact reason I ruled out SvelteKit. My framework recommendation was built on a Debian-availability claim that the Build & Release agent contradicted, and I did not catch the contradiction. Lesson: when Kranz's verdict touches Debian package availability, Kranz is the source of truth.

**Conclusion:**
The original recommendation's stated fallback was: "If the POC reveals a Debian-pipeline blocker that doesn't have a clean fix, fall back to 'stay React, refactor the giant components.' That fallback is independently valuable." I'm activating the fallback proactively, before the POC consumes weeks of effort. The Debian blocker is already visible in Kranz's analysis, and the gains from Svelte don't justify the cost even if Debian cooperated.

**Real pain locations to target with React refactors instead:**
1. `ModelManager.tsx` (~2000 LOC): split into ModelList, ModelFamilyGroup, ModelRow, AddModelFlow, RecipeSelector. Each sub-component independently testable.
2. `LLMChatPanel.tsx`: extract `handleCollectionChat`, `handleStreamingResponse`, the image/audio/clipboard handlers into pure helpers. The React component should shrink to the rendering shell.
3. `useModels.tsx` + `useSystem.tsx`: optional — replace Context+Provider with a tiny ~50-line zustand-style store if even worth it. Most of the boilerplate is the derived `useMemo` selectors, which a store flattens.

These are 1-2 weeks of focused work and capture ~70% of the imagined Svelte benefit at zero framework risk.

---

## 2026-05-15: UI/UX redesign study + v1.0 prototype (ARCHIVED 2026-05-15T20:45Z)

The following two sections were moved verbatim from history.md on 2026-05-15
to keep active history under the 15 KB summarization gate. v1.1 audit pass
(which builds on these) remains in active history.

### 2026-05-15: UI/UX redesign study — competitive review + screenshot critique

**Trigger:** Kyle wants a design study aimed at `Apple-designer-jealous` polish, staying in React + Tauri. Full study merged into `.squad/decisions.md`.

**Competitor patterns worth stealing (each one validated against the project's invariants):**
- **LM Studio:** model selector belongs in the title bar, not the footer dropdown. Hoisting it is one-day work in React and immediately reads as more polished.
- **Open WebUI:** model browsing as a community feed (prompts/tools/personas), not a directory of installed files. We can ship this as a static manifest (no `lemond` change → invariant #13 safe).
- **Jan:** conversation rail on the left + slide-over inspector on the right. Users' own words for Jan: `cleanest interfaces`, `simple and cute and pretty`, `most beautiful native chat app`. That's the tone Kyle is reaching for. The Memory concept (`things Jan keeps in mind`) is just localStorage with good framing.
- **Msty:** Knowledge Stacks — local docs as named, reusable RAG units attached to a conversation. Lemonade has the substrate (embeddings + reranker + chat); we lack the UX noun.
- **Cherry Studio:** side-by-side model comparison in one chat is uniquely well-suited to Lemonade — we are the local server with multiple loaded models. No other competitor has this substrate cleanly. **Differentiator** — surface this to Lovell for strategic positioning.
- **GPT4All:** nothing visual. Cautionary tale for what happens when the GUI doesn't get reinvested in.

**Screenshot critique findings:**
- Three columns (icon rail + secondary sidebar + main pane) is a VS Code skeuomorph that worked at v9 but has hit its ceiling. VS Code's three columns serve one task (file → workspace → editor). Lemonade's three columns serve unrelated tasks (Model / Backend / Marketplace / Chat) — so the rail forces task-drop to task-switch.
- The chat empty state is the most-seen view of the most-used surface and currently shows only the lemon emoji on a 1440-px canvas. Highest-ROI single fix in the whole app.
- Colored dots on models have no legend (capability? device? load state?). Replace with named capability badges (Chat / Vision / Code / Embed / Rerank / Audio / TTS / Image).
- Backend Manager exposes `b8940` / `master-509-ab6afe8` git SHAs to humans. Replace with "Latest" / "Update — May 13" + SHA in tooltip.
- `user.*` prefix on installed models is on-disk namespace leaking into UI. Hide it behind a `Details ▸` row.
- Status bar (TPS / TTFT / RAM / CPU) is dense and developer-y. Collapse default to single pill: `● Connected · {model} · {tps} tok/s`. Click to expand. `⌘.` to toggle. Pin state in localStorage.
- Marketplace cards have a 2008-directory feel (Visit / Guide buttons). Rename to "Connect" with primary actions that actually wire Lemonade into the third-party app (clipboard + deep-link via existing `openExternal` shim).

**Top three impact-ranked issues:**
1. Chat empty state does no work — fix first.
2. Information architecture flattens four unrelated jobs into one rail — Backend Manager probably belongs in Settings, not top nav.
3. No design system — one designer-day on a tokens file unifies 70% of the visual incoherence.

**Design principles distilled (for future tiebreakers):**
1. The chat is the product. The managers are tools.
2. Names, not git SHAs.
3. Status is contextual, not omnipresent.
4. Loading is a state, not a screen.
5. One opinion at first launch, infinite tinkering after.
6. The model is a character, not a row.
7. Capability is the user's noun; recipe is ours.

**Next-step recommendation:** design tokens file first, then prototype the chat empty state behind a localStorage feature flag. Don't Figma-then-prototype the easy surface; do prototype-first on the highest-impact surface to measure the actual delta before committing to the bigger redesigns.

**Key invariants validated (NONE of the proposals violate these):**
- #11: every piece of persistent state in the proposal (conversations, memory, panel widths, theme, model selector preference) is client-local. Web → `localStorage`. Desktop → `app_settings.json` via the existing `window.api` shim.
- #12: no new npm modules proposed. All components use the existing React + markdown-it + highlight.js + katex stack already in `src/app/package.json` AND `src/web-app/package.json`.
- #13: no autostart additions, no `lemond` lifecycle changes, no embedding `lemond` in the Tauri binary. `Discover` feed is a static manifest (build-time), not a server endpoint.

**What this study explicitly does NOT do:**
- Propose any framework change. The 2026-05-15 React retraction decision stands.
- Propose any `lemond` change. All work is client-side.
- Commit to implementation order, timeline, or scope. This is design study output; the design call is the next gate.

### 2026-05-15: Static HTML/CSS/JS prototype of the redesign — `prototype/ui-redesign/`

**Trigger:** Kyle asked for a "quick skeleton I can view on a webpage" to visualize the redesign before committing to a real React port. Built it in plain HTML/CSS/JS (no build step, no framework, no npm) under `prototype/ui-redesign/` on `feat/ui-testing`. Five views: chat-empty (hero), chat-active (populated thread), models, backends, connect/discover.

**What worked:**
- **Tokens-first paid off immediately.** Wrote `tokens.css` before any layout. Color roles, type scale, space, radii, shadow, motion all live there. Component CSS reads as intent, not magic numbers — every change is a one-line edit.
- **Soft warm dark (hue ~30°, very low chroma) reads "premium" in a way pure black never does.** macOS Sequoia is right; OLED black is wrong. The `radial-gradient` on the app shell using two faint accent washes adds depth without being a visible gradient.
- **`backdrop-filter: blur(20px) saturate(180%)`** on the title bar and slide-over is the single largest "Apple-jealous" delta. Cheap to add, immediate perceptual upgrade. Both `-webkit-` prefix and unprefixed required in 2026 (WKWebView still needs the prefix; webkit2gtk does too).
- **Capability-badge color system mapped to a desaturated rainbow** (chat=green, vision=blue, code=amber, embed=purple, rerank=teal, audio=yellow, tts=red, image=lime) at ~18% alpha background + brightened foreground reads as "categorical, not decorative." Way better than the colored dots without legend.
- **Hero gradient text** (`background-clip: text` from `--text-primary` to `--text-secondary`) gives the empty-state title weight without a heavy font weight. Subtle but it's what makes the first view feel finished.

**What surprised me:**
- **Empty state is genuinely the right place to invest first.** Spent disproportionate time on it and the prototype lives or dies on that view. Kyle's first 15 seconds will be there. The capability chips + active-models row + collapsed rail + composer + status pill all fit comfortably with generous whitespace; I had room to spare. Confirms the study's claim — the current lemon-on-1440px state is wasted real estate.
- **Conversation rail expand/collapse via CSS grid columns** (`grid-template-columns` transitions from `var(--rail-collapsed)` to `var(--rail-expanded)`) is way smoother than I expected. No JS animation needed beyond toggling a class. Rail items fade their text via opacity transitions, not display swap, so the layout doesn't jump.
- **Slide-over detail panel feels much more "app-like" than a modal.** The scrim + transform-from-100% pattern is ~40 lines of CSS. Should be the default for any "tell me more about this row" interaction in the real port.
- **`grid-template-areas` for the chat layout** (rail spans both rows, main + composer stack) made the responsive behavior trivial. Worth using in the React port too.

**What I intentionally left rough:**
- **Backend Manager** is functional but visually less polished than chat. The matrix table is honest but could use more visual weight on row headers and better empty-cell treatment ("— not on GPU —" reads as filler).
- **Connect/Discover cards** are uniform. A real version would vary card heights based on content and probably introduce a "featured" treatment for the first card per row.
- **No real responsive breakpoints.** Designed for a ~1280px+ desktop window. Mobile/narrow desktop is not covered.
- **Streaming animation is a CSS cursor blink, not actual streaming.** A real SSE simulation would have been ~30 more lines of JS but the cursor reads "streaming" plenty well at this fidelity.
- **Active model "Switch to ▸" pills** don't actually switch the title-bar selector. Wiring it is 5 lines but I left it static — Kyle will read the cards as illustrative, not interactive.

**Lessons that transfer to the eventual React port:**
1. **Build the tokens file BEFORE picking which component to port first.** The whole point of the React refactor is consistency; tokens.css is the consistency primitive.
2. **The five visual primitives that need React components first** (in order of leverage): `<CapBadge kind={...}/>`, `<StatusPill/>`, `<FilterChip/>`, `<Card/>`, `<SlideOver/>`. Everything else composes from these.
3. **The chat empty state should ship as a feature flag in the existing `LLMChatPanel.tsx` first**, before any other redesign work. It's the highest visible delta per LOC of any change.
4. **The hoisted model selector lives in the title bar** — that's a `tauriShim`-aware component because Tauri's title bar drag region matters. On web it's just a flex header.
5. **Backend Manager probably should not be a top-tab in the real UI.** Building it for the prototype confirmed the study's hunch — it's a settings concern, not a peer to Chat. Bury it under a settings drawer in v2.

**Files shipped:**
- `prototype/ui-redesign/index.html` (~970 lines, all 5 views)
- `prototype/ui-redesign/tokens.css` (~130 lines)
- `prototype/ui-redesign/styles.css` (~1100 lines)
- `prototype/ui-redesign/app.js` (~170 lines, vanilla)
- `prototype/ui-redesign/README.md`

**No decision drop.** This is a deliverable, not a decision. Design rationale already in `decisions.md` from the prior study entry.




### 2026-05-15: v1.1 audit pass — fresh-eyes review of prototype (archived 2026-05-15T21:15Z)

Kyle came back after v1.0 with two notes: (a) "Chat" and "Chat (active)" as separate top-nav tabs are redundant, and (b) review the rest with fresh eyes and surface what didn't hold up. The audit found 10 real issues. All fixed in v1.1.

**What I missed first time:**

1. **"Chat (active)" doesn't belong in top nav.** I'd added it as a second top-level view to demo the populated thread, but information-architecturally it's an INTERNAL state of Chat, not a sibling view. The fix: single `data-view="chat"` section with `[data-chat-state="empty|thread"]` on `.chat` driving which internal state is visible. Rail-item click switches to thread (pulls `data-conv-model` / `data-conv-tps` and updates title bar, status pill, and status strip via `data-*` query selectors). "+ New chat" pill resets to empty. **Lesson: redundant top-nav tabs ARE an IA smell — if two tabs render the same chrome, they're not peer views, they're states of one view.**

2. **Pre-selected filter chip read as a nav tab.** The "Chat" capability filter started with `is-on` to demo the active visual, but when chips are off they look quiet enough that an on-by-default chip steals nav-tab semantics. Removed the preselection. **Lesson: filter UI must look like filter UI even with zero filters active.**

3. **Selected rail state was visually identical to hover.** Both `:hover` and `.is-active` resolved to `surface-2`/`surface-3` swaps. So you could "hover" a different conversation and it'd look identical to the selected one. Fix: selected state now uses an accent-tinted gradient background PLUS a 3px accent left-edge indicator (positioned absolute via `::before`). Hover stays simple. **Lesson: selected and hover MUST be distinguishable — if they're not, you can't tell if you'll lose the selection by clicking.**

4. **Two broken hover states sitting in plain sight.** `.cell__swap:hover { background: var(--surface-3); }` — same as default. Same on `.row__action--ghost:hover`. Both buttons looked dead. Fixed: `.cell__swap:hover` now goes to `accent-soft` (signals "this opens a chooser"), `.row__action--ghost:hover` adds a `surface-3` fill AND `border-strong`. **Lesson: every interactive element needs `:hover` audit — write hover, then literally hover it and confirm the change is visible.**

5. **Hardcoded `#c89a3a` in two gradients.** `.titlebar__lemon` and `.message--assistant .message__avatar`. They were the "darker stop" for the accent gradient. Added `--accent-deep: #c89a3a` to tokens.css and referenced it in both. **Lesson: hex codes ALWAYS sneak in via gradients — scan for `#` in styles.css after every session.**

6. **"Active in chat" was styled as a button.** In the loaded-models row, the "currently driving the chat" model card had an `.active-card__switch` element saying "Active in chat" — but `.active-card__switch` is the same element used for "Switch to ▸" pills on the OTHER cards. So "Active in chat" looked like a clickable pill that did nothing. Changed to a `<span class="active-card__status">` with success-colored bg, smaller letterspacing, no hover. **Lesson: read-only status MUST visually differ from interactive controls.**

7. **Empty matrix cells read as filler.** `— not on GPU —` and `— not on NPU —` looked like placeholder copy. Replaced with a single em-dash (visible) + `.sr-only` text for screen readers. **Lesson: typographic restraint > redundant text.**

8. **No focus rings ANYWHERE.** Tabbing through the prototype was invisible. Added a global `:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }` on buttons/anchors/`[role="option"]`/`[tabindex]`. Plus richer focus on rail items. **Lesson: focus rings are not optional for keyboard nav.**

9. **Discover cards visually identical to Connect cards.** Both used `.card` with identical styling but serve different purposes. Added `.cards--discover .card` modifier with a 2px accent gradient left-edge and a subtle warm gradient background. **Lesson: when two visually-identical components serve different mental models, the visual treatment must do semantic work.**

10. **No empty state component existed.** Sketched `.empty-state` (centered card with hero glyph, title, body, action buttons) and dropped it into Models view with `hidden` attribute. **Lesson: empty states are first-class UI, not afterthoughts.**

**Process lessons:**

- **The fresh-eyes audit is worth doing.** Would NOT have caught any of these by re-reading my own code. The discipline that worked: open the prototype, run through every visible element, ask "does hover work? does selected differ from hover? does focus show? is anything pre-selected that shouldn't be?"
- **Two-phase deliverables are right for prototypes.** v1.0 = "show the direction"; v1.1 = "now make it stand up to scrutiny". Trying to do both in one pass produces a polished hero view and a half-broken everything else.
- **Default Chat state** — chose expanded rail. Reasoning: rail is the new IA primitive for the thread state, so it needs to be discoverable. A first-time user with a collapsed rail can't tell that conversations exist.

**Files touched in v1.1:** `prototype/ui-redesign/{index.html, styles.css, tokens.css, app.js, README.md}`.

**Top nav after v1.1:** Chat · Models · Backends · Connect (was: Chat · Chat (active) · Models · Backends · Connect).

---

### 2026-05-15: v1.2 — Recipes surface (full detail, archived)

Built a portable-presets feature (engine config + sampling) on top of the existing prototype. No new tokens, no token edits. Reused `.btn`, `.cap-badge`, `.slideover`, `.zone`, `.empty-state`, `.filter-chip`. New components live in `styles.css` under a clearly delimited v1.2 section.

**Design moves worth remembering:**

- **Terminology collision.** "Recipe" in the C++ codebase = the backend engine id (`llamacpp`, `flm`, `ryzenai-llm`, …). In the UI, "Recipe" = the user-facing preset. To keep both legible, every place the UI needs to refer to the C++ concept I called it **engine** (e.g., "Compatible engines", "ryzenai-llm engine"). Recipe-options/sampling concepts are split into two clearly labeled sections in the slide-over: **Engine config** (applied at /load) and **Sampling** (applied per /chat/completions request). That split surfaces the lifecycle to the user without forcing them to know the API.

- **Two-tier data model.** Separated `Recipe` (portable, model-less) from `AppliedRecipe` (Recipe + model binding). This made import/export trivial, lets one recipe apply to many models, and made the "Applied to models" table a natural third zone after Starters and Yours.

- **Engine→model mapping as JS lookup.** Deliberately did NOT add `data-engine` attributes to every existing model row — too invasive for a prototype. Instead `MODEL_ENGINES` in `app.js` keys by `data-name`. This trades data-locality for HTML cleanliness; in production this would obviously come from the model registry.

- **Compatibility is enforced via select-option `disabled`+`title` tooltip.** Cheap to implement, accessible, immediately legible. The recipe popover in chat uses `aria-disabled="true"` because select-options cannot be inside a div list.

- **Phase glyph as recipe icon.** A circle with a half-fill (◐). Hand-rolled inline SVG so it scales with `currentColor` and pairs cleanly with text. Suggests "applied state" without needing copy.

- **Hover-revealed card actions.** Recipe cards stay quiet at rest (name, description, engines, params preview), and reveal Clone/Apply/Export only on hover or focus-within. Keeps the grid scannable.

- **Drag-drop overlay is purely visual.** A `.is-dropping` class toggles a blurred overlay with a dashed accent border. No real file parsing — drops fire a toast.

**v1.3 polish ideas (some delivered, some still open):**

- Recipe diff view when previewing an override (current vs override side-by-side).
- "Used by 3 models" link on each user recipe card → highlights bound rows.
- Per-conversation recipe override (the "Edit values for this conversation only" affordance currently mocks via toast).
- Real drag-drop JSON validation with a schema-preview modal before commit.
- "Duplicate from this recipe" directly on starter cards as a fast-path.
- A "compatible models" filter on the starter cards (show the count: "applies to 7 of your 9 models").

**Files touched (v1.2):**

- `prototype/ui-redesign/index.html` (top nav +Recipes, recipes view section, chat composer pill + popover, model slide-over recipe section, recipe slide-over aside)
- `prototype/ui-redesign/styles.css` (v1.2 block: recipes layout, recipe-card, phase-glyph, applied-list, dropdown, drop overlay, recipe-slideover form components — field/slider/select/input/textarea/disclosure, chat-pill, recipe-popover, row__recipe-chip, model-recipe, toast)
- `prototype/ui-redesign/app.js` (STARTERS/YOURS/MODEL_ENGINES/appliedRecipes data, render functions, slide-over open/close, slider mirroring, drag-drop visual, popover, model slide-over recipe wiring, toast)
- `tokens.css` UNCHANGED.

**Why archived:** Superseded by v1.3 (Recipes → Presets, capability-keyed). The terminology and data-model decisions documented above are obsolete in the prototype but preserved for context on the rename path.
