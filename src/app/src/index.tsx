import React from 'react';
import ReactDOM from 'react-dom/client';
import App from './App';

type StartupMetrics = {
  documentStartMs?: number;
  entryMs: number;
  criticalCssReadyMs?: number;
  fullCssReadyMs?: number;
  cssSource?: 'inline-critical' | 'missing-critical' | 'linked-full' | 'fallback-full' | 'missing';
  uiCommitMs?: number;
  coverRemovedMs?: number;
  firstUsableFrameMs?: number;
  firstPaintMs?: number;
  firstContentfulPaintMs?: number;
  loadMs?: number;
  hostBootstrapStartMs?: number;
  hostBridgeReadyMs?: number;
  connectionSettingsReadyMs?: number;
  serverConnectedMs?: number;
  workspacePreloadReadyMs?: number;
};

declare global {
  interface Window {
    __LEMONADE_DOCUMENT_START_MS__?: number;
    __LEMONADE_STARTUP_METRICS__?: StartupMetrics;
  }
}

const startup: StartupMetrics = {
  documentStartMs: window.__LEMONADE_DOCUMENT_START_MS__,
  entryMs: performance.now(),
};
window.__LEMONADE_STARTUP_METRICS__ = startup;

function collectPaintEntries(): void {
  for (const entry of performance.getEntriesByType('paint')) {
    if (entry.name === 'first-paint') startup.firstPaintMs = entry.startTime;
    if (entry.name === 'first-contentful-paint') startup.firstContentfulPaintMs = entry.startTime;
  }
}

if (typeof PerformanceObserver !== 'undefined') {
  try {
    const observer = new PerformanceObserver(() => collectPaintEntries());
    observer.observe({ type: 'paint', buffered: true });
  } catch {
    // Older embedded webviews may not expose buffered paint observations.
  }
}

window.addEventListener('load', () => {
  startup.loadMs = performance.now();
  collectPaintEntries();
}, { once: true });

function cssVariable(name: string): string {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

// The source HTML contains a tiny white boot guard on purpose. Some Lemonade
// web hosts serve the template directly instead of webpack's generated HTML.
// That guard prevents a flash of raw, unstyled controls in both cases.
const criticalStyle = document.getElementById('lemonade-critical-css') as HTMLStyleElement | null;
const hasInlineCriticalCss = (criticalStyle?.textContent?.length || 0) > 256;
if (hasInlineCriticalCss) {
  startup.criticalCssReadyMs = performance.now();
  startup.cssSource = 'inline-critical';
} else {
  startup.cssSource = 'missing-critical';
}

let fullStyleFallbackPromise: Promise<boolean> | null = null;
function fullStylesApplied(): boolean {
  return cssVariable('--lemonade-full-styles-loaded') === '1';
}

function markLinkedStylesReady(): boolean {
  if (!fullStylesApplied()) return false;
  startup.fullCssReadyMs ??= performance.now();
  startup.cssSource = 'linked-full';
  return true;
}

function loadFullStyleFallback(): Promise<boolean> {
  if (markLinkedStylesReady()) return Promise.resolve(true);
  if (fullStyleFallbackPromise) return fullStyleFallbackPromise;

  fullStyleFallbackPromise = Promise.all([
    import(/* webpackChunkName: "app-styles-fallback" */ './styles/tokens.css'),
    import(/* webpackChunkName: "app-styles-fallback" */ './styles/styles.css'),
  ]).then(() => {
    // style-loader injects these rules while the chunk evaluates. The raw CSS
    // fallback has no emitted app.css sentinel, so reaching this point is the
    // readiness signal for the direct-template host path.
    startup.fullCssReadyMs = performance.now();
    startup.cssSource = 'fallback-full';
    return true;
  }).catch((error) => {
    startup.cssSource = 'missing';
    console.error('Failed to load Lemonade full application styles', error);
    return false;
  });

  return fullStyleFallbackPromise;
}

let fullStyleLinkFailed = false;
const fullStyleLink = document.getElementById('lemonade-app-css') as HTMLLinkElement | null;
if (fullStyleLink) {
  fullStyleLink.addEventListener('load', () => {
    requestAnimationFrame(() => { markLinkedStylesReady(); });
  });
  fullStyleLink.addEventListener('error', () => {
    fullStyleLinkFailed = true;
    // If the host served the raw source template there is no embedded critical
    // CSS and app.css may also be absent. Start recovery immediately while the
    // white cover is still hiding the React tree.
    if (!hasInlineCriticalCss) void loadFullStyleFallback();
  }, { once: true });
}

// On the raw-template host path, do not wait for React to expose the missing CSS
// problem. Fetch/inject full styles in parallel with the initial React render.
const stylesReadyForFirstReveal: Promise<boolean> = hasInlineCriticalCss
  ? Promise.resolve(true)
  : loadFullStyleFallback();

function revealApplication(): void {
  if (document.documentElement.classList.contains('lemonade-startup-revealed')) return;
  document.documentElement.classList.add('lemonade-startup-revealed');
  startup.coverRemovedMs = performance.now();
}

function watchFirstUsableFrame(rootElement: HTMLElement): void {
  let frame1 = 0;
  let frame2 = 0;
  let recoveryTimer = 0;
  let scheduled = false;
  let finished = false;

  const finish = () => {
    if (finished || scheduled) return;
    const explicitReady = rootElement.querySelector<HTMLElement>('[data-startup-ready]');
    const activeSlot = rootElement.querySelector<HTMLElement>('.view-slot:not([hidden])');
    const lazyViewReady = activeSlot && !activeSlot.querySelector('.view-loading');
    if (!explicitReady && !lazyViewReady) return;

    scheduled = true;
    startup.uiCommitMs = performance.now();

    void stylesReadyForFirstReveal.then((stylesReady) => {
      // Never expose raw browser-default controls. If both app.css and the JS
      // fallback fail, keep the neutral white cover and surface the error only
      // in the console instead of flashing a broken-looking application.
      if (!stylesReady) {
        console.error('Lemonade UI kept behind startup cover because styles are unavailable.');
        return;
      }

      frame1 = requestAnimationFrame(() => {
        revealApplication();
        frame2 = requestAnimationFrame(() => {
          finished = true;
          startup.firstUsableFrameMs = performance.now();
          collectPaintEntries();
          observer.disconnect();
          console.info('[startup] Lemonade first usable frame', { ...startup });
          window.dispatchEvent(new Event('lemonade:first-usable'));

          // Critical CSS is enough for the default screen, but the rest of the
          // application still needs full styles before the user visits another
          // workspace. Recover after reveal without blocking the first frame.
          if (hasInlineCriticalCss) {
            recoveryTimer = window.setTimeout(() => {
              if (!markLinkedStylesReady()) void loadFullStyleFallback();
            }, fullStyleLinkFailed ? 0 : 120);
          }
        });
      });
    });
  };

  const observer = new MutationObserver(finish);
  observer.observe(rootElement, { childList: true, subtree: true });
  finish();

  window.addEventListener('pagehide', () => {
    observer.disconnect();
    if (frame1) cancelAnimationFrame(frame1);
    if (frame2) cancelAnimationFrame(frame2);
    if (recoveryTimer) window.clearTimeout(recoveryTimer);
  }, { once: true });
}

const rootElement = document.getElementById('root') as HTMLElement;
const root = ReactDOM.createRoot(rootElement);
watchFirstUsableFrame(rootElement);

// React can render behind the white cover while CSS and JS finish their critical
// work. The cover is removed only after both the active view and usable styles
// are ready, eliminating FOUC without serializing React behind CSS loading.
root.render(<App />);
