# Project Context

- **Project:** lemonade
- **User:** Kyle Poineal
- **Created:** 2026-05-15
- **Role:** UI / Frontend — Tauri desktop app + web app

## Core Context

Currently leading a UI POC on `feat/ui-testing`: new UI side-by-side with existing
React/Tauri implementation. Must work BOTH web-served and as a desktop app.

**Framework decision (2026-05-15): React stays put.** Both Mattingly (LOC-quantified
re-examination) and Lovell (strategic ROI) independently converged on "stay React,
refactor in place." Four explicit conditions in Lovell's decision must hold before
the Svelte question can be re-opened. Do NOT re-litigate without them.

Critical constraint: Debian native packaging requires `src/web-app/` to use only
`/usr/share/nodejs` modules. **Kranz is the source of truth for Debian package
availability** — if a prior claim of mine contradicts Kranz, Kranz wins.

## Archived Learnings

Older detailed history entries archived to `history-archive.md` to keep this file
under the 15 KB summarization gate. Pointers below — read the archive for full
detail.

### 2026-05-15: UI POC framework evaluation (archived)

Built the renderer mental map. Read 47 `.tsx` files, the Tauri Rust host, the
webpack staging pattern in `BuildWebApp.cmake`, the `window.api` shim contract,
and the Debian `/usr/share/nodejs` realities. Identified architectural anchors
that survive any framework choice: `window.api`, webpack staging,
`USE_SYSTEM_NODEJS_MODULES`, per-client local state, UDP beacon discovery.

Originally recommended Svelte 4 + webpack POC. **That recommendation was
retracted same-day** (see next entry). Full file inventory and Debian dep notes
preserved in archive — useful as a renderer map for any future agent.

### 2026-05-15: ROI re-examination — retracted Svelte POC (archived)

Kyle pushed back: "Are we gaining anything?" Re-read the code. Quantified
ceiling: ~300 LOC saved (~2-3%) for the cost of rewriting 47 `.tsx` files.

Key findings preserved in archive:
- `LLMChatPanel.tsx` line 756: streaming is already throttled to 30fps because
  React renders too FAST — Svelte's "no virtual DOM" pitch is backward here.
- ~95% of `ModelManager.tsx` is framework-agnostic data shaping. Svelte saves
  nothing on the bulk.
- Only real wins: `useModels.tsx` (170 LOC → ~80 LOC as stores), `useSystem.tsx`
  (similar), refs-to-skip-rerender pattern (~10 LOC in `ChatWindow.tsx`).
- Migration cost: 47 files to rewrite for 2-3% net code reduction. ROI inverted.
- Caught own contradiction with Kranz on `node-svelte` Debian availability.

**Lesson recorded:** When Kranz's verdict touches Debian package availability,
Kranz is the source of truth. Do not re-claim Debian availability without
checking with Kranz first.

## Active Learnings

### 2026-05-24: Presets page — React port from POC

Built `PresetManager.tsx` (~420 LOC) porting the vanilla JS presets
implementation to React. Key implementation notes:

