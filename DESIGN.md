# Design System Specification

## 1. Overview & Creative North Star: "The Crystalline Zest"
This design system is a sensory exploration of refreshment. It moves beyond standard functional UI to evoke the visceral feeling of a chilled glass of lemonade—crisp, translucent, and vibrant. The "Creative North Star" for this system is **Crystalline Zest**.

Unlike rigid, corporate grids, this system leverages **heavy glassmorphism** and a **tonal gradient architecture** to create depth. It rejects the "flat web" in favor of an editorial layout that feels like looking through ice and liquid. We use intentional asymmetry and a strict 4-column content structure to provide a high-end, curated feel that prioritizes readability against a luminous, shifting background.

---

## 2. Colors & Tonal Architecture
The palette is a celebration of citrus. We strictly avoid dark "muddy" yellows or browns to maintain the "chilled" aesthetic.

### The Background Gradient
The primary canvas is not a flat color. It is a vertical gradient:
- **Top (Ice):** `--surface-container-lowest` (#FFFFFF)
- **Bottom (Settled Juice):** Transitioning toward #FFF9C4 (pale lemon).

### Key Tokens
- **Primary (Vibrant Citrus):** `--primary-yellow` (#FCD846). The signature accent for buttons, highlights, and energy.
- **On Primary:** `--on-primary` (#000000). Black text and icons on yellow surfaces for maximum contrast.
- **On Primary Muted:** `--on-primary-muted` (#3a3000). For text on light-yellow tinted surfaces (e.g. chat bubbles).
- **Accent Gold:** `--accent-gold` (#5C4B00). Used for icon tints and value-link text on white surfaces.
- **Natural Accent (Lemon Leaf):** `--tertiary` (#3C6531). Used sparingly for success states.
- **Neutral Surface:** `--surface` (#F6F6F6) provides the "chilled glass" base.

### Text Hierarchy
From darkest to lightest:
- `--text-on-light` (#000000) — Maximum contrast on light backgrounds.
- `--text-primary` (#2D2F2F) — Body text and headings.
- `--text-secondary` (#474747) — Descriptions and supporting text.
- `--text-nav` (#52525b) — Navigation links.
- `--text-muted` (#6b6b6b) — Labels and metadata.
- `--text-light` (#999) — Disabled or decorative text.

### Core Rules
- **The "No-Line" Rule:** 1px solid borders are strictly prohibited for sectioning. Use surface-container shifts to define boundaries.
- **Surface Nesting:** Hierarchy is achieved by layering. A `--surface-container-low` card should sit atop a `--surface` background.
- **Glassmorphism Rule:** Floating navigation and headers must use `--surface` with 60-80% opacity and `--glass-blur` (24px) backdrop-blur.

---

## 3. Typography: Editorial Authority
We pair the geometric precision of **Plus Jakarta Sans** for impact with the high-legibility of **Manrope** for narrative, and **Consolas/Monaco** for code.

- **Display & Headlines (`--font-display`):** Plus Jakarta Sans. Used for headings, buttons, and high-impact UI. Tight tracking, heavy weight (700-800).
- **Body & Labels (`--font-body`):** Manrope. Set in `--text-primary` (#2D2F2F) for maximum contrast.
- **Code (`--font-mono`):** Consolas, Monaco, Courier New. Used in code blocks, terminal cards, and API samples.
- **The Hierarchy Strategy:** Use extreme scale differences. Section headings use `clamp(1.35rem, 2.4vw, 2.1rem)` while body stays at ~0.88-1rem.

---

## 4. Elevation & Depth: Tonal Layering
We do not use structural lines. We use physics.

### The Layering Principle
Depth is achieved by "stacking" tones:
- *Level 0:* Background Gradient.
- *Level 1:* `--glass-bg` rgba(255,255,255,0.4) — The "Glass" layer (ice cards, panels).
- *Level 2:* `--glass-bg-hover` rgba(255,255,255,0.65) — Active/hover states.
- *Level 3:* `--surface-container-lowest` (#FFFFFF) — Solid "Ice" for maximum pop.

### Shadows
All shadows use the `on-surface` color (45, 47, 47), never pure black:
- `--shadow-light`: 0 4px 16px at 4% opacity. Resting cards.
- `--shadow-medium`: 0 8px 32px at 6% opacity. Hover states.
- `--shadow-heavy`: 0 14px 52px at 8.5% opacity. Hero panels and elevated elements.
- `--shadow-ambient`: 0 12px 48px at 5% opacity. Default ice-card shadow.
- `--shadow-yellow`: 0 12px 48px rgba(252,216,70,0.3). Primary buttons.
- `--shadow-yellow-hover`: 0 16px 56px rgba(252,216,70,0.38). Primary button hover.

### Ghost Borders
For essential accessibility on white-on-white elements, use `--outline-variant` (rgba(45,47,47,0.15)). It should be felt, not seen.

### Roundedness
- `--radius-card` (3rem): Ice cards, demo panels, value cards.
- `--radius-xl` (1.5rem): Console panels, API sample blocks.
- `--radius` (1rem): Buttons, interactive elements, inner containers.
- `--radius-btn` (9999px): Pill-shaped CTA buttons and download links.

---

## 5. Components

### The Ice Card (`.ice-card`)
The foundational glassmorphic component. All card-like containers inherit from this:
```css
background: var(--glass-bg);           /* rgba(255,255,255,0.4) */
backdrop-filter: blur(var(--glass-blur)); /* 24px */
border: 1px solid var(--glass-border); /* rgba(255,255,255,0.6) */
border-radius: var(--radius-card);     /* 3rem */
box-shadow: var(--shadow-ambient);
```
Used by: value cards, tech spec cards, demo panel, dev button, release card, console cards.

### Buttons
- **Primary (Yellow):** `--primary-yellow` background, `--on-primary` (#000) text, `--shadow-yellow` glow. Pill-shaped (`--radius-btn`).
- **Secondary (Glass):** Inherits `.ice-card` base. `--text-primary` text. Hover lifts to `--glass-bg-hover`.
- **Section CTA:** Same as primary, with `min-width: 320px` and `open_in_new` icon for external links.
- **Download Link:** Same as primary, used in the Getting Started section.

### Console Card (`.gs-console`)
Ice card with monospace content for terminal commands:
- Inherits `.ice-card` for the glassmorphic shell.
- `pre` uses `white-space: pre; overflow-x: auto` for horizontal scroll on long commands.
- Copy button positioned absolutely, top-right. Turns yellow on hover/copied.

### Platform Selector
Row of platform icon buttons:
- Default: `opacity: 0.75`, transparent background.
- Active: `opacity: 1`, `--glass-bg-hover` background, `--shadow-medium`, slight scale-up.

### Page Spacing Scale
The homepage uses a perfect-fourth (1.333) ratio spacing scale:
```
--page-space-base: 2rem
--page-space-sm:   base           ≈ 2rem
--page-space-md:   base × 1.333   ≈ 2.66rem
--page-space-lg:   base × 1.333²  ≈ 3.55rem
--page-space-xl:   base × 1.333³  ≈ 4.73rem
--page-space-xxl:  base × 1.333⁴  ≈ 6.31rem
```
Section top padding: `--page-space-xxl`. Heading margins: `--page-space-sm`. Internal gaps use fractional multiples of `--page-space-base`.

---

## 6. Do's and Don'ts

### Do:
- **Do** use large amounts of white space (vertical "breathing room") to simulate clarity.
- **Do** ensure text contrast ratios exceed 7:1, especially on glassmorphic panels over the yellow gradient.
- **Do** use the `--tertiary` green only as a "flavor" accent—like a mint leaf in a drink.
- **Do** use `--on-primary` (#000) for text on yellow buttons. It must be high contrast.
- **Do** use the `.ice-card` base class for all glassmorphic containers rather than duplicating properties.
- **Do** reference CSS variables for colors, shadows, and fonts. Avoid hardcoding values that have tokens.

### Don't:
- **Don't** use any dark browns or muddy yellows (#695B00) as button text. Use `--on-primary` (#000) instead.
- **Don't** use 100% opaque, hard-edged cards. Everything should feel like it has a degree of light transmission.
- **Don't** use traditional "Drop Shadows" with pure black. Use the ambient shadow tokens defined in Section 4.
- **Don't** use dividers to separate list items; use tonal shifts or vertical gaps.
- **Don't** use section label "chips" on every section. Reserve them for sections that need categorical context.
