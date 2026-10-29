# Accessibility

The Lemonade App uses the same React UI in the desktop and browser builds. Its accessibility regressions are in [the Playwright tests](tests/a11y.spec.ts).

## UI changes

- Use native controls and labels. Give icon-only buttons accessible names.
- Keep actions keyboard-operable and keyboard focus visible.
- Move focus into opened dialogs or sheets and restore it to the trigger when they close. Trap focus where the UI is modal.
- Expose state and errors without relying on color alone; use appropriate ARIA semantics and live regions.
- Respect `prefers-reduced-motion` for transitions.
- Update accessibility tests when changing interactive behavior.

## Testing

From `src/app/`, run:

```bash
npm run test:a11y
```

The tests include axe-core checks for serious/critical WCAG 2.1 A/AA violations and focused keyboard, focus, labeling, ARIA, and reduced-motion assertions. Additonally text contrast requirements (4.5:1 for normal text, 3:1 for large text). Both light and dark themes should be checked. Passing those ally check does not establish WCAG conformance. Check affected views and workflows manually with a keyboard and, when relevant, a screen reader.