- **Data stays client-side** (invariant #11). `localStorage` for user presets
  and applied bindings. STARTERS are hardcoded constants, never fetched.
- **Slide-over uses local state for slider mirrors.** Each slider value is a
  `useState` that resets via `useEffect` when `preset` changes. This avoids
  the POC's `data-recipe-*` DOM wiring while preserving the mirror display.
- **CSS ported wholesale from POC `styles.css`.** Class names kept as-is
  (`.recipe-card`, `.cap-chip`, etc.) per the decisions.md entry. Added
  `.scrim`, `.slideover`, `.field`, `.slider`, `.select`, `.input` — these
  were missing from the React stylesheet but exist in the POC.
- **Capability chips are a reusable `<CapChip>` component.** Takes `isOn`,
  `disabled`, `onClick` props. Reusable for future chat composer pill work.
- **`PhaseGlyph` component** encapsulates the half-circle SVG at three sizes.
- **Responsive:** recipes head stacks vertically, grid goes single-column,
  applied-row collapses on mobile.
- **Playwright test 13** verifies nav, zones, card count, slide-over open/close.


### 2026-05-15: UI/UX redesign study — competitive review + critique (archived)

Competitive review of LM Studio, Open WebUI, Jan, Msty, Cherry Studio, GPT4All.
Top steals: hoisted model selector (LM Studio), community-feed model browser
(Open WebUI), conversation-rail + slide-over (Jan), Knowledge Stacks (Msty),
side-by-side model comparison (Cherry — **our unique differentiator**, surface
to Lovell). Screenshot critique identified three impact-ranked issues: chat
empty state does no work, IA flattens four jobs into one rail, no design
system. Distilled seven design principles. All proposals validated against
invariants #11/#12/#13. Full study in archive — read for competitor URLs,
verbatim user quotes for Jan, and per-view pain inventory.

### 2026-05-15: Static HTML/CSS/JS prototype v1.0 — prototype/ui-redesign/ (archived)

Built no-build prototype on eat/ui-testing. Five views, tokens-first
discipline, ~970 LOC HTML + ~130 tokens + ~1100 styles + ~170 JS. Key wins:
soft-warm-dark (hue ~30°) over pure black; ackdrop-filter: blur(20px)
saturate(180%) on title bar + slide-over is the largest single Apple-jealous
delta; capability-badge color system; hero gradient text. Five React primitives
identified for eventual port: <CapBadge/>, <StatusPill/>, <FilterChip/>,
<Card/>, <SlideOver/>. Confirmed by hand: build tokens BEFORE porting; ship
chat empty state behind a feature flag in LLMChatPanel.tsx first; Backend
Manager should NOT be a top-tab. v1.0 had unpolished surfaces — see v1.1 below
for the fresh-eyes audit pass. Full v1.0 detail in archive.
### 2026-05-15: v1.1 audit pass — fresh-eyes review (archived)

Kyle asked for a fresh-eyes audit after v1.0. Found 10 real issues, all fixed.
Highest-value lessons (full detail in `history-archive.md`):

- **Redundant top-nav tabs are an IA smell.** "Chat" + "Chat (active)" were
  states of one view, not peer views. Merged into a single `data-view="chat"`
  section with `[data-chat-state="empty|thread"]` driving internal state.
- **Selected ≠ hover.** Both resolving to `surface-2/3` made selection
  invisible. Selected now uses accent-tinted gradient + 3px accent left edge.
- **Hover audit is mandatory.** Two broken hovers (`.cell__swap`,
  `.row__action--ghost`) sat in plain sight because I never literally hovered
  them. Write hover, then hover it.
- **Token discipline includes gradient stops.** Two hardcoded `#c89a3a`
  hexes survived in `.titlebar__lemon` and assistant avatar gradients.
  Added `--accent-deep` to tokens. Scan for `#` after every session.
- **Read-only status MUST differ visually from interactive controls.**
  "Active in chat" was a `.active-card__switch` (same element as "Switch
  to ▸" pills). Same shape ≠ same semantics.
- **Focus rings are not optional.** Added global `:focus-visible` outline
  on buttons/anchors/`[role="option"]`/`[tabindex]`.
- **Visual treatment must do semantic work** when two components serve
  different mental models (Discover vs Connect cards both used `.card`).
- **Empty states are first-class UI.** Sketched `.empty-state` for Models.
- **Two-phase deliverables are right for prototypes.** v1.0 = direction,
  v1.1 = stands up to scrutiny.

Top nav after v1.1: Chat · Models · Backends · Connect.


### 2026-05-15: v1.2 — Recipes surface (archived, superseded by v1.3)

Built a portable-presets feature (engine config + sampling) on the existing prototype. No new tokens. Two-tier data model (`Recipe` portable + `AppliedRecipe` with model binding). Engine→model mapping via `MODEL_ENGINES` JS lookup (acknowledged as registry-sourced in prod). Compatibility enforced via select-option `disabled` + `title` tooltips. Phase glyph (◐) as recipe icon. Hover-revealed Clone/Apply/Export. Drag-drop overlay purely visual.

**The big v1.2 lesson — superseded but worth remembering:** "Recipe" collided with the C++ codebase. v1.2 dodged the collision by calling the codebase concept "engine" in the UI. v1.3 fully resolved it by renaming the UI concept to **Preset** and rekeying compatibility on capability labels instead of engines (see v1.3 entry below). Files touched, polish ideas, and granular v1.2 design rationale preserved in `history-archive.md`.

### 2026-05-23: API wiring — static prototype → live lemond

Wired `prototype/ui-redesign/` to real lemond HTTP endpoints. Created `api.js` as
a standalone connection module (`window.LemonadeAPI` singleton). Rewrote `app.js` to
use live data while preserving all existing UI interactions.

**Architecture:**
- `api.js` wraps all fetch calls, manages connection status (disconnected/connecting/
  connected), handles SSE streaming for chat and model pull.
- `app.js` consumes `api` singleton. Dynamic rendering replaces static HTML zones
  when API data loads. Falls back gracefully when lemond is offline.
- `index.html` gained server settings section in Connect view, data-attributes for
  dynamic containers.
- `styles.css` gained connection status dot variants, server settings form, progress
  bars, loading spinners, model selector dropdown.

**Key decisions:**
- `fetch()` + `ReadableStream` for SSE (not `EventSource`) — we need POST bodies.
- `MODEL_LABELS` map removed — `labelsFor()` now derives from API response `labels`
  field, with fallback inference from recipe/name for audio/tts/image/embed/rerank.
- Preset system stays 100% client-side (invariant #11).
- Chat conversations stored in `Map` in memory, not localStorage (session-scoped).
- 15s health-check polling for reconnection.
- Model selector dropdown populated from loaded LLMs, falls back to "No server".

**Files modified:** `api.js` (new), `app.js` (rewritten), `index.html` (data attrs +
server settings), `styles.css` (connection/loading states). `tokens.css` untouched.


---

### 2026-05-16 — v1.3 Presets (capability-keyed)

Shipped v1.3 on `feat/ui-testing` — renamed Recipes → Presets and rekeyed the compatibility model from engine-list to capability-list. Triggered by Explore's note that "recipe" is overloaded in the codebase (engine id AND OmniRouter meta-recipe collection), and that OmniRouter routes by capability label, not by which backend serves a model.

**What changed (v1.2 → v1.3):**

- **Terminology landed.** "Recipe" is gone from the UI everywhere it referred to the user-facing preset bundle. "Preset" is the user-facing word; "engine" / "backend" remains the word for the codebase concept (llamacpp / flm / ryzenai-llm / …). CSS class names kept the `.recipe-*` prefix (per Kyle's permission) to minimize diff — they're internal hooks now.
- **Capability-keyed compatibility.** `preset.engines: [llamacpp, flm, …]` became `preset.applies_to: [chat, image, …]`. `MODEL_ENGINES` (single-string lookup) became `MODEL_LABELS` (array of capability labels per model). `isCompatible` is now a label-intersection: `preset.applies_to.some(c => model.labels.includes(c))`. A chat preset fits any chat-capable model; a multi-cap model (e.g., Gemma-3 vision = `[chat, vision]`) is compatible with both chat and vision presets — exactly what an OmniRouter-style world wants.
- **Starter set is 8 (6 chat + 2 image).** Adds Sharp (steps 30 · cfg 8.0) and Quick (steps 15 · cfg 7.0) under `applies_to: ["image"]`. Demonstrates that capability-keying actually matters: a Sharp preset offered to a chat model is disabled with the tooltip "Needs image — Qwen3-26B exposes chat".
- **Slide-over options/sampling sections are now conditional.** Sections are tagged `data-preset-fields="chat"` and `data-preset-fields="image"`; `openRecipeSlideover` toggles their `hidden` based on the preset's primary capability. Image preset opens with Steps + CFG scale fields instead of Context size + Temperature.
- **"Engine config" heading renamed to "Options"** (chat) / "Options · per generation" (image). "Compatible engines" heading renamed to "Applies to capabilities". "Backend" select stayed in chat presets but is now framed as a hint ("Backend hint") not a hard binding — the runtime picks the real backend; the preset's backend hint biases.
- **Capability chip visual treatment** (`.cap-chip`): pill with a leading colored dot. Dot color per capability mirrors the existing `cap-badge` hue conventions in the Models view so a "chat" chip and a "chat" badge feel like the same concept. Surface stays neutral so multiple chips in a row don't compete visually. `.is-on`/`.is-off` modifiers used inside the slide-over's "Applies to capabilities" toggle row; cards just show the on-state chips.
- **MODEL_LABELS contains non-chat entries.** Added image (`stable-diffusion-3.5-medium` → `[image, edit]`), audio (`whisper-large-v3-turbo` → `[transcription]`), TTS (`Kokoro-82M` → `[tts]`), embedding (`nomic-embed-text-v1.5`), reranking (`bge-reranker-base`), and multi-cap chat+vision (Gemma-3, Mistral-Small-3.1). The default applied-bindings include `stable-diffusion-3.5-medium → Sharp` so the Applied list shows a non-chat binding out of the box.

**Decisions worth remembering:**

- **CSS class names left as `.recipe-*`.** The tradeoff: anyone reading the CSS sees "recipe" everywhere even though the UI says "preset". The win: zero risk of breaking selectors, much smaller diff. Acceptable because the class names are invisible to users.
- **`data-recipe-*` attributes left as-is** for the same reason. Only the view route key (`data-view-target="presets"` / `data-view="presets"`) was renamed — that's the single user-facing routing concept.
- **`id="recipe-slideover"` left alone.** Same JS-coupling rationale.
- **Backend hint stayed.** Considered dropping it ("Options" implies model-loading flags, not backend choice). Kept because a power user still wants to bias a chat preset toward Vulkan vs ROCm. If we ever fully realize the OmniRouter abstraction, this field probably moves to a per-model setting.

**v1.4 ideas:**

- **Save-as-Preset from model tuning.** When the user saves a preset from a model's tuning panel, seed `applies_to` with just that model's primary capability label. Multi-cap models would offer the choice.
- **Capability filter on the starter cards.** "Show only chat presets" / "Show only image presets". With 8 starters this isn't pressing, but at 15+ it will be.
- **"Compatible with N of M models" mini-badge** on each preset card. Reinforces what capability-keying buys you.
- **Real OmniRouter integration.** The mock `MODEL_LABELS` must come from the model registry / tool registration in production. Capability labels in the UI must match exactly what `lemond` registers — needs a contract definition.
- **Composable presets.** A preset that says `applies_to: ["chat", "vision"]` could "fan out" — the chat-side settings apply to chat-capable models, the vision-side settings to vision-capable models. Currently treated as a single bundle.

**Files touched:**

- `prototype/ui-redesign/index.html` — nav rename, chat pill copy, full Presets view section copy + count, model slide-over Preset section, Preset slide-over rewrite (title, "Applies to capabilities" section, chat/image `data-preset-fields` sections including new Steps + CFG scale sliders).
- `prototype/ui-redesign/app.js` — STARTERS (added Sharp, Quick), MODEL_LABELS replaces MODEL_ENGINES (13 entries spanning all capability types), `labelsFor` + `primaryCap` + `paramsPreview` helpers, `isCompatible` label-intersection logic, `recipeCardHTML` uses cap-chips, `openRecipeSlideover` toggles chat/image field sections + wires Steps/CFG sliders, KNOWN_CAPABILITIES list, applied-list/popover/model-section all use capability language.
- `prototype/ui-redesign/styles.css` — v1.2 comment block updated to v1.3, new `.cap-chip` + `.cap-chip-list` block with per-capability dot colors (chat = accent gold, vision = lilac, image = amber, transcription/audio = teal, tts = pink, embed = blue, rerank = orange, edit = warm tan, code = green).
- `prototype/ui-redesign/tokens.css` — UNCHANGED.

### 2026-05-24 — HuggingFace Model Search Integration

Added HF GGUF model discovery to the Models page search. When the user types 2+
characters, a debounced (400ms) call to the public HF API searches for GGUF models
and displays results in a new "Explore — HuggingFace" zone below the Available zone.

**Architecture decisions:**
- `searchHuggingFace()` is a standalone exported function in `api.ts`, not on the
  `LemonadeAPI` class — it's an external API with no auth, own base URL, no
  connection-state coupling.
- `AbortController` cancels in-flight HF requests when the search query changes —
  prevents stale results from racing ahead of fresh ones.
- HF results filtered against local registry model IDs to avoid duplicates.
- Silent failure: if HF API is unreachable, the zone just doesn't render. Local
  search is never disrupted.

**UX:**
- Zone uses HuggingFace orange (`#FF9D00`) for dot, border accent, tag pills,
  and action link — visually distinct from local model zones.
- Each row shows repo ID, downloads (formatted: 1.2M), likes, pipeline tag, and
  up to 5 relevant tags.
- Click to expand: file list (`siblings[]`), last modified date, direct HF link.
- Zone header: 🤗 dot + "Explore — HuggingFace" + count.
- Disappears when search is cleared.

**Files touched:**
- `prototype/ui-redesign/src/api.ts` — `HFModelResult` interface, `searchHuggingFace()`.
- `prototype/ui-redesign/src/components/ModelManager.tsx` — HF state, debounced
  effect, `filteredHfResults` memo, `renderHfRow()`, HF zone JSX.
- `prototype/ui-redesign/src/styles/styles.css` — `.zone__dot--hf`, `.row--hf`,
  `.hf-zone__loading`, `.hf-zone__spinner`, `.hf-zone__empty`, HF detail file list.
- `prototype/ui-redesign/tests/features.spec.ts` — test 07 extended to verify HF
  zone appears on search.

### 2026-05-31 — UI perf & bugfix pass (May 29–31)

Five fixes across the prototype, all on `feat/ui-testing`:

1. **HF "Invalid Date" fix** (`11d1f251`) — HF API omits `lastModified` without
   `expand[]`; switched to `createdAt` (always present) with null guard.
2. **Download cancel/abort** (`d5743b98`) — `AbortController` in `pullModel()`
   with abort signal threaded through the fetch read loop. Cancel buttons on both
   registry and HF download progress bars.
3. **LogViewer virtualization** (`1b1bbd4d`) — Fixed 22px line height, OVERSCAN=10,
   only renders visible window. Batched WebSocket entries at 100ms. Eliminated
   per-entry re-renders.
4. **Auto-scroll fix** — Replaced fragile `isProgrammatic` flag + timer with
   pointer/wheel event listeners to detect genuine user-initiated scrolls.
5. **Jump-to-bottom fix** (`2374212f`) — `rAF` double-tap in `scrollToBottom()`
   plus explicit `scrollTop` state update for correct virtual container re-render.

**Lessons:**
- **Virtualization changes the scroll contract.** A virtualized container doesn't
  know its own height until rendered; `scrollToBottom()` must wait two animation
  frames (one to lay out, one to measure) before the final `scrollTop` is valid.
- **Never detect "programmatic scroll" with a flag + timer.** The timing is
  inherently racy. Detect user-initiated scrolls positively (pointer/wheel events)
  and treat everything else as programmatic.
- **Batch WebSocket pushes.** At high log rates, per-message `setState` calls
  cause frame drops. A 100ms batching interval is imperceptible to the user but
  eliminates render thrashing.
- **HF API defaults are sparse.** Always null-guard optional fields; prefer
  fields documented as always-present (`createdAt`) over richer but opt-in ones
  (`lastModified`).
