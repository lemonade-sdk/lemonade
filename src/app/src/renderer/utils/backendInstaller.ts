import { downloadTracker } from './downloadTracker';
import type { DownloadProgressEvent } from './downloadTracker';
import { serverFetch } from './serverConfig';
import { fetchSystemInfoData, Recipes } from './systemData';
import { ModelsData } from './modelData';
import { toFrontendOptionName, OPTION_DEFINITIONS } from '../recipes/recipeOptionsConfig';
import { getCollectionComponents, isCollectionModel } from './collectionModels';

function extractServerErrorMessage(errorText: string, fallback: string): string {
  if (!errorText) return fallback;

  try {
    const parsed = JSON.parse(errorText);
    if (typeof parsed?.error === 'string' && parsed.error.trim()) {
      return parsed.error;
    }
  } catch {
    // Not JSON; return raw text below.
  }

  return errorText;
}

/**
 * Registration data for custom user models sent with /pull requests.
 */
export interface ModelRegistrationData {
  checkpoint?: string;
  checkpoints?: Record<string, string>;
  components?: string[];
  recipe: string;
  source?: 'huggingface' | 'modelscope';
  mmproj?: string;
  labels?: string[];
  reasoning?: boolean;
  vision?: boolean;
  embedding?: boolean;
  reranking?: boolean;
}

/**
 * Thrown when a download (model or backend) is aborted due to user pause or cancel
 * via the Download Manager UI. Callers can inspect `.reason` to distinguish.
 */
export class DownloadAbortError extends Error {
  reason: 'paused' | 'cancelled';
  constructor(reason: 'paused' | 'cancelled') {
    super(`Download ${reason}`);
    this.name = 'DownloadAbortError';
    this.reason = reason;
  }
}

/** @deprecated Use DownloadAbortError instead */
export const PullModelAbortError = DownloadAbortError;

const SERVER_DOWNLOAD_POLL_INTERVAL_MS = 500;
const SERVER_DOWNLOAD_SNAPSHOT_ERROR_TIMEOUT_MS = 300_000;

export async function controlDownload(downloadId: string, action: 'pause' | 'cancel' | 'remove'): Promise<DownloadProgressEvent | undefined> {
  const response = await serverFetch('/downloads/control', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ id: downloadId, action }),
  });

  if (!response.ok) {
    const errorText = await response.text();
    throw new Error(extractServerErrorMessage(errorText, response.statusText));
  }

  if (action === 'remove') return undefined;
  return await response.json().catch(() => undefined);
}

function serverSnapshotMatchesDownloadId(item: DownloadProgressEvent, downloadId: string): boolean {
  return item.id === downloadId ||
    (!item.id && item.model_name != null && downloadId === `model:${item.model_name}`) ||
    (!item.id && item.model_name != null && downloadId === `backend:${item.model_name}`);
}

export async function waitForDownloadStatus(
  downloadId: string,
  statuses: Array<'paused' | 'cancelled' | 'completed' | 'error'>,
  timeoutMs = 15000,
  allowMissing = true,
): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const response = await serverFetch('/downloads');
      if (response.ok) {
        const downloads = await response.json();
        const download = Array.isArray(downloads)
          ? downloads.find((item: DownloadProgressEvent) => serverSnapshotMatchesDownloadId(item, downloadId))
          : undefined;
        if (!download) {
          if (allowMissing) return;
        } else if (statuses.includes(download.status) && download.running !== true) {
          return;
        }
      }
    } catch {
      // Keep waiting. This is only used as a cleanup barrier.
    }
    await new Promise(resolve => setTimeout(resolve, 250));
  }

  throw new Error(`Timed out waiting for download ${downloadId} to reach ${statuses.join(', ')}`);
}



async function isModelDownloadedOnServer(modelName: string): Promise<boolean> {
  try {
    const response = await serverFetch('/models?show_all=true', { cache: 'no-store' });
    if (!response.ok) return false;

    const data = await response.json();
    const modelList = Array.isArray(data) ? data : data.data || [];
    const model = modelList.find((m: any) => m.id === modelName || m.name === modelName);
    return model?.downloaded === true;
  } catch {
    return false;
  }
}

