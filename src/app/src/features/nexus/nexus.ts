import { useEffect, useSyncExternalStore } from 'react';

export interface NexusDevice {
  node_id: string;
  device_name: string;
  tunnel_ip: string;
  management_pubkey: string;
  permissions: string[];
  can_remove: boolean;
  is_online?: boolean;
  latency_ms?: number;
}
export interface NexusStatus {
  controller_url?: string;
  available: boolean;
  enabled: boolean;
  registered?: boolean;
  locked: boolean;
  mesh_up: boolean;
  status: string;
  node_id?: string;
  group_node_id?: string;
  tunnel_ip?: string;
  device_name?: string;
  peer_count?: number;
  members: NexusDevice[];
  error?: string;
  event_sequence?: number;
}
export interface NexusInvite {
  code?: string;
  link_token: string;
  expires_at: number;
  device_name: string;
}
interface PasskeyChallenge {
  challenge: string;
  user_id: string;
  rp_id: string;
  credential_id: string;
  registration: boolean;
}
export interface PasskeyPrompt { message: string; needs_pin: boolean; rp_id: string }
interface NetworkState { selectedServerId?: string; switchingServer?: boolean; passkey?: PasskeyPrompt; status: NexusStatus | null; error: string | null; notice: string | null }
let state: NetworkState = { status: null, error: null, notice: null };
const listeners = new Set<() => void>();
const peerKeys = new Map<string, string>();
let subscribers = 0;
let timer: ReturnType<typeof setTimeout> | undefined;
let pollRunning = false;
let eventStream: AbortController | undefined;
let localController: { source: string; endpoint: string; key: string } | undefined;
let lastMemberIds: Set<string> | undefined;
const publish = (next: NetworkState) => { state = next; listeners.forEach(listener => listener()); };

export function isNexusDesktop(): boolean {
  return typeof window !== 'undefined' && Boolean(window.api && window.api.isWebApp !== true);
}
export function encodeBase64Url(buffer: ArrayBuffer): string {
  return btoa(String.fromCharCode(...new Uint8Array(buffer))).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}
export function decodeBase64Url(value: string): Uint8Array<ArrayBuffer> {
  const encoded = value.replace(/-/g, '+').replace(/_/g, '/');
  return Uint8Array.from(atob(encoded + '='.repeat((4 - encoded.length % 4) % 4)), c => c.charCodeAt(0));
}
async function controller() {
  const { default: api } = await import('../../api');
  await api.loadConnectionSettings();
  if (localController?.source === api.baseUrl) localController.key = api.apiKey;
  if (!localController) {
    const url = new URL(api.baseUrl);
    if (!['localhost', '127.0.0.1', '[::1]'].includes(url.hostname)) {
      throw new Error('Connect to this device’s local Lemonade server to manage its network.');
    }
    const endpoint = api.baseUrl;
    const key = api.apiKey;
    const response = await fetch(`${endpoint}/api/v1/nexus/status`, { headers: key ? { Authorization: `Bearer ${key}` } : {}, signal: AbortSignal.timeout(10000) });
    if (!response.ok) throw new Error(`Cannot access the local network controller (${response.status}).`);
    const status = await response.json() as NexusStatus;
    const control = new URL(status.controller_url || endpoint);
    if (control.protocol !== 'http:' || control.hostname !== '127.0.0.1') throw new Error('The server returned an invalid local network controller.');
    localController = { source: endpoint, endpoint: control.origin, key };
  }
  return localController;
}
export async function nexusRequest<T>(path: string, body?: unknown): Promise<T> {
  const local = await controller();
  const response = await fetch(`${local.endpoint}/api/v1/nexus/${path}`, {
    method: body === undefined ? 'GET' : 'POST',
    headers: { ...(body === undefined ? {} : { 'Content-Type': 'application/json' }),
      ...(local.key ? { Authorization: `Bearer ${local.key}` } : {}) },
    ...(body === undefined ? {} : { body: JSON.stringify(body) }),
    cache: 'no-store',
    signal: AbortSignal.timeout(35000),
  });
  const data = await response.json();
  if (!response.ok) throw new Error(typeof data?.error === 'string' ? data.error : `Network request failed (${response.status}).`);
  return data as T;
}
export async function refreshNexus(): Promise<void> {
  if (pollRunning) return;
  pollRunning = true;
  try {
    const status = await nexusRequest<NexusStatus>('status');
    let notice = state.notice;
    if (lastMemberIds && state.status?.mesh_up && status.mesh_up) {
      const joined = status.members.filter(member => !lastMemberIds!.has(member.node_id));
      if (joined.length) notice = `${joined.map(member => member.device_name || 'A device').join(', ')} joined your group.`;
    }
    lastMemberIds = status.mesh_up ? new Set(status.members.map(member => member.node_id)) : undefined;
    publish({ ...state, status, error: null, notice });
  } catch (error) {
    publish({ ...state, error: error instanceof Error ? error.message : String(error) });
  } finally { pollRunning = false; }
}
async function streamEvents() {
  const abort = new AbortController();
  eventStream = abort;
  try {
    const local = await controller();
    const response = await fetch(`${local.endpoint}/api/v1/nexus/events?after=${state.status?.event_sequence || 0}`, {
      headers: local.key ? { Authorization: `Bearer ${local.key}` } : {}, signal: abort.signal,
    });
    if (!response.ok || !response.body) return;
    const reader = response.body.getReader();
    const decoder = new TextDecoder();
    let buffer = '';
    while (!abort.signal.aborted) {
      const { value, done } = await reader.read();
      if (done) break;
      buffer += decoder.decode(value, { stream: true });
      let boundary: number;
      while ((boundary = buffer.indexOf('\n\n')) >= 0) {
        const event = buffer.slice(0, boundary); buffer = buffer.slice(boundary + 2);
        if (event.includes('\ndata: ')) void refreshNexus();
      }
      if (buffer.length > 65536) break;
    }
    await reader.cancel();
  } catch { /* Polling continues when the event stream is unavailable. */ }
  finally { if (eventStream === abort) eventStream = undefined; }
}
async function poll() {
  await refreshNexus();
  if (subscribers) timer = setTimeout(poll, 5000);
}
const subscribe = (listener: () => void) => { listeners.add(listener); return () => { listeners.delete(listener); }; };
export function useNexusNetwork(active = true): NetworkState {
  useEffect(() => {
    if (!active) return;
    if (++subscribers === 1) { void poll(); void streamEvents(); }
    return () => { if (--subscribers === 0) { clearTimeout(timer); eventStream?.abort(); } };
  }, [active]);
  return useSyncExternalStore(subscribe, () => state);
}
export function dismissNexusNotice() { publish({ ...state, notice: null }); }


