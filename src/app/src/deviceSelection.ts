/** Helpers for the per-model GPU selectors. A selection names a device on one
    server, so it is only meaningful against that server's own device list. */

export function isAutomaticDevice(value: string | undefined | null): boolean {
  const text = (value ?? '').trim().toLowerCase();
  return text === '' || text === 'auto';
}

/** The entry of `options` that names the same device as `value`, ignoring case
    (the CLI and API docs spell devices `CUDA1`, the app writes `cuda1`). */
export function matchingDeviceOption(value: string | undefined | null, options: readonly string[]): string | undefined {
  const wanted = (value ?? '').trim().toLowerCase();
  if (!wanted) return undefined;
  return options.find(option => option.toLowerCase() === wanted);
}

/** True when the selection can be applied on a server that offers `available`.
    Automatic is always applicable. */
export function deviceSelectionIsAvailable(value: string | undefined | null, available: readonly string[]): boolean {
  return isAutomaticDevice(value) || matchingDeviceOption(value, available) !== undefined;
}

/** The host and port of the server the app is talking to, for labelling which
    machine a device list belongs to. */
export function serverLabel(baseUrl: string): string {
  try {
    return new URL(baseUrl).host || baseUrl;
  } catch {
    return baseUrl;
  }
}
