# Session log — 2026-05-15T19:45:00Z

**Requested by:** Kyle Poineal
**Theme:** Framework retraction + UX redesign study
**Branch:** `feat/ui-testing`

## What happened

1. **Kyle pushed back on Svelte POC.** Asked the load-bearing question: "Are we
   gaining anything?" Triggered Mattingly's ROI re-examination.

2. **Mattingly retracted her own Svelte POC recommendation.** Re-read the renderer
   code, quantified the actual delta: ~300 LOC saved (~2-3% reduction) for the
   cost of rewriting 47 `.tsx` files. Also caught own contradiction with Kranz on
   `node-svelte` Debian availability. Recommended React component decomposition
   instead.

3. **Lovell delivered an independent Lead verdict** in parallel. Same conclusion
   (stay React) via different reasoning: strategic ROI + externalized
   maintenance cost on Debian packaging + bus-factor risk. Provided four explicit
   decision-rule conditions for reopening the Svelte question (lock-in protection).

4. **Kyle accepted "stay React"** and pivoted to UX redesign.

5. **Mattingly delivered a UX redesign study** — critique of 3 screenshots, 6-site
   competitive review (LM Studio / Open WebUI / Jan / Msty / Cherry Studio /
   GPT4All) + Apple reference, 7 design principles, concrete proposals per
   surface, 5-step next-move plan starting with design tokens.

## Key pivots

- **Framework decision settled.** React stays. Future agents must not re-litigate
  unless one of Lovell's four conditions is satisfied.
- **Unique differentiator surfaced.** Side-by-side model comparison in one chat —
  Lemonade has the substrate (multiple loaded models, local server), no competitor
  has it cleanly. Worth strategic emphasis.
- **Convergence strengthens both verdicts.** Mattingly and Lovell arrived at the
  same answer via independent reasoning paths. Neither rests on the other.

## Outstanding decisions

- **Design call** on Mattingly's UX study. Specifically:
  1. Design tokens file — recommended first move, lowest cost.
  2. Chat empty state prototype behind a localStorage feature flag.
  3. Backend Manager: demote-to-Settings vs device-first matrix (strategic call).
  4. Marketplace / Discover split (defer per Mattingly).
- **React decomposition POC** sanctioned by Lovell. Target: `ModelManager.tsx`
  (75 KB → sub-components under 10 KB each). Not yet started.

## Files affected

- `.squad/decisions.md` — 3 new entries appended (Mattingly retraction, Lovell
  verdict, UX study)
- `.squad/orchestration-log/` — 3 new entries
- `.squad/agents/mattingly/history.md` — summarized (was 16,199 bytes, over the
  15 KB gate); full content preserved in `.squad/agents/mattingly/history-archive.md`
- `.squad/agents/lovell/history.md` — appended verdict summary (within size limits)

## Notes for next session

- React stays put — do not re-open the Svelte question without Lovell's four
  conditions.
- Design call is the next gate, not implementation.
- Side-by-side model comparison is the headline differentiator — track it.