function nativePasskeys(): boolean { return isNexusDesktop() && '__TAURI_INTERNALS__' in window; }
async function nativePasskey<T>(options: PasskeyChallenge, deviceName: string, registration: boolean): Promise<T> {
  const [{ invoke }, { listen }] = await Promise.all([import('@tauri-apps/api/core'), import('@tauri-apps/api/event')]);
  const update = (message: string, needs_pin: boolean) => publish({ ...state, passkey: { message, needs_pin, rp_id: options.rp_id } });
  const unlisten = await listen<{ message: string; needs_pin: boolean }>('nexus:passkey-status', event => update(event.payload.message, event.payload.needs_pin));
  update('Connect your FIDO2 security key. Verification may require its PIN or fingerprint.', false);
  try { return await invoke<T>('nexus_passkey', { options: { ...options, device_name: deviceName, registration } }); }
  finally { unlisten(); publish({ ...state, passkey: undefined }); }
}
export async function sendNexusPasskeyPin(pin: string): Promise<void> {
  const { invoke } = await import('@tauri-apps/api/core');
  await invoke('nexus_passkey_pin', { pin });
  if (state.passkey) publish({ ...state, passkey: { ...state.passkey, needs_pin: false, message: 'Touch your security key to continue.' } });
}
export async function cancelNexusPasskey(): Promise<void> {
  const { invoke } = await import('@tauri-apps/api/core');
  await invoke('nexus_passkey_cancel');
}

