# 2026-05-15T19:30:00Z — Mattingly: UI/UX redesign study

**Agent:** Mattingly (UI / Frontend)
**Mode:** sync (substantive design study expected as direct response)
**Requested by:** Kyle Poineal
**Trigger:** Kyle accepted "stay React" verdict and immediately pivoted: design study
aimed at "Apple-designer-jealous" polish, staying in React + Tauri.

## Why routed to Mattingly
Frontend / UI agent. Design study is exactly the role. The 2026-05-15 retraction
also constrained this work: no framework debate, React stays put. Mattingly is the
agent who just made that commitment — appropriate she owns the design study that
sits inside that constraint.

## Files authorized to read
- 3 screenshots provided by Kyle (Model Manager view, Backend Manager view, Marketplace view)
- `docs/dev/philosophy.md` (re-read for design tenet alignment)
- `src/app/src/renderer/components/panels/LLMChatPanel.tsx` (current chat empty state)
- `src/app/src/renderer/ModelManager.tsx` (capability badge data substrate)
- `src/app/src/renderer/components/managers/BackendManager.tsx` (SHA exposure)
- `src/app/src/renderer/components/managers/Marketplace.tsx`
- `src/web-app/webpack.config.js` (dep set constraint verification)

## External sites fetched (competitive review)
- `https://lmstudio.ai`
- `https://openwebui.com`
- `https://jan.ai`
- `https://msty.ai` (redirected from `msty.app`)
- `https://cherry-ai.com`
- `https://nomic.ai/gpt4all`
- Apple Intelligence / System Settings / Activity Monitor as reference (no fetch, recall)

## Files produced
- `.squad/decisions/inbox/mattingly-ux-redesign-study.md` — full study, ~5KB
- Appended `### 2026-05-15: UI/UX redesign study — competitive review + screenshot critique` to `.squad/agents/mattingly/history.md`

## Outcome
**Design study delivered, no implementation commitment.** Five sections:

1. **Critique of current UI** — IA is a VS Code skeuomorph that's hit its ceiling;
   chat empty state does no work; status bar is too dense; no design system.
   Top-3 ranked: chat empty state, IA flattening, no design tokens.
2. **Competitive review** — 6 competitors + Apple reference. Key steals: LM Studio
   model selector in title bar, Open WebUI community feed, Jan conversation rail +
   Memory, Msty Knowledge Stacks, Cherry Studio side-by-side model comparison
   (identified as Lemonade's unique differentiator).
3. **7 design principles** — chat is the product; names not SHAs; status is
   contextual; loading is a state; one opinion at first launch; model is a
   character; capability is user's noun.
4. **Concrete proposals per surface** — chat / Model Manager / Backend Manager /
   Marketplace / status bar / Settings. Each grounded with row templates, layout
   patterns, and specific affordances.
5. **5-step next-move plan** starting with design tokens file, then prototyping
   chat empty state behind a localStorage feature flag.

## Invariants explicitly honored
- #11: every piece of persistent state in the study is client-local (localStorage / app_settings.json)
- #12: no new npm modules proposed
- #13: no autostart, no lemond changes, no lemond embedding

## What this study explicitly does NOT do
- Propose any framework change (retraction stands)
- Propose any `lemond` change
- Commit to implementation order, timeline, or scope

## Downstream effects
- Next gate is a design call with Kyle. Mattingly recommends design tokens first as
  the lowest-cost unblocking move.
- The "side-by-side model comparison" finding identifies a unique product
  differentiator Lemonade can credibly claim. Worth surfacing to Lovell for
  strategic positioning.
