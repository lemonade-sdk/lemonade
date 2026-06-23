# Decision: Chat Rail Listbox Focus Model + Account Menu Modal/Non-Modal

**Author:** Mattingly  
**Date:** 2026-06-22  
**Branch:** `feat/gui3-chat-account-a11y`  
**Issues:** #2346 (conversation rail) · #2348 (account menu)  
**PR:** #2363  
**Status:** Implemented, tests passing (58 passed / 7 skipped / 0 failed)

---

## 1. Conversation Rail — Listbox Focus Model

### Context

The conversation rail (`<ul role="listbox">`) already had the correct ARIA role and label but no focus management. Individual `<li role="option">` elements were not focusable, had no `aria-selected`, and the container had no keyboard event handlers. For a blind NVDA user, the entire conversation list was opaque — no way to navigate it by keyboard.

### Decision: Roving tabindex (not aria-activedescendant)

Two standard patterns exist for managing focus in a listbox:

| Pattern | Mechanism | AT support | Complexity |
|---------|-----------|-----------|------------|
| **Roving tabindex** | Selected/first item has `tabIndex=0`; others `-1`; `.focus()` on arrow nav | Excellent across all screen readers | Low |
| **aria-activedescendant** | Container is the single Tab stop; `aria-activedescendant` points at "active" option ID | Good but inconsistent in some SRs | Medium |

**Chose roving tabindex** because:
1. NVDA support is better-established for roving tabindex on listboxes.
2. Simpler React implementation — no container `tabIndex` state, no ID-to-element lookup.
3. Already the right pattern given that the `<ul>` isn't the focus target.

### Roving tabindex rules

- `selectedConversation.tabIndex = 0` (the active conversation)
- If no active conversation: `conversations[0].tabIndex = 0`
- All other options: `tabIndex = -1`
- ArrowDown/Up call `options[newIdx].focus()` — works on `tabIndex=-1` elements too

### Delete button: tabIndex=-1

The `<button class="rail__item-delete">` inside each option gets `tabIndex={-1}` to keep the listbox Tab-footprint clean (one stop per option). The button remains:
- Reachable by NVDA virtual cursor (browse mode navigates all DOM elements regardless of tabIndex).
- Identifiable by its `aria-label="Delete conversation: {title}"`.
- Visually shown when the parent `<li>` has keyboard focus (`.rail__item:focus-within .rail__item-delete { opacity: 1 }`).

**Tradeoff:** Sighted keyboard-only users (no screen reader) cannot Tab to the delete button. This is acceptable because (a) deleting a conversation is a non-critical, infrequent action; (b) WCAG 2.1.1 (Keyboard) is still satisfied via the screen reader virtual cursor path; (c) adding the button back to Tab order would require either an always-visible delete button or complex Tab-order manipulation within the listbox.

---

## 2. Account Menu — Modal Dialog Decision

### Context

`AccountMenu.tsx` already declared `role="dialog"` on the panel and `aria-haspopup="dialog"` on the trigger. However, it was missing `aria-modal`, a focus trap, Escape handler, and focus restore — the "half-dialog/half-popover" problem described in #2348.

The panel is visually rendered as a positioned popover (no backdrop, `z-index: 80`) but semantically declared as a dialog.

### Decision: MODAL dialog

Alternatives considered:

| Option | Pros | Cons |
|--------|------|------|
| **Full modal (chosen)** | Consistent with existing `role="dialog"` declaration; focus contained; screen readers don't escape to page content | Visually looks like a popover (no backdrop) — minor visual/semantic mismatch |
| Non-modal popover (role=menu) | Matches visual treatment | Requires restructuring multi-mode forms to `menuitem` semantics; `role=menu` is wrong for sign-in forms |
| Non-modal unlabeled popover | Simplest | Weakest semantics; no standard pattern for multi-mode forms |

**Chose MODAL** because:
1. The component already declared `role="dialog"` — changing it to `role="menu"` would be a regression.
2. The panel contains complex multi-mode forms (sign-in, create account, settings with destructive admin actions). Modal containment prevents screen-reader focus from accidentally landing on chat messages or other content while the panel is open.
3. The existing `useFocusTrap` hook handles all implementation complexity — zero new dependencies.
4. `aria-modal="true"` + `useFocusTrap` + Escape + focus restore is the complete WCAG 2.1 requirement checklist for modal dialogs.

### Implementation notes

- `useFocusTrap(panelRef, open)` focuses the **× close button** (first focusable element) on open. This is correct: the panel always has the × button regardless of current mode.
- `autoFocus` on form inputs in 'signin' / 'create' modes fires on re-render WHEN those modes activate — after the trap has already set focus on the × button. The `autoFocus` fires naturally without conflicting with the trap.
- `closePanel()` uses `requestAnimationFrame(() => triggerRef.current?.focus())` for deferred restore — needed because the panel DOM is still present during the synchronous `setOpen(false)` call.
- The Escape handler calls `e.stopPropagation()` to prevent the event from bubbling up to the bottom-sheet Escape handler (which would close the mobile sheet unexpectedly).
- Trigger's `onClick` takes a separate open/close path: clicking to close does NOT call `closePanel()` (focus is already on the trigger from the click). Only Escape and the × button go through `closePanel()`.
