# Project Context

- **Project:** lemonade
- **User:** Kyle Poineal
- **Created:** 2026-05-15
- **Stack:** C++17 server, Tauri v2 desktop app, React 19 + TypeScript web app, Python integration tests, CMake build, multi-OS packaging
- **Domain:** Local LLM/AI inference server with multiple wrapped backends (llama.cpp, FastFlowLM, RyzenAI, vLLM, whisper.cpp, stable-diffusion.cpp, Kokoro)

## Core Context

Lead role: owns scope, decisions, code review. Reviewer authority with rejection lockout.
Working branch is `feat/ui-testing` — no merges to `main`.

## Learnings

### 2026-05-15 — UI POC architectural review

- **Invariant #12 is the binding constraint on framework choice.** The web-app build pipeline must resolve from Debian's `/usr/share/nodejs`, `/usr/lib/nodejs`, `/usr/share/javascript` when packaging the `.deb`. React+webpack survives this because every dep in `src/web-app/package.json` is curated to match what Debian ships. Svelte is plausibly survivable (Debian ships `node-svelte`) but the surrounding toolchain (rollup/vite plugins) needs verification. **Flutter Web is a hard no** — its output comes from `dart2js`, not npm. Adopting Flutter would either break the Debian package or require an entirely separate web-app build path, which is exactly what invariant #12 forbids.
- **Invariant #13 is mostly about discipline, not framework.** Any framework can avoid autostart. The real risk is a contributor copy-pasting "auto-launch on login" boilerplate from a Tauri/Electron tutorial. Reviewer policy must catch this.
- **Invariant #11 is per-client state.** All three frameworks can store local state. The trap is convenience APIs that sync to a server — none of them must be wired to `lemond`.
- **Single-instance:** `tauri-plugin-single-instance` keys on the bundle identifier, so a new Tauri-based POC binary with a different identifier won't collide with the existing `lemonade-app.exe`. Flutter has no built-in single-instance — that's an extra task if Flutter wins.
- **Side-by-side topology decision:** new sources at `src/app-next/` and `src/web-app-next/` (additive, no edits to existing trees). New web UI runs on its own dev port and talks to `lemond` over CORS, so no `lemond` route changes are required. New desktop is a separate binary, NOT autostarted.
- **Reviewer lockout reminder:** I'm Reviewer on this branch. Strict lockout applies — if I reject a revision, the original author may not be the one to fix it.
- **Kyle's framework experience signal:** Kyle has Svelte experience. That's not an architectural fact, but it tempers the framework risk story — Svelte is a known quantity to him, React is the status quo, Flutter is the unknown.

### 2026-05-15 — POC sanction verdict (independent of Mattingly)

- **Verdict: C — sanction a DIFFERENT POC.** Reject Svelte POC as scoped. Sanction a React decomposition POC targeting `ModelManager.tsx` (75 KB god-component) + state-hoisting on `ChatWindow.tsx` (13 KB, already partly decomposed).
- **Key sizing fact:** `ModelManager.tsx` is 75,054 bytes. `ChatWindow.tsx` is 13,858 bytes. The pain is concentrated in ONE file, and it's a component-design problem, not a framework problem. Svelte would produce a 75 KB Svelte god-component.
- **Strategic frame that decided it:** Philosophy doc says the GUI exists for two reasons only — to demo capability and to manage models. AGENTS.md says UI changes are core-maintainer-only. The project treats UI as a means, not an asset. Fungibility tenet applies to BACKENDS, not renderers. Investing in UI-framework velocity is investing in the wrong layer.
- **Externalized cost objection:** Kranz says Svelte is "tolerable" for Debian packaging. Tolerable ≠ free. Every entry in `contrib/debian/control` is something Mario Limonciello maintains forever. Doubling that surface is a permanent tax paid by someone NOT on this squad. Externalized cost is the hardest cost to take back.
- **POC success-criterion failure:** "One panel works in Svelte" cannot answer "will the project be better off in 18 months with two renderers." N=1 panel by the framework's strongest advocate is the classic POC trap. By contrast, the React refactor POC has a concrete falsifiable criterion: ModelManager → N sub-components, each <10 KB, state hoisted, next feature lands faster.
- **Reversibility is asymmetric:** the POC itself is cheap to abandon. The temptation to NOT abandon it (because the marginal cost of porting "just one more" panel feels low at every step) is what produces the long-term hybrid hell.
- **Mind-change conditions documented** in the verdict file so the question can be reopened with evidence, not vibes. Notably condition #4: if Kyle commits to personally owning the Svelte half forever, that's a valid maintainer-override clause and I'll surface it as such.
- **Reviewer posture going forward:** if Kyle accepts verdict C, my auto-reject list expands — any `src/app-next/` or `src/web-app-next/` tree is rejected on sight until the React decomposition POC delivers and we re-evaluate.
- **Independence note:** wrote this without reading Mattingly's verdict on the same question. Convergence or divergence between the two is a useful signal to Kyle in itself.


### 2026-05-15: Team update — prototype v1.2 added Recipes (user-facing presets)

Mattingly built a Recipes surface into the static prototype (prototype/ui-redesign/). User-facing "Recipe" = portable preset (engine config + sampling) bound to one or more models. This collides with the code-level meaning of ecipe (the backend engine id: `llamacpp`, `flm`, `ryzenai-llm`, etc.). When porting this prototype to real React, refer to the code-level concept as **"engine type"** or **"backend"** to avoid confusion. Full rationale in .squad/decisions.md under the 2026-05-15T20:03:13Z entry.

