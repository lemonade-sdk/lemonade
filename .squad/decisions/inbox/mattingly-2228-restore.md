# Decision: Restore PR #2228 after accidental revert

**Author:** Mattingly  
**Date:** 2026-06-15  
**Status:** Implemented — pushed as `2d8c45f0` on `kpoin/ui-testing`

---

## Context

PR #2228 (commit `115d464f`) was merged into `kpoin/ui-testing` on 2026-06-14T16:16Z. It added:
- Capability badge icons: Popular → flame, Tools → wrench, Reasoning → brain, MTP → rocket
- `reasoningElapsedMs` plumbing in `api.ts` + duration display in `ChatView.tsx`
- Preset + collection logic expansion in `lemonadeTools.ts`
- `labelDisplay` map expansion and `capabilityLabelsForModel` in `ModelManager.tsx`
- Badge alignment CSS in `styles.css`
- `CapabilityIcon` swap in `PresetManager.tsx`

Hours after merge, a "rebase ui-mobile-layout onto ui-testing" linearization (PR #2229, commit `c6529721`) extracted prototype/ files from a merge tree that did NOT include #2228's changes. This silently reverted all 7 affected files to a pre-#2228 state while retaining the a11y and mobile additions that landed after #2228.

Kyle discovered the regression on 2026-06-15: capability badges had no icons, reasoning timing was absent, and lemonadeTools was missing preset/collection tool support.

---

## What was done

Cherry-picked `115d464f` onto `kpoin/ui-testing` at HEAD `5c4ecdc2`:

```
git cherry-pick 115d464f
```

This produced whole-file conflicts in all 7 affected files. Resolution strategy for each:

| File | Strategy |
|------|----------|
| `Icon.tsx` | Took `115d464f` version wholesale — no a11y/mobile changes to this file |
| `lemonadeTools.ts` | Took `115d464f` version wholesale — no a11y/mobile changes to this file |
| `api.ts` | Took HEAD (has `window.location.hostname` mobile fix), applied `reasoningElapsedMs` plumbing from #2228 diff |
| `ChatView.tsx` | Took HEAD (has aria-live regions, bottom-sheet trap), added `formatDurationMs`/`reasoningSummary` and updated reasoning `<summary>` |
| `ModelManager.tsx` | Took HEAD (has a11y button conversions), added expanded `labelDisplay`, new helper functions (`iconForCapabilityLabel`, `capabilityLabelsForModel`, etc.), updated `renderLabels` and detail cap rendering |
| `PresetManager.tsx` | Took HEAD (has focus trap), removed `capabilityIcon` import, added `CapabilityIcon` + `useFocusTrap` imports, updated `CapabilityChip` render |
| `styles.css` | Took HEAD (has mobile media queries, focus rings, reduced-motion blocks, bottom-sheet styles), appended the 26-line badge alignment block from #2228 |

---

## Verification

**TypeScript:** `npx tsc --noEmit` — exits 0, no errors.

**#2228 content restored (grep hit counts):**
- `flame`/`wrench`/`brain`/`rocket` — 9 hits in Icon.tsx ✓
- `capabilityIconName`/`CapabilityIcon` — 14 hits across source ✓
- `reasoningElapsedMs` — 7 hits (api.ts + ChatView.tsx + LiveStreamStats) ✓
- `reasoningSummary` — 2 hits in ChatView.tsx ✓

**Mobile/a11y preserved (regression check):**
- `bottom-sheet` — 18 hits ✓
- `useFocusTrap` — 5 hits ✓
- `aria-live` — 2 hits ✓
- `window.location.hostname` — 2 hits in api.ts ✓
- `prefers-reduced-motion` — 1 hit in styles.css ✓
- `options-block__btn--selected` — 2 hits in styles.css ✓

---

## Commit

```
2d8c45f0  fix(ui): polish capability badges and reasoning timing
          (restored after accidental revert via c6529721)
```

Pushed to `origin/kpoin/ui-testing`.

---

## Downstream work needed (NOT done here — Kranz/Kyle decision)

The branches `kpoin/ui-mobile-layout` and `kpoin/ui-accessibility` diverged from a `kpoin/ui-testing` state that was missing #2228. Now that #2228 is restored on `kpoin/ui-testing`, those branches should be re-merged with `kpoin/ui-testing` so their next forward-merge doesn't accidentally re-introduce the reverted state.

**Recommended action:** Before any further work on those downstream branches, run:
```
git log --oneline --diff-filter=M -- prototype/ui-redesign/src/components/Icon.tsx | head -5
```
and verify `flame`/`wrench`/`brain`/`rocket` are present. If not, merge `kpoin/ui-testing` into the branch.

---

## Prevention

After any rebase, linearization, or large merge that touches `prototype/`, immediately verify content markers:
```powershell
Select-String -Path "prototype/ui-redesign/src/components/Icon.tsx" -Pattern "flame|wrench|brain|rocket"
Select-String -Path "prototype/ui-redesign/src/api.ts" -Pattern "reasoningElapsedMs"
Select-String -Path "prototype/ui-redesign/src/tools/lemonadeTools.ts" -Pattern "allStoredPresets"
```
Missing matches = regression. See `history.md` 2026-06-15 entry for the full sentinel list.