async function isBackendInstalledOnServer(recipe: string, backend: string): Promise<boolean> {
  try {
    const systemData = await fetchSystemInfoData();
    const backendState = systemData.info?.recipes?.[recipe]?.backends?.[backend]?.state;
    return backendState === 'installed' || backendState === 'update_available';
  } catch {
    return false;
  }
}

async function waitForServerDownloadTerminal(
  downloadId: string,
  modelName: string,
  abortController: AbortController,
  getAbortReason: () => 'paused' | 'cancelled' | undefined,
  initialJobSeen = false,
): Promise<void> {
  let sawServerJob = initialJobSeen;
  let consecutiveMissingSnapshots = 0;
  let snapshotErrorsStartedAt: number | undefined;

  while (true) {
    if (abortController.signal.aborted) {
      throw new DownloadAbortError(getAbortReason() ?? 'paused');
    }

    let snapshot: DownloadProgressEvent | undefined;
    try {
      snapshot = (await downloadTracker.hydrateFromServer({ throwOnError: true }))
        .find(item => item.id === downloadId || item.model_name === modelName);
    } catch (error) {
      const now = Date.now();
      snapshotErrorsStartedAt ??= now;
      if (now - snapshotErrorsStartedAt >= SERVER_DOWNLOAD_SNAPSHOT_ERROR_TIMEOUT_MS) {
        throw new Error(`Timed out refreshing server download state: ${error instanceof Error ? error.message : 'Unknown error'}`);
      }

      console.warn('Failed to refresh server download snapshot; keeping current state:', error);
      await new Promise(resolve => setTimeout(resolve, SERVER_DOWNLOAD_POLL_INTERVAL_MS));
      continue;
    }

    snapshotErrorsStartedAt = undefined;

    if (!snapshot) {
      if (sawServerJob) {
        consecutiveMissingSnapshots += 1;
        if (consecutiveMissingSnapshots >= 10) {
          // Terminal jobs are intentionally short-lived server-side. If a tab was
          // asleep or briefly disconnected, a successful job may have expired
          // before this waiter saw its completed snapshot. Verify the model state
          // before treating a missing row as cancellation.
          if (await isModelDownloadedOnServer(modelName)) {
            downloadTracker.completeDownload(downloadId);
            window.dispatchEvent(new CustomEvent('modelsUpdated'));
            return;
          }
          downloadTracker.removeDownload(downloadId);
          throw new DownloadAbortError(getAbortReason() ?? 'cancelled');
        }
      }
    } else {
      sawServerJob = true;
      consecutiveMissingSnapshots = 0;
      const stopped = snapshot.running !== true;
      if (snapshot.status === 'completed' && stopped) return;
      if (snapshot.status === 'paused' && stopped) throw new DownloadAbortError('paused');
      if (snapshot.status === 'cancelled' && stopped) throw new DownloadAbortError('cancelled');
      if (snapshot.status === 'error' && stopped) throw new Error(snapshot.error || 'Unknown download error');
    }

    await new Promise(resolve => setTimeout(resolve, SERVER_DOWNLOAD_POLL_INTERVAL_MS));
  }
}


