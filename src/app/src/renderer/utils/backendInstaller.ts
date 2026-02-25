import { downloadTracker } from './downloadTracker';
import { serverFetch } from './serverConfig';
import { fetchSystemInfoData, Recipes } from './systemData';
import { ModelsData } from './modelData';
import { toFrontendOptionName, OPTION_DEFINITIONS } from '../recipes/recipeOptionsConfig';

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
  checkpoint: string;
  recipe: string;
  mmproj?: string;
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

/**
 * Install a backend with Download Manager integration.
 * Calls POST /api/v1/install with SSE streaming and tracks progress.
 * This is the single codepath for all backend installations.
 */
export async function installBackend(
  recipe: string,
  backend: string,
  showInDownloadManager: boolean = true
): Promise<void> {
  const displayName = `${recipe}:${backend}`;
  const abortController = new AbortController();

  let downloadId: string | undefined;
  if (showInDownloadManager) {
    downloadId = downloadTracker.startDownload(displayName, abortController, 'backend');
    window.dispatchEvent(new CustomEvent('download:started', { detail: { modelName: displayName } }));
  }

  let isPaused = false;
  let isCancelled = false;
  let downloadCompleted = false;

  // Listen for pause/cancel events from Download Manager UI
  const handleCancel = (event: Event) => {
    const detail = (event as CustomEvent).detail;
    if (detail.modelName === displayName) {
      isCancelled = true;
      abortController.abort();
    }
  };
  const handlePause = (event: Event) => {
    const detail = (event as CustomEvent).detail;
    if (detail.modelName === displayName) {
      isPaused = true;
      abortController.abort();
    }
  };

  window.addEventListener('download:cancelled', handleCancel);
  window.addEventListener('download:paused', handlePause);

  try {
    const response = await serverFetch('/install', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ recipe, backend, stream: true }),
      signal: abortController.signal,
    });

    if (!response.ok) {
      const errorText = await response.text();
      throw new Error(`Failed to install backend: ${errorText || response.statusText}`);
    }

    const reader = response.body?.getReader();
    if (!reader) {
      throw new Error('No response body');
    }

    const decoder = new TextDecoder();
    let buffer = '';
    let currentEventType = 'progress';

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

              if (currentEventType === 'progress' && downloadId) {
                downloadTracker.updateProgress(downloadId, data);
              } else if (currentEventType === 'complete') {
                if (downloadId) {
                  downloadTracker.completeDownload(downloadId);
                }
                downloadCompleted = true;
              } else if (currentEventType === 'error') {
                const errorMsg = data.error || 'Unknown error';
                if (downloadId) {
                  downloadTracker.failDownload(downloadId, errorMsg);
                }
                throw new Error(errorMsg);
              }
            } catch (parseError) {
              // Re-throw application errors (e.g. from 'error' events); only
              // swallow JSON parse failures so the stream can continue.
              if (!(parseError instanceof SyntaxError)) {
                throw parseError;
              }
              console.error('Failed to parse SSE data:', line, parseError);
            }
          } else if (line.trim() === '') {
            currentEventType = 'progress';
          }
        }
      }
    } catch (streamError) {
      // If we already got the complete event, ignore stream errors
      if (!downloadCompleted) {
        throw streamError;
      }
    }

    if (!downloadCompleted && downloadId) {
      downloadTracker.completeDownload(downloadId);
    }

    // Notify system context so BackendManager updates its status
    window.dispatchEvent(new CustomEvent('backendsUpdated'));
  } catch (error: any) {
    // If download completed successfully, ignore any connection-close errors
    if (downloadCompleted) {
      window.dispatchEvent(new CustomEvent('backendsUpdated'));
      return;
    }

    if (error.name === 'AbortError') {
      if (isPaused && downloadId) {
        downloadTracker.pauseDownload(downloadId);
        throw new DownloadAbortError('paused');
      } else {
        if (downloadId) {
          downloadTracker.cancelDownload(downloadId);
        }
        // Signal that file handles are released so DownloadManager can clean up
        window.dispatchEvent(new CustomEvent('download:cleanup-complete', {
          detail: { id: downloadId, modelName: displayName }
        }));
        throw new DownloadAbortError('cancelled');
      }
    } else {
      if (downloadId) {
        downloadTracker.failDownload(downloadId, error.message || 'Unknown error');
      }
      throw error;
    }
  } finally {
    window.removeEventListener('download:cancelled', handleCancel);
    window.removeEventListener('download:paused', handlePause);
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

  if (backendInfo.state === 'installed') return;

  if (backendInfo.state === 'installable' || backendInfo.state === 'update_required') {
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
 * - Dispatches `modelsUpdated` event on success
 *
 * @throws DownloadAbortError if the user pauses or cancels via Download Manager
 * @throws Error on download failure
 */
export async function pullModel(
  modelName: string,
  options?: {
    registrationData?: ModelRegistrationData;
    showInDownloadManager?: boolean;
  }
): Promise<void> {
  const showInDownloadManager = options?.showInDownloadManager ?? true;
  const abortController = new AbortController();

  let downloadId: string | undefined;
  if (showInDownloadManager) {
    downloadId = downloadTracker.startDownload(modelName, abortController, 'model');
    window.dispatchEvent(new CustomEvent('download:started', { detail: { modelName } }));
  }

  let downloadCompleted = false;
  let isPaused = false;
  let isCancelled = false;

  // Listen for pause/cancel events from Download Manager UI
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
    // Build request body — include registration data for custom models
    const requestBody: Record<string, unknown> = { model_name: modelName, stream: true };
    if (options?.registrationData) {
      const rd = options.registrationData;
      requestBody.checkpoint = rd.checkpoint;
      requestBody.recipe = rd.recipe;
      if (rd.mmproj) requestBody.mmproj = rd.mmproj;
      if (rd.reasoning) requestBody.reasoning = rd.reasoning;
      if (rd.vision) requestBody.vision = rd.vision;
      if (rd.embedding) requestBody.embedding = rd.embedding;
      if (rd.reranking) requestBody.reranking = rd.reranking;
    }

    const response = await serverFetch('/pull', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(requestBody),
      signal: abortController.signal,
    });

    if (!response.ok) {
      const errorText = await response.text();
      throw new Error(`Failed to download model: ${errorText || response.statusText}`);
    }

    const reader = response.body?.getReader();
    if (!reader) {
      throw new Error('No response body');
    }

    const decoder = new TextDecoder();
    let buffer = '';
    let currentEventType = 'progress';

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
            // Parse JSON separately so server errors aren't swallowed
            let data;
            try {
              data = JSON.parse(line.substring(5).trim());
            } catch {
              console.error('Failed to parse SSE data:', line);
              continue;
            }

            if (currentEventType === 'progress' && downloadId) {
              downloadTracker.updateProgress(downloadId, data);
            } else if (currentEventType === 'complete') {
              if (downloadId) {
                downloadTracker.completeDownload(downloadId);
              }
              downloadCompleted = true;
            } else if (currentEventType === 'error') {
              const errorMsg = data.error || 'Unknown error';
              if (downloadId) {
                downloadTracker.failDownload(downloadId, errorMsg);
              }
              throw new Error(errorMsg);
            }
          } else if (line.trim() === '') {
            currentEventType = 'progress';
          }
        }
      }
    } catch (streamError) {
      // If we already got the complete event, ignore stream errors
      if (!downloadCompleted) {
        throw streamError;
      }
    }

    if (!downloadCompleted && downloadId) {
      downloadTracker.completeDownload(downloadId);
    }

    // Notify all components that models have been updated
    window.dispatchEvent(new CustomEvent('modelsUpdated'));
  } catch (error: any) {
    // If download completed successfully, ignore any connection-close errors
    if (downloadCompleted) {
      window.dispatchEvent(new CustomEvent('modelsUpdated'));
      return;
    }

    if (error.name === 'AbortError') {
      if (isPaused && downloadId) {
        downloadTracker.pauseDownload(downloadId);
        throw new DownloadAbortError('paused');
      } else {
        if (downloadId) {
          downloadTracker.cancelDownload(downloadId);
        }
        // Signal that file handles are released so DownloadManager can clean up
        window.dispatchEvent(new CustomEvent('download:cleanup-complete', {
          detail: { id: downloadId, modelName }
        }));
        throw new DownloadAbortError('cancelled');
      }
    } else {
      if (downloadId) {
        downloadTracker.failDownload(downloadId, error.message || 'Unknown error');
      }
      throw error;
    }
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
  const modelInfo = modelsData[modelName];
  const recipe = modelInfo?.recipe;
  if (recipe) {
    // If loadBody specifies an explicit backend, install that one specifically
    const explicitBackend = extractExplicitBackend(options?.loadBody);
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
    await pullModel(modelName);
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
      const errorMsg = errorData.error || `Failed to load model: ${loadResponse.statusText}`;

      // If model was invalidated (e.g., corrupted or removed), re-pull and retry once
      if (errorMsg.includes('model_invalidated') || errorMsg.includes('not found')) {
        console.warn('Model invalidated, re-pulling and retrying load...');
        await pullModel(modelName);
        const retryResponse = await serverFetch('/load', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(loadPayload),
        });
        if (!retryResponse.ok) {
          const retryError = await retryResponse.json().catch(() => ({}));
          throw new Error(retryError.error || `Failed to load model after re-pull: ${retryResponse.statusText}`);
        }
        return;
      }

      throw new Error(errorMsg);
    }
  };

  await loadModel();
}