function checkPasskeyContext(rpId: string) {
  if (!isNexusDesktop()) throw new Error('Use the desktop app to manage passkeys.');
  if (!window.isSecureContext || !navigator.credentials || !window.PublicKeyCredential) {
    throw new Error('This desktop webview does not support passkeys. Update Lemonade and your operating system.');
  }
  const host = window.location.hostname;
  if (host !== rpId && !host.endsWith(`.${rpId}`)) {
    throw new Error('The desktop passkey origin does not match the network’s passkey domain. A compatible native passkey configuration is required.');
  }
}
const toHex = (bytes: Uint8Array) => Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('');
export async function registerNexusPasskey(deviceName: string): Promise<void> {
  const options = await nexusRequest<PasskeyChallenge>('passkey/challenge?register=1');
  if (nativePasskeys()) {
    const result = await nativePasskey<Record<string, string>>(options, deviceName, true);
    await nexusRequest('passkey/register', result);
    return;
  }
  checkPasskeyContext(options.rp_id);
  const credential = await navigator.credentials.create({ publicKey: {
    challenge: decodeBase64Url(options.challenge),
    rp: { id: options.rp_id, name: 'Lemonade Nexus' },
    user: { id: new TextEncoder().encode(options.user_id), name: options.user_id, displayName: deviceName },
    pubKeyCredParams: [{ type: 'public-key', alg: -7 }],
    authenticatorSelection: { authenticatorAttachment: 'platform', residentKey: 'discouraged', userVerification: 'required' },
    attestation: 'none', timeout: 60000,
  } }) as PublicKeyCredential | null;
  if (!credential) throw new Error('Passkey registration was cancelled.');
  const response = credential.response as AuthenticatorAttestationResponse;
  const spki = response.getPublicKey?.();
  if (!spki) throw new Error('Your desktop webview cannot export passkey public keys. Update Lemonade.');
  const key = await crypto.subtle.importKey('spki', spki, { name: 'ECDSA', namedCurve: 'P-256' }, true, ['verify']);
  const publicKey = await crypto.subtle.exportKey('jwk', key);
  if (!publicKey.x || !publicKey.y) throw new Error('The passkey did not return a P-256 public key.');
  await nexusRequest('passkey/register', { credential_id: encodeBase64Url(credential.rawId),
    public_key_x: toHex(decodeBase64Url(publicKey.x)), public_key_y: toHex(decodeBase64Url(publicKey.y)) });
}
export async function unlockNexus(deferJoin = false): Promise<void> {
  const options = await nexusRequest<PasskeyChallenge>('passkey/challenge');
  if (!options.credential_id) throw new Error('Create a passkey for this device first.');
  if (nativePasskeys()) {
    const assertion = await nativePasskey<Record<string, string>>(options, '', false);
    await nexusRequest('passkey/unlock', { defer_join: deferJoin, passkey_assertion: assertion });
    return;
  }
  checkPasskeyContext(options.rp_id);
  const credential = await navigator.credentials.get({ publicKey: {
    challenge: decodeBase64Url(options.challenge), rpId: options.rp_id,
    allowCredentials: [{ id: decodeBase64Url(options.credential_id), type: 'public-key' }],
    userVerification: 'required', timeout: 60000,
  } }) as PublicKeyCredential | null;
  if (!credential) throw new Error('Passkey confirmation was cancelled.');
  const response = credential.response as AuthenticatorAssertionResponse;
  await nexusRequest('passkey/unlock', { defer_join: deferJoin, passkey_assertion: {
    credential_id: encodeBase64Url(credential.rawId), authenticator_data: encodeBase64Url(response.authenticatorData),
    client_data_json: encodeBase64Url(response.clientDataJSON), signature: encodeBase64Url(response.signature),
  } });
}
export async function prepareNexus(deviceName: string, joining: boolean) {
  await nexusRequest('enable', {});
  let status: NexusStatus | undefined;
  for (let attempt = 0; attempt < 20; attempt++) {
    status = await nexusRequest<NexusStatus>('status');
    if (status.status !== 'starting') break;
    await new Promise(resolve => setTimeout(resolve, 250));
  }
  if (!status || status.status === 'starting') throw new Error('Network is still starting. Try again shortly.');
  await nexusRequest('device', { device_name: deviceName });
  if (!status.registered) await registerNexusPasskey(deviceName);
  await unlockNexus(joining);
}
export function nexusStatusLabel(status: NexusStatus | null): string {
  if (!status?.enabled) return 'Mesh off';
  if (status.locked) return 'Mesh locked';
  if (status.mesh_up) return 'Mesh connected';
  if (status.status === 'joining' || status.status === 'starting') return 'Mesh connecting';
  return 'Mesh offline';
}

export async function selectNexusInferenceServer(nodeId: string, suppliedKey?: string): Promise<void> {
  if (state.switchingServer) throw new Error('A server switch is already in progress.');
  const local = await controller();
  const { default: api } = await import('../../api');
  const previousEndpoint = api.baseUrl;
  const previousKey = api.apiKey;
  if (state.switchingServer) throw new Error('A server switch is already in progress.');
  publish({ ...state, switchingServer: true });
  try {
    const target = nodeId === 'local' ? { endpoint: local.source } : await nexusRequest<{ endpoint: string }>('egress', { node_id: nodeId });
    const key = nodeId === 'local' ? local.key : suppliedKey ?? peerKeys.get(nodeId) ?? '';
    if (!await api.switchServer(target.endpoint, key)) throw new Error(api.lastConnectionError || 'Could not connect to the selected inference server.');
    if (nodeId !== 'local' && suppliedKey !== undefined) peerKeys.set(nodeId, suppliedKey);
    publish({ ...state, selectedServerId: nodeId });
  } catch (error) {
    await api.switchServer(previousEndpoint, previousKey);
    throw error;
  } finally { publish({ ...state, switchingServer: false }); }
}
