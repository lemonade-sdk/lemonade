/* Layout breakpoints shared by CSS media queries and JS layout checks.
 *
 * CSS custom properties cannot be used inside @media, so these values are
 * duplicated as literals in styles.css. Keep them in sync — a JS check that
 * disagrees with its media query leaves a band of widths where the two
 * layouts fight each other. DESIGN.md documents the roles.
 */

/** At or below this width the workspace shows one panel and rails go modal. */
export const MOBILE_BREAKPOINT = 768;

/** At or below this width action labels may collapse to icons. */
export const PHONE_BREAKPOINT = 480;

export const mobileMediaQuery = `(max-width: ${MOBILE_BREAKPOINT}px)`;

export function isMobileLayout(): boolean {
  if (typeof window === 'undefined') return false;
  return window.matchMedia(mobileMediaQuery).matches;
}
