# Decisions

## 2026-05-31: User directive — model defaults
**By:** Kyle Poineal (via Copilot)
**What:** Default all spawned agents to `claude-sonnet-4.6`. Scribe stays on `claude-haiku-4.5`. The coordinator (Squad) is not a spawnable agent and continues to use whatever the session runs on.
**Why:** User request — captured for team memory. Written to `.squad/config.json` (Layer 0 persistent config).

---

## 2026-05-24: HuggingFace Search as Standalone Function
**Author:** Mattingly
**Status:** Implemented

**Context:** Kyle requested HuggingFace model discovery in the Models page. The HF API is an external service, not part of lemond.

**Decision:** `searchHuggingFace()` is a standalone exported function in `api.ts`, **not** a method on the `LemonadeAPI` class. Rationale:
- `LemonadeAPI` is specifically for lemond communication (auth headers, base URL, connection status)
- HF search is unauthenticated, has its own base URL, and doesn't affect connection state
- Keeping it separate means HF failures never pollute lemond connection status

**UX Choices:**
- HF zone only appears when search query is 2+ characters — avoids cluttering the default view
- 400ms debounce balances responsiveness vs. API rate limiting
- Results are filtered against local registry to avoid showing duplicates
- Silent failure: if HF API is unreachable (firewall, no internet), the zone just doesn't render

**Impact:** No backend changes required — browser-direct fetch to HF; no new dependencies added; Playwright test 07 updated to verify HF zone presence.

---

## 2026-05-31: Preset/Recipe Architecture Guardrails for v1.4
**By:** Lovell (Lead)
**Status:** Architectural constraints for UI work; does NOT require lemond changes

**Executive Summary:** v1.3 established a clean wall: UI = presets (capability-keyed, 100% client-side), codebase = recipes (backend engine id + config). The wall is holding. Three risks identified:

1. **Contract drift:** UI MODEL_LABELS is hardcoded mock; real source is `lemond` model registry (`server_models.json`, line 36–63 shows `labels` array). Capability-keying works only if UI reads live labels.
2. **Persistence temptation:** Nothing is stopping Mattingly from proposing preset save endpoints; they **MUST stay client-side** (localStorage/IDB only).
3. **Invariant #11 enforcement:** Presets are per-client state. No server persists them, no API syncs them.

**Terminology Truth: Wall Status ✓**

| Layer | Term | Definition | Location |
|-------|------|-----------|----------|
| **UI (JavaScript)** | Preset | Capability-keyed config (chat / image / etc) + sampling params + display name | `app.js:231–245` (STARTERS, YOURS arrays) |
| **C++ codebase** | Recipe | Backend engine ID (`llamacpp`, `flm`, `ryzenai-llm`, `whispercpp`, `sd-cpp`, `kokoro`, `vllm`) | `server_models.json` field `recipe` (line 4, 10, etc.) |
| **C++ codebase** | RecipeOptions | Per-recipe JSON config applied to backend at model load via `POST /api/v1/load` | `recipe_options.h:1–32` |
| **Model metadata** | Labels | Capability strings (`reasoning`, `coding`, `vision`, `tool-calling`, etc.) | `server_models.json` array field `labels` (e.g., line 36–38: `["reasoning"]`) |

**Contract Risk: Capability Labels Source of Truth:**

`app.js:265–303` has logic that reconciles capabilities from TWO sources — live model registry labels + recipe-based heuristics. If `lemond` adds a new label and the heuristic misses it, UI and runtime disagree.

**Real Source of Truth:**
- **Live model data:** `GET /api/v1/models?show_all=true` returns model objects with `labels` array (from `server_models.json` + GGUF capability inference in `model_manager.cpp:434–438`).
- **Capability registration:** OmniRouter tool system in `lemond` determines what's actually available at runtime (independent of model labels).

**Production Requirement (for v1.4 UX, NOT this task):**

**MUST:** When presets UI goes to production, ONLY read labels from live `models` endpoint response. Do NOT hardcode fallback heuristics. If a label goes missing, that's a data bug to fix in `server_models.json`, not a UI workaround.

**Invariant Checks:**

✓ **Invariant #11** — Presets stay client-side. No `/presets` API, no server persistence.
✓ **Invariant #12** — `src/web-app/package.json` must resolve from Debian's native npm modules only.
✓ **Invariant #1** — Every new endpoint is registered under `/api/v0/`, `/api/v1/`, `/v0/`, `/v1/`.

**Scope Guardrails — v1.4 UX Work Boundaries:**

✅ **SAFE for v1.4 UI Work (NO lemond changes):**
- Render preset cards with capability chips (chat, image, etc.)
- Filter models by capability (via preset `applies_to` field)
- Display preset sampling params
- Store preset edits in localStorage
- Apply selected preset to model load by passing `recipe_options` to `/api/v1/load`
- Display which preset is applied to each loaded model
- Add capability chip toggles for filtering (client-side UI state only)