async function waitForBackendDownloadTerminal(
  downloadId: string,
  displayName: string,
  recipe: string,
  backend: string,
  abortController: AbortController,
): Promise<void> {
  let sawServerJob = false;
  let consecutiveMissingSnapshots = 0;
  let snapshotErrorsStartedAt: number | undefined;

  while (true) {
    if (abortController.signal.aborted) {
      throw new DownloadAbortError('paused');
    }

    let snapshot: DownloadProgressEvent | undefined;
    try {
      snapshot = (await downloadTracker.hydrateFromServer({ throwOnError: true }))
        .find(item => item.id === downloadId || item.model_name === displayName);
    } catch (error) {
      const now = Date.now();
      snapshotErrorsStartedAt ??= now;
      if (now - snapshotErrorsStartedAt >= SERVER_DOWNLOAD_SNAPSHOT_ERROR_TIMEOUT_MS) {
        throw new Error(`Timed out refreshing backend download state: ${error instanceof Error ? error.message : 'Unknown error'}`);
      }

      console.warn('Failed to refresh backend download snapshot; keeping current state:', error);
      await new Promise(resolve => setTimeout(resolve, SERVER_DOWNLOAD_POLL_INTERVAL_MS));
      continue;
    }

    snapshotErrorsStartedAt = undefined;

    if (!snapshot) {
      if (sawServerJob) {
        consecutiveMissingSnapshots += 1;
        if (consecutiveMissingSnapshots >= 10) {
          if (await isBackendInstalledOnServer(recipe, backend)) {
            downloadTracker.completeDownload(downloadId);
            window.dispatchEvent(new CustomEvent('backendsUpdated'));
            return;
          }
          downloadTracker.removeDownload(downloadId);
          throw new DownloadAbortError('cancelled');
        }
      }
    } else {
      sawServerJob = true;
      consecutiveMissingSnapshots = 0;
      const stopped = snapshot.running !== true;
      if (snapshot.status === 'completed' && stopped) return;
      if (snapshot.status === 'paused' && stopped) throw new DownloadAbortError('paused');
      if (snapshot.status === 'cancelled' && stopped) throw new DownloadAbortError('cancelled');
      if (snapshot.status === 'error' && stopped) throw new Error(snapshot.error || 'Unknown backend install error');
    }

    await new Promise(resolve => setTimeout(resolve, SERVER_DOWNLOAD_POLL_INTERVAL_MS));
  }
}

async function consumeLegacyPullStream(response: Response, downloadId: string): Promise<void> {
  const reader = response.body?.getReader();
  if (!reader) throw new Error('No response body');

  const decoder = new TextDecoder();
  let buffer = '';
  let currentEventType = 'progress';
  let downloadCompleted = false;

  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;

      buffer += decoder.decode(value, { stream: true });
      const lines = buffer.split('\n');
      buffer = lines.pop() || '';

      for (const line of lines) {
        if (line.startsWith('event:')) {
          currentEventType = line.substring(6).trim();
        } else if (line.startsWith('data:')) {
          try {
            const data = JSON.parse(line.substring(5).trim());
            if (currentEventType === 'progress') {
              downloadTracker.updateProgress(downloadId, data);
            } else if (currentEventType === 'complete') {
              downloadTracker.completeDownload(downloadId);
              downloadCompleted = true;
            } else if (currentEventType === 'error') {
              throw new Error(data.error || 'Unknown download error');
            }
          } catch (parseError) {
            if (!(parseError instanceof SyntaxError)) throw parseError;
            console.error('Failed to parse SSE data:', line, parseError);
          }
        } else if (line.trim() === '') {
          currentEventType = 'progress';
        }
      }
    }
  } catch (streamError) {
    if (!downloadCompleted) throw streamError;
  }

  if (!downloadCompleted) {
    downloadTracker.completeDownload(downloadId);
  }
}

/**
 * Install a backend with Download Manager integration.
 * Calls POST /api/v1/install with SSE streaming and tracks progress.
 * This is the single codepath for all backend installations.
 */
