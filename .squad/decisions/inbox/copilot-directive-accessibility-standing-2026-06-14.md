### 2026-06-14: Accessibility is a standing directive
**By:** Kyle (kpoin) (via Copilot)
**What:** Accessibility is now a standing project requirement, not optional. Every PR, feature, and UI change must consider a11y by default. Definition of done for any UI work includes:
- WCAG 2.1 AA compliance (semantic HTML, ARIA, keyboard nav, focus management, color contrast 4.5:1, screen reader labels)
- LLM-specific accessibility: aria-live for streaming output, response verbosity controls, high-contrast modes, reduced motion, dyslexia-friendly font option, keyboard shortcuts
- Reference: `prototype/ui-redesign/ACCESSIBILITY.md` for the canonical plan and current status
**Why:** Kyle: "every PR, feature, and UI change should consider a11y by default ... Not an afterthought."
**Scope:** All UI work in this repository (prototype, future Tauri app, any new surfaces).
**Owner:** All agents doing UI/frontend work (primary: Mattingly).
