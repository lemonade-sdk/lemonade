# Decision: Mobile Layout Round 3 — Visual Verification + Cache Fix

**Date:** 2026-06-13  
**Author:** Mattingly (UI agent)  
**Status:** Implemented  
**Branch:** kpoin/ui-mobile-layout

## Context

Kyle reported that his phone still showed the old un-fixed layout after Rounds 1 and 2 of mobile CSS fixes. This raised the possibility that the CSS changes weren't taking effect.

## Investigation

Used Playwright with a 390×844 viewport (iPhone 14 equivalent) to screenshot the Chat and Models pages at `http://localhost:8080`. Visual inspection confirmed:

- Nav bar: icons-only, 7 buttons fit without scrolling ✓
- Model cards: stacked vertically, names readable ✓
- No horizontal overflow ✓

**Conclusion:** Round 1 and 2 fixes ARE live. The issue is browser caching on Kyle's phone.

## Root Cause

`webpack-dev-server` with `style-loader` serves CSS embedded in JS bundles. HMR updates via WebSocket, but phone browsers on LAN frequently disconnect (screen lock, tab switch, Wi-Fi handoff). Without `Cache-Control` headers, the phone browser serves its cached copy of the JS bundle — which still contains old CSS.

## Changes Made

1. **webpack.config.js** — Added `headers: { 'Cache-Control': 'no-store' }` to `devServer` config. Prevents phone browsers from caching stale bundles.
2. **styles.css** — Minor polish: better right-edge padding, tighter hero cards, smaller composer pills, smaller section labels — all at 480px breakpoint.
3. **scripts/screenshot-mobile.mjs** — New utility for future visual verification with Playwright.

## Recommendation for Kyle

Hard-refresh the page on his phone (pull down to refresh in iOS Safari, or long-press the reload button → "Request Desktop Site" → refresh again). After the `no-store` header is active (requires dev server restart), future CSS changes will always be fetched fresh.

## Lesson Learned

Always verify CSS changes with actual screenshots before declaring them done. A screenshot is proof; code review alone is not sufficient for visual correctness.