❌ **BLOCKED — Out of v1.4 Scope:**
1. **Preset API endpoints** — `/api/v1/presets`, etc. Violates invariant #11.
2. **Server-side preset persistence** — Requires DB schema, multi-client sync, API versioning. OUT OF SCOPE.
3. **Backend selection via preset** — Presets carry `backend` as display only; actual backend chosen by Router.
4. **Capability-based routing to endpoints** — Presets are config bundles, not routers.
5. **Automatic preset migration** — Future registry-merge decision.

**Future Decisions (Not v1.4):**

| Feature | Decision Owner | Impact | Note |
|---------|---|--------|------|
| "Export my presets" | Architecture review | Medium | Preset JSON serialization; offline backup |
| "Sync presets across my devices" | Architecture review | High | Breaks invariant #11; needs v2.0 rethink. |
| "Share presets with team" | Architecture review | High | Out of scope. |
| "Ship starter presets in the model registry" | Lovell + Kyle | Medium | Would modify `server_models.json` structure. Not v1.4. |

---

## 2026-05-31: Presets v1.4 proposal
**Author:** Mattingly
**Scope:** `prototype/ui-redesign/` only; no `lemond` changes.

**Audit Summary:**

The React implementation in `prototype/ui-redesign/src/components/PresetManager.tsx` differs from v1.3:

**Data Model Divergence:**
- React: `Preset` has top-level `recipe` (backend-recipe keyed)
- v1.3: User-facing preset had `applies_to: ['chat' | 'image' | ...]` (capability keyed)

**UI/UX Problems Identified:**
1. Primary selector is too technical (recipe target + recipe_options keys)
2. "Preset" and "recipe" terminology collide again
3. No compatibility guard for incompatible model/preset pairs
4. Only loaded models first-class; presets useful while deciding what to load
5. Sampling editable but not applied
6. Starter vs custom distinction awkward
7. "Your presets" pre-populated weakens meaning of "your"
8. Apply button buried in top-level tab instead of inline model detail

**Conceptual Model:**

Users expect a preset to answer: **What should this model feel like when I use it?**
- "Fast answers"
- "High quality writing"
- "Code review"
- "Long document context"
- "Sharp image"
- "Quick image draft"

**Recommended Internal Shape (POC):**

```ts
interface Preset {
  id: string;
  name: string;
  description: string;
  applies_to: Capability[];
  options: Record<string, unknown>;       // load/generation options
  sampling?: SamplingParams;
  engine_hint?: PresetRecipe | 'auto';     // optional, advanced
  starter: boolean;
}
```

**v1.4 Proposals:**

1. **Restore capability-keyed presets:** Replace top-level `recipe` with `applies_to`; rename card chip from "Recipe target" to "Applies to"; keep `engine_hint` under "Advanced engine options".
2. **Make Apply model-aware:** Use all known models; show capabilities; disable incompatible presets with explanations.
3. **Move first-use discovery inline:** Model row/card + model detail slide-over get preset section; chat composer gets preset pill.
4. **Simplify slide-over:** Header → use-with-picker → behavior section → advanced engine options → footer.
5. **Reframe starters:** Keep visible as examples; hide "Your presets" until user creates/clones/imports one.
6. **Make bindings explicit:** Say "Applies on this device/browser only."
7. **Sampling as request defaults:** Wire into chat composer or label "Saved request defaults — not yet applied."
8. **Do not touch `lemond`:** No v1.4 proposal requires C++ changes.

**Open Questions (7):**

1. Should "Your presets" start empty or seeded?
2. Should starter presets be visible always or hidden after user creates first custom?
3. Should users see backend/engine choices at all, or behind "Advanced"?
4. Should applying preset reload model immediately or stage choice?
5. For multi-capability models, bindings per model or per model+capability?
6. Should sampling be wired now or explicitly deferred?
7. Should imported JSON use v1.3 `applies_to` as canonical?

---

## 2026-05-31: Presets v1.4 shipped
**Author:** Mattingly
**Status:** Complete (build green, 2 Playwright tests passing)

**What Shipped:**

- **Schema change:** Presets now use `applies_to: Capability[]`; top-level `recipe` moved to optional `engine_hint`; `recipe_options` remains for advanced backend load options.
- **Staged-binding semantics:** Applying a preset stores one local binding per model and shows "Will apply on next load"; does not call `api.loadModel()` immediately. Next explicit load merges staged `recipe_options`.
- **Sampling consumer:** `prototype/ui-redesign/src/api.ts` merges active preset sampling (`temperature`, `top_p`, `top_k`, `repeat_penalty`) into `/api/v1/chat/completions` request bodies.
- **Import policy:** v1.4 import requires `applies_to`; legacy files rejected with "This file uses the legacy schema. Use the v1.4 export instead."
- **Files changed:** `prototype/ui-redesign/src/presetStore.ts`, `src/components/PresetManager.tsx`, `src/api.ts`, `src/styles/styles.css`, `tests/features.spec.ts`.

**Validation:** `npm run build` passes with existing webpack bundle-size warnings.
