# 2026-05-15T19:00:00Z — Mattingly: ROI re-examination, Svelte POC retraction

**Agent:** Mattingly (UI / Frontend)
**Mode:** sync (direct response to Kyle's pushback)
**Requested by:** Kyle Poineal
**Trigger:** Kyle questioned the prior Svelte POC recommendation — "Does it even make sense to use Svelte vs React? Are we gaining anything?"

## Why routed to Mattingly
Mattingly authored the prior Svelte POC recommendation (2026-05-15T18:00:00Z). The
honest re-examination of her own recommendation had to come from her — both as the
author of record and because she had already loaded the renderer mental map needed
to quantify the actual delta.

## Files authorized to read
- `src/app/src/renderer/components/panels/LLMChatPanel.tsx` (verify streaming throttle claim)
- `src/app/src/renderer/ModelManager.tsx` (count framework-agnostic LOC)
- `src/app/src/renderer/components/managers/DownloadManager.tsx`
- `src/app/src/renderer/components/managers/BackendManager.tsx`
- `src/app/src/renderer/ChatWindow.tsx` (re-examine `phaseRef` pattern)
- `src/app/src/renderer/hooks/useModels.tsx` / `useSystem.tsx` / `useInferenceState.ts`
- `src/app/src/renderer/tauriShim.ts`
- `.squad/decisions.md` (read Kranz's verdict on `node-svelte` Debian availability)

## Files produced
- `.squad/decisions/inbox/mattingly-roi-reassessment.md` — retraction decision
- Appended `### 2026-05-15: ROI re-examination — retracting the Svelte POC recommendation` to `.squad/agents/mattingly/history.md`

## Outcome
**Retraction.** Recommendation: stay on React 19 + webpack, refactor in place.
Quantified ceiling: ~300 LOC saved across ~10-15k LOC of renderer (2-3% reduction)
for the cost of rewriting 47 `.tsx` files. ROI is upside-down. Also caught own
contradiction with Kranz on Debian availability of `node-svelte` /
`node-svelte-loader` — Kranz is the build authority, defer to Kranz. Activated
the original recommendation's stated fallback ("if blocker, stay React, refactor
giant components") proactively rather than after weeks of POC effort.

## Downstream effects
- Lovell now needs to issue a Lead verdict on the framework question (he produced
  it independently, in parallel, ~same timestamp). Both arrived at "stay React"
  via different reasoning paths.
- Mattingly's history now contains both the pre-retraction analysis AND the
  retraction itself — preserved for future agents to learn from.
