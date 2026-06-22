import { serverFetch } from './serverConfig';

export async function fetchRuntimeConfig(): Promise<Record<string, unknown>> {
  const response = await serverFetch('/internal/config');
  if (!response.ok) {
    throw new Error(`Failed to load server config (${response.status})`);
  }
  return response.json();
}

export async function updateRuntimeConfig(changes: Record<string, unknown>): Promise<void> {
  const response = await serverFetch('/internal/set', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(changes),
  });

  if (!response.ok) {
    let message = `Failed to update server config (${response.status})`;
    try {
      const body: unknown = await response.json();
      if (typeof body === 'object' && body !== null && typeof (body as { error?: unknown }).error === 'string') {
        message = (body as { error: string }).error;
      }
    } catch {
      // ignore parse errors
    }
    throw new Error(message);
  }
}

export async function fetchDefaultModel(): Promise<string> {
  const config = await fetchRuntimeConfig();
  return typeof config.default_model === 'string' ? config.default_model : '';
}

export async function saveDefaultModel(model: string): Promise<void> {
  await updateRuntimeConfig({ default_model: model });
}