export async function installBackend(
  recipe: string,
  backend: string,
  showInDownloadManager: boolean = true
): Promise<string | void> {
  const displayName = `${recipe}:${backend}`;
  const abortController = new AbortController();
  const downloadId = downloadTracker.getStableDownloadId(displayName, 'backend');

  if (showInDownloadManager) {
    downloadTracker.startDownload(displayName, abortController, 'backend');
    downloadTracker.startServerPolling();
    window.dispatchEvent(new CustomEvent('download:started', { detail: { modelName: displayName, downloadType: 'backend' } }));
  }

  try {
    const response = await serverFetch('/install', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ recipe, backend, stream: true, subscribe: false }),
      signal: abortController.signal,
      cache: 'no-store',
    });

    if (!response.ok) {
      const errorText = await response.text();
      throw new Error(`Failed: ${errorText || response.statusText}`);
    }

    const data = await response.json();
    if (data.action) {
      if (showInDownloadManager) {
        downloadTracker.completeDownload(downloadId);
      }
      window.dispatchEvent(new CustomEvent('open-external-content', { detail: { url: data.action } }));
      return 'action';
    }

    if (showInDownloadManager) {
      downloadTracker.applyServerDownload(data);
      await waitForBackendDownloadTerminal(downloadId, displayName, recipe, backend, abortController);
      // Backend completion is applied through the server snapshot; downloadTracker
      // emits backendsUpdated once for backend jobs when the terminal snapshot arrives.
    } else {
      await waitForBackendDownloadTerminal(downloadId, displayName, recipe, backend, abortController);
    }
  } catch (error: any) {
    if (error instanceof DownloadAbortError) {
      throw error;
    }

    if (error.name === 'AbortError') {
      downloadTracker.pauseDownload(downloadId, false);
      throw new DownloadAbortError('paused');
    }

    if (showInDownloadManager) {
      downloadTracker.failDownload(downloadId, error.message || 'Unknown error');
    }
    throw error;
  }
}

/**
 * Install the appropriate backend for a recipe before loading a model.
 * Uses recipe default backend (first backend in server-provided priority order).
 * Installs/updates when required and throws actionable errors when not viable.
 */
export async function ensureBackendForRecipe(
  recipe: string,
  recipes?: Recipes
): Promise<void> {
  if (!recipes || !recipes[recipe]) return;

  const recipeInfo = recipes[recipe];
  const defaultBackend = recipeInfo.default_backend;
  if (!defaultBackend) {
    throw new Error(`No supported backend available for recipe ${recipe}.`);
  }

  const backendInfo = recipeInfo.backends[defaultBackend];
  if (!backendInfo) {
    throw new Error(`Default backend '${defaultBackend}' not found for recipe ${recipe}.`);
  }

  // `update_available` is a soft signal: the backend is fully usable, GitHub
  // just has a newer tag. Don't block model flows on it.
  if (backendInfo.state === 'installed' || backendInfo.state === 'update_available') return;

  if (backendInfo.state === 'installable' || backendInfo.state === 'update_required') {
    const action = backendInfo.action || '';
    const htmlUrlMatch = action.match(/https?:\/\/[^\s]+\.html/);
    if (htmlUrlMatch) {
      window.dispatchEvent(new CustomEvent('open-external-content', { detail: { url: htmlUrlMatch[0] } }));
      throw new Error(backendInfo.message || `Please follow the guide to set up ${recipe}.`);
    }
    await installBackend(recipe, defaultBackend, true);
    return;
  }

  if (backendInfo.state === 'unsupported') {
    const reason = backendInfo.message || 'Backend is not supported on this system.';
    throw new Error(`${recipe}:${defaultBackend} is unsupported. ${reason}`);
  }

  throw new Error(`Unsupported backend state for ${recipe}:${defaultBackend}: ${backendInfo.state}`);
}

/**
 * Uninstall a backend. Single codepath for all backend removals.
 * Dispatches `backendsUpdated` on success so the system context refreshes.
 */
export async function uninstallBackend(recipe: string, backend: string): Promise<void> {
  const response = await serverFetch('/uninstall', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ recipe, backend }),
  });

  if (!response.ok) {
    const errorText = await response.text();
    throw new Error(extractServerErrorMessage(errorText, response.statusText));
  }

  window.dispatchEvent(new CustomEvent('backendsUpdated'));
}

/**
 * Delete a model's files. Single codepath for all model deletions.
 * Dispatches `modelsUpdated` on success so the models context refreshes.
 */
export async function deleteModel(modelName: string): Promise<void> {
  const response = await serverFetch('/delete', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ model_name: modelName }),
  });

  if (!response.ok) {
    const errorText = await response.text();
    throw new Error(errorText || response.statusText);
  }

  window.dispatchEvent(new CustomEvent('modelsUpdated'));
}

