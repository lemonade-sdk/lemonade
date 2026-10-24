import { useEffect, useSyncExternalStore } from 'react';

export interface NexusDevice {
  node_id: string;
  device_name: string;
  tunnel_ip: string;
  management_pubkey: string;
  permissions: string[];
  permissions_known?: boolean;
  is_admin?: boolean;
  is_owner?: boolean;
  is_inference?: boolean;
  tier?: 3 | 4;
  access_known?: boolean;
  can_remove: boolean;
  is_online?: boolean;
  latency_ms?: number;
}
export interface NexusAccessEntry {
  node_id: string;
  device_name: string;
  is_admin: boolean;
  tier: 3 | 4;
  see: boolean;
  connect: boolean;
  editable: boolean;
}
export interface NexusDeviceAccess {
  node_id: string;
  device_name: string;
  access_known: boolean;
  can_manage: boolean;
  members: NexusAccessEntry[];
}
export const NEXUS_GROUP_PERMISSIONS: { name: string; label: string }[] = [
  { name: 'read', label: 'See the group' },
  { name: 'write', label: 'Write' },
  { name: 'add_child', label: 'Invite devices' },
  { name: 'edit_node', label: 'Edit the group' },
  { name: 'delete_node', label: 'Delete the group' },
  { name: 'connect_private', label: 'Connect privately' },
  { name: 'connect_shared', label: 'Connect to shared services' },
  { name: 'allocate_ip', label: 'Allocate addresses' },
  { name: 'expand_subnet', label: 'Expand the subnet' },
  { name: 'relay_forward', label: 'Forward relay traffic' },
  { name: 'relay_register', label: 'Register relays' },
  { name: 'stun_respond', label: 'Answer STUN requests' },
  { name: 'routing_coordinate', label: 'Coordinate routing' },
];
export function hasNexusPermission(permissions: string[] | undefined, name: string): boolean {
  return Boolean(permissions?.includes(name) || permissions?.includes('admin'));
}
export function nexusTierLabel(device: NexusDevice): string {
  return device.is_inference ? 'Tier 3 · Inference server' : 'Tier 4 · Client';
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
  remote_management?: boolean;
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
export type NexusManagementMode = 'local' | 'remote';
interface NetworkState { mode?: NexusManagementMode; selectedServerId?: string; switchingServer?: boolean; passkey?: PasskeyPrompt; status: NexusStatus | null; error: string | null; notice: string | null }
let state: NetworkState = { status: null, error: null, notice: null };
const listeners = new Set<() => void>();
const peerKeys = new Map<string, string>();
let subscribers = 0;
let timer: ReturnType<typeof setTimeout> | undefined;
let pollRunning = false;
let eventStream: AbortController | undefined;
interface Controller { mode: NexusManagementMode; source: string; endpoint: string; key: string }
let localController: Controller | undefined;
const API_KEY_ERROR = 'The server requires a valid API key. Check it in Settings > Server, then try again.';
// The server this client connected to before it selected a mesh peer. Relayed requests go to
// `${home.endpoint}/api/v1/nexus/peers/<node>`; Nexus management keeps using home itself.
let home: { endpoint: string; key: string } | undefined;
const relayEndpoint = (endpoint: string, nodeId: string) => `${endpoint}/api/v1/nexus/peers/${encodeURIComponent(nodeId)}`;
export function isNexusInferencePeer(device: NexusDevice, selfId?: string): boolean {
  return device.node_id !== selfId && Boolean(device.is_inference) && device.is_online === true;
}
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
export function isLoopbackUrl(value: string): boolean {
  return ['localhost', '127.0.0.1', '[::1]'].includes(new URL(value).hostname);
}
// A nonloopback server is managed through its public listener with the API key. Loopback
// URLs reach the local management listener.
async function controller(): Promise<Controller> {
  const { default: api } = await import('../../api');
  await api.loadConnectionSettings();
  if (home && !state.switchingServer && !api.baseUrl.startsWith(`${home.endpoint}/api/v1/nexus/peers/`)) {
    home = undefined;
    if (state.selectedServerId) publish({ ...state, selectedServerId: undefined });
  }
  const base = home?.endpoint ?? api.baseUrl;
  const baseKey = home ? home.key : api.apiKey;
  if (!isLoopbackUrl(base)) return { mode: 'remote', source: base, endpoint: base, key: baseKey };
  if (localController && localController.source !== base) localController = undefined;
  if (localController) localController.key = baseKey;
  if (!localController) {
    const endpoint = base;
    const key = baseKey;
    const response = await fetch(`${endpoint}/api/v1/nexus/status`, { headers: key ? { Authorization: `Bearer ${key}` } : {}, signal: AbortSignal.timeout(10000) });
    if (response.status === 401) throw new Error(API_KEY_ERROR);
    if (!response.ok) throw new Error(`Cannot access the local network controller (${response.status}).`);
    const status = await response.json() as NexusStatus;
    const control = new URL(status.controller_url || endpoint);
    if (control.protocol !== 'http:' || control.hostname !== '127.0.0.1') throw new Error('The server returned an invalid local network controller.');
    localController = { mode: 'local', source: endpoint, endpoint: control.origin, key };
  }
  return localController;
}
export async function nexusAccountController(): Promise<{ source: string; local: boolean; request: typeof nexusRequest }> {
  const target = { ...await controller() };
  return { source: target.source, local: target.mode === 'local', request: <T>(path: string, body?: unknown) => nexusRequest<T>(path, body, target) };
}
export async function nexusRequest<T>(path: string, body?: unknown, pinnedController?: Controller): Promise<T> {
  const send = async () => {
    const target = pinnedController || await controller();
    if (state.mode !== target.mode) publish({ ...state, mode: target.mode });
    return { target, response: await fetch(`${target.endpoint}/api/v1/nexus/${path}`, {
      method: body === undefined ? 'GET' : 'POST',
      headers: { ...(body === undefined ? {} : { 'Content-Type': 'application/json' }),
        ...(target.key ? { Authorization: `Bearer ${target.key}` } : {}) },
      ...(body === undefined ? {} : { body: JSON.stringify(body) }),
      cache: 'no-store',
      signal: AbortSignal.timeout(35000),
    }) };
  };
  let response: Response;
  try {
    ({ response } = await send());
  } catch (error) {
    // A lost POST response may already have changed the server; only reads can be retried safely.
    const previousController = localController;
    localController = undefined;
    if (pinnedController || !previousController || body !== undefined) throw error;
    ({ response } = await send());
  }
  if (response.status === 401) throw new Error(API_KEY_ERROR);
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
    if (local.mode === 'remote') return;
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
  update('Follow your device’s passkey verification prompt.', false);
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
export async function registerNexusPasskey(deviceName: string, request = nexusRequest): Promise<void> {
  const options = await request<PasskeyChallenge>('passkey/challenge?register=1');
  if (nativePasskeys()) {
    const result = await nativePasskey<Record<string, string>>(options, deviceName, true);
    await request('passkey/register', result);
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
  await request('passkey/register', { credential_id: encodeBase64Url(credential.rawId),
    public_key_x: toHex(decodeBase64Url(publicKey.x)), public_key_y: toHex(decodeBase64Url(publicKey.y)) });
}
export async function unlockNexus(deferJoin = false, request = nexusRequest): Promise<void> {
  const options = await request<PasskeyChallenge>('passkey/challenge');
  if (!options.credential_id) throw new Error('Create a passkey for this device first.');
  if (nativePasskeys()) {
    const assertion = await nativePasskey<Record<string, string>>(options, '', false);
    await request('passkey/unlock', { defer_join: deferJoin, passkey_assertion: assertion });
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
  await request('passkey/unlock', { defer_join: deferJoin, passkey_assertion: {
    credential_id: encodeBase64Url(credential.rawId), authenticator_data: encodeBase64Url(response.authenticatorData),
    client_data_json: encodeBase64Url(response.clientDataJSON), signature: encodeBase64Url(response.signature),
  } });
}
export async function prepareNexus(deviceName: string, joining: boolean, request = nexusRequest,
  beforeUnlock?: (status: NexusStatus) => void) {
  await request('enable', {});
  let status: NexusStatus | undefined;
  for (let attempt = 0; attempt < 20; attempt++) {
    status = await request<NexusStatus>('status');
    if (status.status !== 'starting') break;
    await new Promise(resolve => setTimeout(resolve, 250));
  }
  if (!status || status.status === 'starting') throw new Error('Network is still starting. Try again shortly.');
  beforeUnlock?.(status);
  await request('device', { device_name: deviceName });
  if (!status.registered) await registerNexusPasskey(deviceName, request);
  await unlockNexus(joining, request);
}
export function nexusStatusLabel(status: NexusStatus | null): string {
  if (!status?.enabled) return 'Mesh off';
  if (status.locked) return 'Mesh locked';
  if (status.mesh_up) return 'Mesh connected';
  if (status.status === 'joining' || status.status === 'starting') return 'Mesh connecting';
  return 'Mesh offline';
}

// A peer is reached through this client's own server, which relays the request over the
// mesh. The selection lives only in this client; other clients of the server are unaffected.
export async function selectNexusInferenceServer(nodeId: string, suppliedKey?: string): Promise<void> {
  if (state.switchingServer) throw new Error('A server switch is already in progress.');
  const { default: api } = await import('../../api');
  if (nodeId === 'local' && !home) {
    publish({ ...state, selectedServerId: 'local' });
    return;
  }
  let server: { endpoint: string; key: string };
  if (home) server = home;
  else {
    const origin = await controller();
    server = { endpoint: origin.source, key: origin.key };
  }
  const previous = { endpoint: api.baseUrl, key: api.apiKey, peerKey: api.peerApiKey, home };
  if (state.switchingServer) throw new Error('A server switch is already in progress.');
  publish({ ...state, switchingServer: true });
  try {
    if (nodeId === 'local') {
      home = undefined;
      if (!await api.switchServer(server.endpoint, server.key)) throw new Error(api.lastConnectionError || 'Could not connect to this device’s server.');
    } else {
      home = server;
      const peerKey = suppliedKey ?? peerKeys.get(nodeId) ?? '';
      if (!await api.switchServer(relayEndpoint(server.endpoint, nodeId), server.key, peerKey)) {
        throw new Error(api.lastConnectionError || 'Could not connect to the selected inference server.');
      }
      if (suppliedKey !== undefined) peerKeys.set(nodeId, suppliedKey);
    }
    publish({ ...state, selectedServerId: nodeId });
  } catch (error) {
    home = previous.home;
    await api.switchServer(previous.endpoint, previous.key, previous.peerKey);
    throw error;
  } finally { publish({ ...state, switchingServer: false }); }
}

export function setNexusMemberRole(nodeId: string, role: 'admin' | 'member'): Promise<NexusDevice> {
  return nexusRequest(`members/${encodeURIComponent(nodeId)}/role`, { role });
}
export function setNexusMemberPermissions(nodeId: string, permissions: string[]): Promise<NexusDevice> {
  return nexusRequest(`members/${encodeURIComponent(nodeId)}/permissions`, { permissions });
}
export function getNexusDeviceAccess(nodeId: string): Promise<NexusDeviceAccess> {
  return nexusRequest(`devices/${encodeURIComponent(nodeId)}/access`);
}
export function setNexusDeviceAccess(nodeId: string, memberNodeId: string, see: boolean, connect: boolean): Promise<NexusDeviceAccess> {
  return nexusRequest(`devices/${encodeURIComponent(nodeId)}/access`, { member_node_id: memberNodeId, see, connect });
}
