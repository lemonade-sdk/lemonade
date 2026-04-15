# Design System Specification

## 1. Overview & Creative North Star: "The Crystalline Zest"
This design system is a sensory exploration of refreshment. It moves beyond standard functional UI to evoke the visceral feeling of a chilled glass of lemonade—crisp, translucent, and vibrant. The "Creative North Star" for this system is **Crystalline Zest**.

Unlike rigid, corporate grids, this system leverages **heavy glassmorphism** and a **tonal gradient architecture** to create depth. It rejects the "flat web" in favor of an editorial layout that feels like looking through ice and liquid. We use intentional asymmetry and a strict 4-column content structure to provide a high-end, curated feel that prioritizes readability against a luminous, shifting background.

---

## 2. Colors & Tonal Architecture
The palette is a celebration of citrus. We strictly avoid dark "muddy" yellows or browns to maintain the "chilled" aesthetic.

### The Background Gradient
The primary canvas is not a flat color. It is a vertical gradient:
- **Top (Ice):** `surface_container_lowest` (#FFFFFF)
- **Bottom (Settled Juice):** `primary_fixed` (#FCD846) transitioning toward a vibrant citrus base.

### Key Tokens
- **Primary (Vibrant Citrus):** `primary_fixed` (#FCD846) and `on_secondary` (#FAF972). Use these for high-energy accents.
- **Natural Accent (Lemon Leaf):** `tertiary` (#3C6531). This is our "organic" anchor, used sparingly for success states or specialized editorial calls-to-out.
- **Neutral Surface:** `surface` (#F6F6F6) provides the "chilled glass" base.

### Core Rules
- **The "No-Line" Rule:** 1px solid borders are strictly prohibited for sectioning. Use `surface-container` shifts to define boundaries.
- **Surface Nesting:** Hierarchy is achieved by layering. A `surface_container_low` card should sit atop a `surface` background. For the highest importance, use `surface_container_lowest` (Pure Ice White) to "pop" against the yellow-gradient bottom.
- **Glassmorphism Rule:** Floating navigation and headers must use `surface` with 60-80% opacity and a `20px` backdrop-blur. This allows the citrus background to bleed through softly.

---

## 3. Typography: Editorial Authority
We pair the geometric precision of **Plus Jakarta Sans** for impact with the high-legibility of **Manrope** for narrative.

- **Display & Headlines (Plus Jakarta Sans):** These are the "hero" elements. Use `display-lg` (3.5rem) with tight tracking to create a bold, premium editorial look.
- **Body & Labels (Manrope):** Set in `on_surface` (#2D2F2F) for maximum contrast. The weight should be optimized for clarity against translucent backgrounds.
- **The Hierarchy Strategy:** Use extreme scale differences. A `display-md` headline paired with a `label-md` creates a "high-fashion" layout tension that feels custom-built rather than templated.

---

## 4. Elevation & Depth: Tonal Layering
We do not use structural lines. We use physics.

- **The Layering Principle:** Depth is achieved by "stacking" tones.
    * *Level 0:* Background Gradient.
    * *Level 1:* `surface_container` (The "Glass" layer).
    * *Level 2:* `surface_container_lowest` (The "Ice" layer/Active Cards).
- **Ambient Shadows:** Shadows must mimic light passing through liquid. Use the `on_surface` color at 5% opacity with a `48px` blur and `12px` Y-offset. Never use pure black shadows.
- **Ghost Borders:** For essential accessibility on white-on-white elements, use `outline_variant` at 15% opacity. It should be felt, not seen.
- **Roundedness:** Use the `lg` (2rem) and `xl` (3rem) tokens for containers to mimic the soft, refracted edges of ice cubes and glassware. Use `DEFAULT` (1rem) for buttons and interactive elements.

---

## 5. Components

### Buttons
- **Primary:** `primary_container` (#FCD846) background with `on_primary_container` (#5C4B00) text. Shape: `DEFAULT` (1rem) radius.
- **Secondary (Glass):** Semi-transparent white with `20px` backdrop blur and a `ghost border`.
- **Tertiary:** `tertiary` (#3C6531) text only, used for "Organic" actions.

### Cards & 4-Column Blocks
- **The Layout:** Content must align to a 4-column structure.
- **Separation:** Forbid divider lines. Use `md` (1.5rem) spacing or a shift from `surface_container_low` to `surface_container_high`.
- **Styling:** Cards should use `DEFAULT` (1rem) roundedness and a soft ambient shadow to appear as if floating in the "juice."

### Input Fields
- **Surface:** `surface_container_lowest` with a 10% `outline` token.
- **States:** On focus, the border disappears and is replaced by a `2px` glow using the `primary` color.

### Additional Signature Component: The "Ice Chip"
- Small, highly-rounded (`full`) informational badges using `surface_bright` with high transparency and backdrop-blur. Used for categories or metadata to keep the interface feeling light and "effervescent."

---

## 6. Do's and Don'ts

### Do:
- **Do** use large amounts of white space (vertical "breathing room") to simulate clarity.
- **Do** ensure text contrast ratios exceed 7:1, especially when overlaying Glassmorphic panels on the yellow gradient.
- **Do** use the `tertiary` green only as a "flavor" accent—like a mint leaf in a drink.

### Don't:
- **Don't** use any dark browns or muddy yellows (#695B00). If a color feels "burnt," it is prohibited.
- **Don't** use 100% opaque, hard-edged cards. Everything should feel like it has a degree of light transmission.
- **Don't** use traditional "Drop Shadows." Use the **Ambient Shadow** rule defined in Section 4.
- **Don't** use dividers to separate list items; use tonal shifts or 24px vertical gaps.