/**
 * Download a model with SSE progress tracking shown in the Download Manager.
 * This is the single codepath for all model downloads via POST /pull.
 *
 * Supports:
 * - Download Manager progress tracking (always on by default)
 * - Pause/cancel from Download Manager UI (throws DownloadAbortError)
 * - Custom model registration data
 *
 * The streaming download path dispatches `modelsUpdated` on success. The
 * `registrationOnly` fast path does not — it transfers no bytes, so its
 * callers are responsible for refreshing the models context.
 *
 * @throws DownloadAbortError if the user pauses or cancels via Download Manager
 * @throws Error on download failure
 */
export async function pullModel(
  modelName: string,
  options?: {
    registrationData?: ModelRegistrationData;
    showInDownloadManager?: boolean;
    collectionComponents?: string[];
    /** Declared model size in GB from the registry, used as the download
     *  total when the server can't emit a cumulative size (e.g. FLM pull). */
    declaredSizeGB?: number;
    /** Force a Hugging Face update check even when the model is already
     *  cached. Defaults to false (cache-first): an already-downloaded model is
     *  reused without contacting Hugging Face. Only explicit "update" actions
     *  should set this to true. */
    upgrade?: boolean;
    /** Persist a model/collection definition without downloading anything.
     *  Used when all required files are already present (e.g. saving an Omni
     *  collection whose components are downloaded): the server registers the
     *  record and transfers no bytes, so there is no progress to show. */
    registrationOnly?: boolean;
  }
): Promise<void> {
  // Registration-only fast path: persist the definition with a synchronous,
  // non-streaming pull. No Download Manager entry or SSE progress, because
  // nothing is downloaded. Stays cache-first unless the caller opts into upgrade.
  if (options?.registrationOnly) {
    const requestBody: Record<string, unknown> = {
      model_name: modelName,
      ...(options.registrationData ?? {}),
      stream: false,
      subscribe: false,
      do_not_upgrade: options.upgrade !== true,
    };
    const response = await serverFetch('/pull', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(requestBody),
      cache: 'no-store',
    });
    if (!response.ok) {
      const errorText = await response.text();
      throw new Error(errorText || response.statusText);
    }
    return;
  }

  const showInDownloadManager = options?.showInDownloadManager ?? true;
  const abortController = new AbortController();
  const declaredTotalBytes = options?.declaredSizeGB
    ? Math.round(options.declaredSizeGB * 1024 * 1024 * 1024)
    : undefined;
  const downloadId = downloadTracker.getStableDownloadId(modelName, 'model');

  if (showInDownloadManager) {
    downloadTracker.startDownload(
      modelName,
      abortController,
      'model',
      options?.collectionComponents,
      declaredTotalBytes,
    );
    downloadTracker.startServerPolling();
    window.dispatchEvent(new CustomEvent('download:started', { detail: { modelName } }));
  }

  let isPaused = false;
  let isCancelled = false;

  const handleCancel = (event: Event) => {
    const detail = (event as CustomEvent).detail;
    if (detail.modelName === modelName) {
      isCancelled = true;
      abortController.abort();
    }
  };

  const handlePause = (event: Event) => {
    const detail = (event as CustomEvent).detail;
    if (detail.modelName === modelName) {
      isPaused = true;
      abortController.abort();
    }
  };

  window.addEventListener('download:cancelled', handleCancel);
  window.addEventListener('download:paused', handlePause);

  try {
    const requestBody: Record<string, unknown> = {
      model_name: modelName,
      stream: true,
      subscribe: false,
    };

    if (options?.registrationData) {
      Object.assign(requestBody, options.registrationData);
    }

    // Cache-first by default: an already-downloaded model is reused instead of
    // triggering a Hugging Face update check (and a possible full re-download).
    // Set after registrationData so the helper's decision is authoritative.
    requestBody.do_not_upgrade = options?.upgrade !== true;

    const response = await serverFetch('/pull', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(requestBody),
      signal: abortController.signal,
      cache: 'no-store',
    });

    if (!response.ok) {
      const errorText = await response.text();
      throw new Error(`Failed to download model: ${errorText || response.statusText}`);
    }

    const contentType = response.headers.get('Content-Type') || '';
    if (contentType.includes('text/event-stream')) {
      await consumeLegacyPullStream(response, downloadId);
      window.dispatchEvent(new CustomEvent('modelsUpdated'));
      return;
    }

    const startedSnapshot = await response.json();
    downloadTracker.applyServerDownload(startedSnapshot);

    await waitForServerDownloadTerminal(
      downloadId,
      modelName,
      abortController,
      () => isCancelled ? 'cancelled' : (isPaused ? 'paused' : undefined),
      true,
    );

    // The terminal server snapshot is applied inside waitForServerDownloadTerminal,
    // which emits modelsUpdated once. Avoid a second local completion/update here.
  } catch (error: any) {
    if (error instanceof DownloadAbortError) {
      throw error;
    }

    if (error.name === 'AbortError') {
      if (isPaused) {
        downloadTracker.pauseDownload(downloadId, false);
        throw new DownloadAbortError('paused');
      }
      if (isCancelled) {
        downloadTracker.cancelDownload(downloadId, false);
        window.dispatchEvent(new CustomEvent('download:cleanup-complete', {
          detail: { id: downloadId, modelName }
        }));
        throw new DownloadAbortError('cancelled');
      }
      return;
    }

    downloadTracker.failDownload(downloadId, error.message || 'Unknown error');
    throw error;
  } finally {
    window.removeEventListener('download:cancelled', handleCancel);
    window.removeEventListener('download:paused', handlePause);
  }
}

