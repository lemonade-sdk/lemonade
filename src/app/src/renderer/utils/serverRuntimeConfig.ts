/**
 * Helpers for reading and writing lemond's server-wide runtime configuration
 * (config.json) from the renderer.
 *
 * Server-wide config (e.g. `models_dir`) is fundamentally different from the
 * per-client preferences stored in `app_settings.json`: it is shared by every
 * client connected to the same `lemond` instance and therefore MUST NOT be
 * mirrored into `AppSettings`. This module talks directly to lemond over HTTP,
 * mirroring the pattern used for `/health` and `/system-stats` (see
 * `StatusBar.tsx`, `AboutModal.tsx`, and the comment in `tauriShim.ts`).
 *
 * Endpoints used:
 *   GET  /internal/config           → full config JSON snapshot
 *   POST /internal/set              → { key: value, ... } partial update
 *
 * These live at the server root, not under `/api/v1`, so we build absolute
 * URLs from `serverConfig.getServerBaseUrl()` rather than calling
 * `serverConfig.fetch()` (which auto-prefixes `/api/v1`).
 */

import { serverConfig } from './serverConfig';

const MODELS_DIR_AUTO = 'auto';

export type RuntimeConfigStatus =
  | { kind: 'ok'; modelsDir: string; extraModelsDir: string }
  | { kind: 'unauthorized' }
  | { kind: 'error'; message: string };

export type RuntimeConfigUpdateResult =
  | { kind: 'ok' }
  | { kind: 'unauthorized' }
  | { kind: 'error'; message: string };

function buildInternalUrl(path: string): string {
  const base = serverConfig.getServerBaseUrl().replace(/\/+$/, '');
  return `${base}${path.startsWith('/') ? path : `/${path}`}`;
}

function authHeaders(): Record<string, string> {
  const key = serverConfig.getAPIKey();
  return key ? { Authorization: `Bearer ${key}` } : {};
}

function classifyHttpError(status: number, body: string): RuntimeConfigStatus & RuntimeConfigUpdateResult {
  if (status === 401 || status === 403) {
    // /internal/* may require LEMONADE_ADMIN_API_KEY rather than the regular
    // LEMONADE_API_KEY; surface that distinctly so callers can guide the user.
    return { kind: 'unauthorized' } as RuntimeConfigStatus & RuntimeConfigUpdateResult;
  }
  return {
    kind: 'error',
    message: `Server returned ${status}${body ? `: ${body.slice(0, 200)}` : ''}`,
  } as RuntimeConfigStatus & RuntimeConfigUpdateResult;
}

/**
 * Fetch the current runtime configuration from lemond.
 *
 * Returns `{ kind: 'unauthorized' }` when the server rejects the request for
 * auth reasons; the UI should explain that admin-level access is required.
 */
export async function getRuntimeConfig(): Promise<RuntimeConfigStatus> {
  await serverConfig.waitForInit();
  try {
    const response = await fetch(buildInternalUrl('/internal/config'), {
      headers: authHeaders(),
    });
    if (!response.ok) {
      const text = await response.text().catch(() => '');
      return classifyHttpError(response.status, text) as RuntimeConfigStatus;
    }
    const data = await response.json();
    const modelsDir = typeof data?.models_dir === 'string' ? data.models_dir : MODELS_DIR_AUTO;
    const extraModelsDir = typeof data?.extra_models_dir === 'string' ? data.extra_models_dir : '';
    return { kind: 'ok', modelsDir, extraModelsDir };
  } catch (error) {
    return {
      kind: 'error',
      message: error instanceof Error ? error.message : String(error),
    };
  }
}

async function postInternalSet(body: Record<string, unknown>): Promise<RuntimeConfigUpdateResult> {
  await serverConfig.waitForInit();
  try {
    const response = await fetch(buildInternalUrl('/internal/set'), {
      method: 'POST',
      headers: {
        ...authHeaders(),
        'Content-Type': 'application/json',
      },
      body: JSON.stringify(body),
    });
    if (!response.ok) {
      const text = await response.text().catch(() => '');
      return classifyHttpError(response.status, text) as RuntimeConfigUpdateResult;
    }
    return { kind: 'ok' };
  } catch (error) {
    return {
      kind: 'error',
      message: error instanceof Error ? error.message : String(error),
    };
  }
}

/**
 * Set `models_dir` to an absolute path on the server's machine.
 *
 * The server applies the change immediately and invalidates its model cache;
 * no restart is required. Callers should refresh any model listing afterward.
 */
export function setModelsDir(path: string): Promise<RuntimeConfigUpdateResult> {
  return postInternalSet({ models_dir: path });
}

/**
 * Reset `models_dir` to the `"auto"` sentinel, which makes lemond honor the
 * user's `HF_HOME` / `HF_HUB_CACHE` env vars (defaulting to
 * `~/.cache/huggingface/hub`). See `docs/embeddable/models.md`.
 */
export function resetModelsDir(): Promise<RuntimeConfigUpdateResult> {
  return postInternalSet({ models_dir: MODELS_DIR_AUTO });
}

/**
 * Set `extra_models_dir` to an absolute path on the server's machine. The
 * server recursively scans this directory for loose `.gguf` files and exposes
 * them under the `extra.` prefix. Pass an empty string to disable the feature
 * (use `clearExtraModelsDir` for clarity).
 */
export function setExtraModelsDir(path: string): Promise<RuntimeConfigUpdateResult> {
  return postInternalSet({ extra_models_dir: path });
}

/**
 * Clear `extra_models_dir` (writes the empty-string default), disabling the
 * loose-GGUF discovery feature. See `docs/embeddable/models.md`.
 */
export function clearExtraModelsDir(): Promise<RuntimeConfigUpdateResult> {
  return postInternalSet({ extra_models_dir: '' });
}

export const MODELS_DIR_AUTO_SENTINEL = MODELS_DIR_AUTO;

export function isModelsDirAuto(value: string): boolean {
  return value.trim().toLowerCase() === MODELS_DIR_AUTO;
}

export function isExtraModelsDirEnabled(value: string): boolean {
  return value.trim() !== '';
}