### 2026-05-16: Bug fix #1885 — Bug report template CLI command migration

Deprecated \lemonade-server\ command references throughout \.github/ISSUE_TEMPLATE/bug-report.yml\. The executable shim was replaced by discrete modern commands: \lemond\ (server), \lemonade\ (CLI client with \--version\, \scan\, \status\, \logs\, \ecipes\ subcommands). Updated all 6 bug report template sections referencing the old command set. **Key insight:** issue templates and docs are often the last to reflect CLI/API renames. Treat them as critical paths in deprecation reviews. PR #1914.

---

### 2026-05-16 — UI POC: Preset terminology landed (capability-keyed)

v1.3 prototype confirms: user-facing concept is **Preset** (capability-keyed). Codebase "recipe" is the engine id OR collection meta-recipe — when porting to real React/TypeScript, presets live entirely client-side. The model registry's `labels` array is the source of capability info — do NOT recreate hardcoded `MODEL_LABELS` in production code. See `decisions.md` (`2026-05-16T14:30:00Z: UI prototype v1.3 — Recipes → Presets, capability-keyed`).

Open question for production: where do capability labels come from? Must align with whatever `lemond` registers for OmniRouter tool registration — otherwise UI and runtime can disagree about what a model can do.

### 2026-05-31 — Preset/Recipe architecture review for v1.4 UX

**Wall Status:** ✓ Clean. UI=preset (capability-keyed, client-side only), C++=recipe (backend engine id) + RecipeOptions (per-recipe config JSON). No terminology leaks detected.

**Terminology findings:**
- Presets: capability-keyed config bundles with sampling params + recipe options, stored in localStorage only (`lemonade_user_presets` key). See `app.js:231–245` (hardcoded starters + YOURS array).
- Recipes: backend engine IDs (`llamacpp`, `flm`, `ryzenai-llm`, etc.) in `server_models.json` field `recipe` (line 4 et seq).
- RecipeOptions: per-recipe JSON config applied at load via `POST /api/v1/load` (defined in `recipe_options.h:1–32`).
- Labels: capability strings (e.g., `reasoning`, `coding`, `vision`, `tool-calling`) in `server_models.json` `labels` array (lines 36–38 show `["reasoning"]` examples). Real source of truth: live model registry via `GET /api/v1/models?show_all=true`.

**Contract risk:** `app.js:265–303` has `labelsFor()` with two data sources — live labels + recipe heuristics (e.g., "if recipe=='kokoro', add 'tts'"). If lemond adds a new label, heuristic will miss it → UI/runtime capability mismatch. **Production requirement:** read only from live `models` endpoint; drop fallback heuristics to force data bugs to surface in registry, not UI workarounds.

**Invariant status:**
- ✓ Invariant #11 (per-client local state): presets are 100% localStorage/IndexedDB. No `/presets` API routes exist; v1.4 UX work must NOT propose preset endpoints.
- ✓ Invariant #12 (Debian web-app deps): presets need zero new npm modules; prototype uses vanilla JS. If ported to web-app, any new deps must be in Debian `/usr/share/nodejs` (verify with Kranz before use).
- ✓ Invariant #1 (quad-prefix routes): no preset endpoints proposed; if any are added later, must register under `/api/v0/`, `/api/v1/`, `/v0/`, `/v1/`.

**Scope guardrails filed:** `.squad/decisions/inbox/lovell-presets-v1.4-guardrails.md`. Key out-of-POC items: preset API endpoints, server-side persistence, backend selection override, capability-based endpoint routing. All blocked without separate lemond changes.

### 2026-05-31 — Presets v1.4 shipped within guardrails

**Confirmation:** Mattingly shipped v1.4 per guardrails. UI changes:
- Schema: `applies_to: Capability[]` (capability-keyed, not recipe-keyed) ✓
- Storage: localStorage (`lemonade_user_presets`, `lemonade_applied_presets`) ✓
- Binding: staged (stores local binding, "Will apply on next load") ✓
- Sampling: wired into chat request bodies ✓
- No new `/presets` API routes proposed ✓
- Import policy: v1.4 requires `applies_to`; legacy rejected ✓

**No lemond changes required.** Wall intact. Build green, 2 tests passing. Ready for production trial.

### 2026-06-13 — PR review session: PRs #2223, #2224 from boclifton-MSFT

**PR #2223 (style refactor):** Merged. Clean split of monolithic `styles.css` into 22 partials. Well-documented import order. No regressions.

**PR #2224 (server settings / model folders):** Changes requested. Three blocking issues:
1. Merge conflict — adds to `styles.css` which #2223 just deleted. Needs rebase into `partials/settings.css`.
2. Web-app incompatibility — direct `import { open } from '@tauri-apps/plugin-dialog'` in shared renderer (`ServerSettings.tsx`) will break `src/web-app/` build. Needs runtime guard or lazy import.
3. CI failures (.deb, .rpm, macOS dmg) — likely cascading from #2.

**Key learning:** When the shared renderer (`src/app/src/`) imports Tauri-only plugins directly (not through `tauriShim.ts`), the web-app build breaks because it uses its own constrained `package.json`. Any Tauri-native API must be gated behind `window.__TAURI__` checks or routed through the shim layer. This is a recurring architectural constraint to enforce in reviews.

**Repo note:** GitHub disallows merge commits on this repo — squash merge is required even on feature branches.