/**
 * Extract an explicit backend selection from loadBody options.
 * Looks for keys ending in `_backend` with non-empty string values,
 * maps back to the frontend option name, and returns the associated recipe and backend.
 */
function extractExplicitBackend(loadBody?: Record<string, unknown>): { recipe: string; backend: string } | null {
  if (!loadBody) return null;

  for (const [apiKey, value] of Object.entries(loadBody)) {
    if (!apiKey.endsWith('_backend') || typeof value !== 'string' || !value) continue;

    const frontendKey = toFrontendOptionName(apiKey);
    const def = OPTION_DEFINITIONS[frontendKey];
    if (def && 'backendRecipe' in def && def.backendRecipe) {
      return { recipe: def.backendRecipe, backend: value };
    }
  }

  return null;
}

/**
 * Universal pre-flight check for all inference requests.
 * Ensures backend is installed, model is downloaded, and model is loaded —
 * all through tracked SSE paths visible in the Download Manager.
 *
 * Steps:
 * 1. GET /health → if model already loaded, return early (skip if options.skipHealthCheck)
 * 2. Call onModelLoading callback (lets UI show spinner)
 * 3. Fetch fresh system info → install backend if needed (tracked in Download Manager)
 * 4. Check if model is downloaded → re-verify via /models if uncertain
 * 5. If not downloaded, pull model (tracked in Download Manager)
 * 6. POST /load → load model into memory (merge loadBody if provided)
 */
export async function ensureModelReady(
  modelName: string,
  modelsData: ModelsData,
  options?: {
    onModelLoading?: () => void;
    skipHealthCheck?: boolean;
    loadBody?: Record<string, unknown>;
  },
): Promise<void> {
  await ensureModelReadyInternal(modelName, modelsData, options, new Set<string>());
}

