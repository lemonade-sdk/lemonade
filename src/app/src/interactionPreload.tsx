import React, { lazy } from 'react';

type PreloadableComponent<P extends object> = {
  Component: React.ComponentType<P>;
  preload: () => Promise<void>;
};

function createPreloadableComponent<P extends object, M>(
  loader: () => Promise<M>,
  select: (module: M) => React.ComponentType<P>,
): PreloadableComponent<P> {
  let resolved: React.ComponentType<P> | null = null;
  let modulePromise: Promise<{ default: React.ComponentType<P> }> | null = null;

  const load = () => {
    if (!modulePromise) {
      modulePromise = loader().then(module => {
        resolved = select(module);
        return { default: resolved };
      });
    }
    return modulePromise;
  };

  const LazyComponent = lazy(load);
  const Component: React.FC<P> = props => resolved
    ? React.createElement(resolved, props)
    : React.createElement(LazyComponent, props);

  return {
    Component,
    preload: () => load().then(() => undefined),
  };
}

type ModelDetailProps = React.ComponentProps<typeof import('./components/ModelDetailPanel')['ModelDetailPanel']>;
type RouterEditorProps = React.ComponentProps<typeof import('./components/RouterEditorPanel')['default']>;
type GlobalModelSettingsProps = React.ComponentProps<typeof import('./components/GlobalModelSettingsPanel')['default']>;
type InspectViewProps = React.ComponentProps<typeof import('./components/InspectView')['default']>;
type LogViewerProps = React.ComponentProps<typeof import('./components/LogViewer')['default']>;

const modelDetail = createPreloadableComponent<ModelDetailProps, typeof import('./components/ModelDetailPanel')>(
  () => import(/* webpackChunkName: "models-detail" */ './components/ModelDetailPanel'),
  module => module.ModelDetailPanel,
);
const routerEditor = createPreloadableComponent<RouterEditorProps, typeof import('./components/RouterEditorPanel')>(
  () => import(/* webpackChunkName: "models-router-editor" */ './components/RouterEditorPanel'),
  module => module.default,
);
const globalModelSettings = createPreloadableComponent<GlobalModelSettingsProps, typeof import('./components/GlobalModelSettingsPanel')>(
  () => import(/* webpackChunkName: "models-global-settings" */ './components/GlobalModelSettingsPanel'),
  module => module.default,
);
const inspectView = createPreloadableComponent<InspectViewProps, typeof import('./components/InspectView')>(
  () => import(/* webpackChunkName: "monitor-telemetry" */ './components/InspectView'),
  module => module.default,
);
const logViewer = createPreloadableComponent<LogViewerProps, typeof import('./components/LogViewer')>(
  () => import(/* webpackChunkName: "monitor-logs" */ './components/LogViewer'),
  module => module.default,
);

export const ModelDetailPanelPreloaded = modelDetail.Component;
export const RouterEditorPanelPreloaded = routerEditor.Component;
export const GlobalModelSettingsPanelPreloaded = globalModelSettings.Component;
export const InspectViewPreloaded = inspectView.Component;
export const LogViewerPreloaded = logViewer.Component;

export async function preloadModelManagerSecondarySurfaces(): Promise<void> {
  // Details is the first action inside Models; give it the request/evaluation
  // slot before lower-frequency editors.
  await modelDetail.preload();
  await Promise.all([routerEditor.preload(), globalModelSettings.preload()]);
}

export async function preloadMonitorSecondarySurfaces(): Promise<void> {
  // Telemetry is the common first action inside Monitor; Logs follows.
  await inspectView.preload();
  await logViewer.preload();
}

let primaryInteractionPreloadPromise: Promise<void> | null = null;
let deferredInteractionPreloadPromise: Promise<void> | null = null;

function reportPreloadFailures(label: string, results: PromiseSettledResult<void>[]): void {
  const failed = results.filter(result => result.status === 'rejected');
  if (failed.length) console.debug(`Failed to warm ${failed.length} ${label} surface(s)`);
}

/**
 * Warm the two surfaces that were still noticeable on a first interaction.
 * This is deliberately started only after the first usable frame by App.tsx,
 * so it can never hold the startup cover or compete with critical JS.
 */
export function preloadPrimaryInteractionSurfaces(): Promise<void> {
  if (!primaryInteractionPreloadPromise) {
    primaryInteractionPreloadPromise = Promise.allSettled([
      modelDetail.preload(),
      inspectView.preload(),
    ]).then(results => reportPreloadFailures('primary interaction', results));
  }
  return primaryInteractionPreloadPromise;
}

/** Warm lower-frequency secondary surfaces after the two primary ones. */
export function preloadDeferredInteractionSurfaces(): Promise<void> {
  if (!deferredInteractionPreloadPromise) {
    deferredInteractionPreloadPromise = Promise.allSettled([
      routerEditor.preload(),
      globalModelSettings.preload(),
      logViewer.preload(),
    ]).then(results => reportPreloadFailures('deferred interaction', results));
  }
  return deferredInteractionPreloadPromise;
}

/**
 * Start interaction warming in UX priority order. The returned promise covers
 * only Model Details + Monitor Telemetry; lower-frequency surfaces continue in
 * the background and never delay broad workspace warming.
 */
export function preloadInteractionSurfaces(): Promise<void> {
  const primary = preloadPrimaryInteractionSurfaces();
  void primary.finally(() => { void preloadDeferredInteractionSurfaces(); });
  return primary;
}
