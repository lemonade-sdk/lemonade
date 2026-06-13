# Session Log — 2026-05-15T21:15:00Z — Recipes: design + build

**User:** Kyle Poineal
**Branch:** `feat/ui-testing`
**Scope:** Static prototype only (`prototype/ui-redesign/`). No backend, no
`src/app/`, no `src/web-app/`.

## Arc

1. **Kyle proposed a "personas" concept** for tuning backend behavior — a
   user-facing way to bundle engine config + sampling presets and apply them
   to models.
2. **Research (Explore agent):** Mapped the C++ `recipe` concept — backend
   engine id with per-engine option schema, separate from request-level
   sampling, 4-layer precedence (global → model-json → user-saved →
   per-request).
3. **Design synthesis (coordinator + Kyle):** Landed on a two-tier client-side
   model — portable `Recipe` (template) + `AppliedRecipe` (Recipe + model
   binding). Bundled starters yes. Client-side only (invariant #11).
4. **Terminology call (Kyle):** UI keeps the name "Recipes" despite collision
   with the code-level engine-id meaning. Code/contributor docs will re-label
   the engine-level concept as "engine type" or "backend" to avoid confusion.
5. **Build (Mattingly):** Delivered v1.2 — new top-nav tab, six bundled
   starters, two mock user recipes, applied-to-models zone, recipe slide-over
   splitting engine config (load-time) from sampling (request-time), chat
   composer pill with switch-recipe popover, model slide-over recipe selector,
   drag-drop JSON import overlay (visual only), compatibility tooltips on
   apply dropdowns. No new design tokens. `tokens.css` unchanged.

## Open judgment calls for Kyle

- Starter mutability (rename allowed on starters, or full read-only with
  prominent Clone flow?).
- "Save as Recipe" from model tuning — auto-seed source model's engine in
  Compatible Engines, or always prompt?
- Recipe JSON schema — document now in `docs/` (portable is a selling point)
  or defer until wired?

## Intentional deviation

Mattingly used a JS map `MODEL_ENGINES` keyed by `data-name` instead of
adding `data-engine` attrs to every model row. Acceptable for prototype;
production port should source engine from the model registry response.
