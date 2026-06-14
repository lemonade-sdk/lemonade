# 2026-05-15T19:00:00Z — Lovell: Independent Lead verdict on UI POC

**Agent:** Lovell (Lead)
**Mode:** sync (decision gate)
**Requested by:** Kyle Poineal
**Trigger:** Kyle requested an independent Lead opinion on Mattingly's framework
recommendation. Explicitly NOT coordinated with Mattingly to preserve independence.

## Why routed to Lovell
Lead role owns scope, strategic ROI, and reviewer policy. The framework question is
a strategic resource-allocation decision (where does engineering capital go?), not
a frontend implementation question — exactly Lead's wheelhouse. Kyle wanted a
second pair of eyes that wasn't anchored to Mattingly's prior analysis.

## Files authorized to read
- `docs/dev/philosophy.md` (strategic framing for the GUI's role in the project)
- `AGENTS.md` (invariants, UI-as-core-maintainers-only governance)
- `contrib/debian/control` (dep-surface size estimate)
- `src/app/src/renderer/ChatWindow.tsx` (verify 13 KB claim, decomposition state)
- `src/app/src/renderer/ModelManager.tsx` (verify 75 KB god-component claim)
- `src/app/package.json` (verify React-only ecosystem surface)
- `.squad/decisions.md` (read Kranz's packaging verdict, Mattingly's recommendation,
  Liebergot's contract document)

## Files produced
- `.squad/decisions/inbox/lovell-poc-sanction.md` — verdict: sanction a DIFFERENT
  POC (React decomposition on `ModelManager.tsx`), reject Svelte POC as scoped
- Appended verdict summary to `.squad/agents/lovell/history.md`

## Outcome
**Verdict C: Sanction a different POC.** Reject Svelte POC. Sanction in its place
a React decomposition POC focused on the 75 KB `ModelManager.tsx` god-component
and a state-hoisting pass on `ChatWindow.tsx`. Strategic reasoning:

1. Project philosophy explicitly treats UI as a means, not a product.
2. Externalized maintenance cost — `debian/control` is maintained by Mario
   Limonciello, not this squad. Doubling the dep surface is a tax we don't pay.
3. POC's success criterion ("one panel works in Svelte") cannot answer the
   strategic question being asked.
4. The actual pain is observable and falsifiable: `ModelManager.tsx` is 75 KB.
   A React decomposition POC has a concrete measurable outcome.
5. Reversibility cuts both ways — once `src/app-next/` exists, marginal cost of
   "just one more panel" feels low until the tree has two renderers forever.

Provided four explicit decision-rule conditions under which Svelte would be
re-opened (lock-in protection: this isn't a permanent ban).

## Convergence with Mattingly
Independent reasoning paths, same conclusion: stay React. Mattingly arrived via
quantified LOC analysis; Lovell arrived via strategic ROI + externalized-cost
analysis. The convergence strengthens both verdicts — neither rests on the
other's reasoning.

## Downstream effects
- Kyle accepted the "stay React" outcome.
- Reviewer policy on `feat/ui-testing` (Lovell's prior decision) remains in force,
  now strengthened: any framework swap is now also auto-reject without a new
  decision satisfying one of the four re-open conditions.
- Cleared the path for Kyle's UX redesign request (next entry).
