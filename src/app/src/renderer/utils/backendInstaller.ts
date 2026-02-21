import { downloadTracker } from './downloadTracker';
import { serverFetch } from './serverConfig';
import { Recipes } from './systemData';

/**
 * Install a backend with optional Download Manager integration.
 * Calls POST /api/v1/install with SSE streaming and tracks progress.
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
    downloadId = downloadTracker.startDownload(displayName, abortController);
    window.dispatchEvent(new CustomEvent('download:started', { detail: { modelName: displayName } }));
  }

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
    let completed = false;

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
              completed = true;
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

    if (!completed && downloadId) {
      downloadTracker.completeDownload(downloadId);
    }
  } catch (error: any) {
    if (error.name === 'AbortError') {
      if (downloadId) {
        downloadTracker.cancelDownload(downloadId);
      }
    } else {
      if (downloadId) {
        downloadTracker.failDownload(downloadId, error.message || 'Unknown error');
      }
      throw error;
    }
  }
}

/**
 * Install the appropriate backend for a recipe before loading a model.
 * Picks the first supported backend. If already installed, returns immediately.
 * Silently ignores errors so model loading can still proceed (server installs as fallback).
 */
export async function ensureBackendForRecipe(
  recipe: string,
  recipes?: Recipes
): Promise<void> {
  if (!recipes || !recipes[recipe]) return;

  const recipeInfo = recipes[recipe];
  const backends = Object.entries(recipeInfo.backends);

  // Find first supported backend that needs installation
  const needsInstall = backends.find(
    ([, info]) => info.supported && !info.available
  );

  if (!needsInstall) return; // All supported backends are installed

  const [backendName] = needsInstall;
  try {
    await installBackend(recipe, backendName, true);
  } catch (error) {
    // Log but don't throw - server will attempt install during load as fallback
    console.warn(`Backend pre-install failed for ${recipe}:${backendName}:`, error);
  }
}
