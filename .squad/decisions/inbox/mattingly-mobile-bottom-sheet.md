# Decision: Mobile Bottom Sheet for Conversations

**Date:** 2026-06-13  
**Author:** Mattingly (UI agent)  
**Branch:** kpoin/ui-mobile-layout  
**Status:** Implemented (pending commit by Squad)

## Context

On mobile (≤480px), the conversation rail is hidden (`display: none` at 768px breakpoint). Users have no way to switch between conversations or start a new chat on their phone.

## Decision

Implement a slide-up bottom sheet triggered by a mobile-only "Conversations" button. The sheet reuses the same conversation list state and handlers already in `ChatView.tsx`.

## Approach: Separate bottom sheet element (Option 3)

### Options considered

| # | Approach | Pros | Cons |
|---|----------|------|------|
| 1 | CSS-only class toggle on existing `<aside class="rail">` | No extra markup | Conflicts with existing 768px `display: none`; requires complex media query overrides; fragile |
| 2 | React portal extraction | Clean component boundary | Over-engineered for POC; adds portal complexity; new component file |
| 3 | **Separate bottom sheet div with duplicated list JSX** | Clean CSS separation; zero risk to desktop rail; minimal React state (1 boolean) | ~40 lines of repeated conversation map JSX |

**Chose Option 3** because:
- Zero risk of regression to the desktop rail layout
- CSS for the sheet is fully isolated inside `@media (max-width: 480px)`
- The "duplicated" JSX is trivial (a `.map()` call) and references the same state/handlers — not actual logic duplication
- Lowest-touch approach for a POC

## Implementation details

- **Trigger:** `.chat__mobile-rail-trigger` — sticky bar at top of `.chat__main`, visible only at ≤480px
- **Sheet:** `position: fixed; bottom: 0` with `transform: translateY(100%/0)` transition (280ms ease-out)
- **Drag-to-close:** Pointer events on handle div; threshold 100px deltaY
- **ESC key:** Keyboard listener when sheet is open
- **Focus return:** On close, focus goes back to trigger button
- **No new dependencies:** Pure CSS + vanilla pointer events

## Tradeoffs

- The conversation list JSX appears twice in `ChatView.tsx`. If the rail item structure changes, both must be updated. Acceptable for a POC; in production, extract a `<ConversationListItem>` component.
- `window.innerWidth` check in the toggle handler is a JS-based breakpoint rather than pure CSS. This is necessary because the same button (`rail__toggle`) serves both desktop and mobile roles. A `useMediaQuery` hook would be cleaner but adds complexity for no functional benefit at POC stage.

## Files modified

- `prototype/ui-redesign/src/components/ChatView.tsx`
- `prototype/ui-redesign/src/styles/styles.css`
- `prototype/ui-redesign/scripts/screenshot-bottom-sheet.mjs` (new)