async function ensureModelReadyInternal(
  modelName: string,
  modelsData: ModelsData,
  options: {
    onModelLoading?: () => void;
    skipHealthCheck?: boolean;
    loadBody?: Record<string, unknown>;
  } | undefined,
  visited: Set<string>,
): Promise<void> {
  if (visited.has(modelName)) {
    throw new Error(`Circular collection model dependency detected for "${modelName}".`);
  }
  visited.add(modelName);
  try {
    const modelInfo = modelsData[modelName];
    if (isCollectionModel(modelInfo)) {
      options?.onModelLoading?.();
      const components = getCollectionComponents(modelInfo);
      for (const component of components) {
        if (!modelsData[component]) {
          throw new Error(`Omni-model "${modelName}" references missing component "${component}".`);
        }
        await ensureModelReadyInternal(component, modelsData, {
          onModelLoading: options?.onModelLoading,
          skipHealthCheck: options?.skipHealthCheck,
        }, visited);
      }
      return;
    }

    // Step 1: Check if model is already loaded via health endpoint
    if (!options?.skipHealthCheck) {
      try {
        const healthResponse = await serverFetch('/health');
        if (healthResponse.ok) {
          const healthData = await healthResponse.json();
          const allLoaded: any[] = healthData.all_models_loaded || [];
          const isLoaded = allLoaded.some(
            (m: any) => m.model_name === modelName
          );
          if (isLoaded) {
            return; // Model is already loaded — fast path
          }
        }
      } catch {
        // Health check failed — continue with the full pre-flight
      }
    }

    // Step 2: Signal UI that model loading is in progress
    options?.onModelLoading?.();

    // Step 3: Ensure backend is installed (fetch fresh system info to avoid stale closure)
    const recipe = modelInfo?.recipe;
    if (recipe) {
      // If loadBody specifies an explicit backend, install that one specifically
      const selectedBackendOptions = {
        ...(modelInfo?.recipe_options ?? {}),
        ...(options?.loadBody ?? {}),
      };
      const explicitBackend = extractExplicitBackend(selectedBackendOptions);
      if (explicitBackend) {
        const freshSystemData = await fetchSystemInfoData();
        const freshRecipes = freshSystemData.info?.recipes;
        const recipeInfo = freshRecipes?.[explicitBackend.recipe];
        const backendInfo = recipeInfo?.backends?.[explicitBackend.backend];

        if (backendInfo) {
          if (backendInfo.state === 'installable' || backendInfo.state === 'update_required') {
            await installBackend(explicitBackend.recipe, explicitBackend.backend, true);
          } else if (backendInfo.state === 'unsupported') {
            const reason = backendInfo.message || 'Backend is not supported on this system.';
            throw new Error(`${explicitBackend.recipe}:${explicitBackend.backend} is unsupported. ${reason}`);
          }
        } else {
          throw new Error(`Selected backend not found: ${explicitBackend.recipe}:${explicitBackend.backend}`);
        }
      } else {
        const freshSystemData = await fetchSystemInfoData();
        const freshRecipes = freshSystemData.info?.recipes;
        await ensureBackendForRecipe(recipe, freshRecipes);
      }
    }

    // Step 4: Check if model is downloaded
    let isDownloaded = modelInfo?.downloaded === true;
    if (!isDownloaded) {
      // Re-verify via /models?show_all=true in case modelsData is stale
      try {
        const modelsResponse = await serverFetch('/models?show_all=true');
        if (modelsResponse.ok) {
          const data = await modelsResponse.json();
          const modelList = Array.isArray(data) ? data : data.data || [];
          const freshModel = modelList.find((m: any) => m.id === modelName);
          isDownloaded = freshModel?.downloaded === true;
        }
      } catch {
        // If re-check fails, proceed — pull will be a no-op if already downloaded
      }
    }

    // Step 5: Pull model if not downloaded (shows in Download Manager)
    if (!isDownloaded) {
      await pullModel(modelName, { declaredSizeGB: modelsData[modelName]?.size });
    }

    // Step 6: Load model into memory (merge loadBody if provided)
    const loadModel = async () => {
      const loadPayload: Record<string, unknown> = { model_name: modelName, ...options?.loadBody };
      const loadResponse = await serverFetch('/load', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(loadPayload),
      });

      if (!loadResponse.ok) {
        const errorData = await loadResponse.json().catch(() => ({}));
        const errorMsg = (typeof errorData.error === 'string' ? errorData.error : errorData.error?.message) || `Failed to load model: ${loadResponse.statusText}`;

        throw new Error(errorMsg);
      }
    };

    await loadModel();
  } finally {
    visited.delete(modelName);
  }
}
