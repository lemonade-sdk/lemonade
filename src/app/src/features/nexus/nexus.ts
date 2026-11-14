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
  connection_type?: 'local' | 'p2p' | 'relay' | 'unknown';
  latency_ms?: number;
  member?: boolean;
  group_role?: 'home' | 'member';
  group_id?: string;
  can_connect?: boolean;
}
export interface NexusJoinMembership { status: 'member' | 'pending' | 'refused'; group_id?: string; error?: string }
export interface NexusMembership {
  node_id: string;
  hostname: string;
  parent_id?: string;
  role: 'home' | 'member';
  permissions: string[];
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
export const NEXUS_SERVICE_PERMISSIONS: { name: string; label: string }[] = [
  { name: 'svc:inference', label: 'Run inference' },
  { name: 'svc:mcp', label: 'Use MCP tools' },
  { name: 'svc:models_manage', label: 'Manage models' },
  { name: 'svc:backends', label: 'Manage backends' },
  { name: 'svc:jobs', label: 'Manage jobs' },
  { name: 'svc:system', label: 'Access system information' },
  { name: 'svc:management', label: 'Manage Nexus' },
  { name: 'svc:internal', label: 'Access internal endpoints' },
];
export function hasNexusPermission(permissions: string[] | undefined, name: string): boolean {
  return Boolean(permissions?.includes(name) || permissions?.includes('admin'));
}
export function nexusTierLabel(device: NexusDevice): string {
  return device.is_inference ? 'Tier 3 · Inference server' : 'Tier 4 · Client';
}
export interface NexusMeshPortUrl { device_name: string; node_id: string; self: boolean; url: string }
export interface NexusMeshPort {
  running: boolean;
  port?: number;
  scheme?: string;
  local_domain?: string;
  urls?: NexusMeshPortUrl[];
  error?: string;
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
  error_retryable?: boolean;
  event_sequence?: number;
  remote_management?: boolean;
  identity_source?: 'system' | 'user' | 'handoff';
  unlock?: 'passkey' | 'device_key';
  needs_user_key?: boolean;
  has_network?: boolean;
  needs_rejoin?: boolean;
  revoked_public_key?: string;
  join_membership?: NexusJoinMembership;
  // A headless server (system identity, no passkey yet) unlocks from another machine only with
  // its setup code, which lemond keeps in `setup_code_file` on that server.
  setup_code_required?: boolean;
  setup_code_file?: string;
  mesh_port?: NexusMeshPort;
}
export function nexusMeshPortUrl(status: NexusStatus | null | undefined, nodeId: string): string | undefined {
  if (!status?.mesh_port?.running) return undefined;
  return status.mesh_port.urls?.find(entry => entry.node_id === nodeId || (entry.self && nodeId === status.node_id))?.url;
}
export type NexusConnection = 'p2p' | 'relay' | 'connecting' | 'offline';
export const NEXUS_CONNECTION_LABELS: Record<NexusConnection, string> = { p2p: 'P2P', relay: 'Relay', connecting: 'Connecting', offline: 'Offline' };
export function nexusDeviceConnection(status: NexusStatus | null | undefined, device: NexusDevice): NexusConnection | undefined {
  if (!status || device.node_id === status.node_id || !status.mesh_up || status.locked) return undefined;
  if (device.is_online === false) return 'offline';
  if (device.is_online !== true) return device.can_connect ? 'connecting' : undefined;
  return device.connection_type === 'p2p' ? 'p2p' : device.connection_type === 'relay' ? 'relay' : 'connecting';
}
export function nexusDeviceTags(status: NexusStatus | null | undefined, device: NexusDevice): string[] {
  return [
    ...(status && device.node_id === status.node_id ? ['This device'] : []),
    ...(device.is_inference === undefined ? [] : [device.is_inference ? 'Server' : 'Client']),
    ...(device.member ? ['Member'] : []),
    ...(device.group_role === 'member' ? ['Joined network'] : []),
    ...(device.is_admin ? ['Admin'] : []),
  ];
}
export function nexusInferenceServers(status: NexusStatus | null | undefined): NexusDevice[] {
  if (!status?.mesh_up || status.locked) return [];
  const seen = new Set<string>();
  return status.members.filter(device => device.node_id !== status.node_id && device.is_inference && device.can_connect !== false && seen.size < seen.add(device.node_id).size);
}
export function nexusServerUnavailable(status: NexusStatus | null | undefined, device: NexusDevice): 'connecting' | 'offline' | undefined {
  const connection = nexusDeviceConnection(status, device);
  return connection === 'p2p' || connection === 'relay' ? undefined : connection ?? 'offline';
}
export type NexusMeshState = 'off' | 'locked' | 'connecting' | 'connected' | 'offline';
export function nexusMeshSummary(status: NexusStatus | null | undefined, error?: string | null): { state: NexusMeshState; label: string; count?: number } {
  if (!status?.enabled) return { state: 'off', label: 'Not set up' };
  if (status.locked) return { state: 'locked', label: 'Locked' };
  if (status.mesh_up) {
    if (error) return { state: 'offline', label: 'Offline' };
    const servers = nexusInferenceServers(status);
    const online = servers.filter(device => { const connection = nexusDeviceConnection(status, device); return connection === 'p2p' || connection === 'relay'; }).length;
    if (online === 1) return { state: 'connected', label: '1 server online' };
    if (online) return { state: 'connected', label: '{count} servers online', count: online };
    if (!servers.length) return { state: 'connected', label: 'No servers yet' };
    return { state: 'connected', label: servers.some(device => nexusDeviceConnection(status, device) === 'connecting') ? 'Connecting…' : 'Servers offline' };
  }
  if (status.status === 'joining' || status.status === 'starting') return { state: 'connecting', label: 'Connecting…' };
  return { state: 'offline', label: 'Offline' };
}
export function nexusDeviceOnline(status: NexusStatus | null | undefined, nodeId: string): boolean | undefined {
  if (!status) return undefined;
  if (!status.mesh_up || status.locked) return nodeId === status.node_id ? false : undefined;
  return status.members.find(device => device.node_id === nodeId)?.is_online;
}
export function nexusConnectionLabel(status: NexusStatus | null | undefined, nodeId: string): 'P2P' | 'Relay' | undefined {
  if (!status || nodeId === status.node_id || nexusDeviceOnline(status, nodeId) !== true) return undefined;
  const member = status.members.find(device => device.node_id === nodeId);
  return member?.connection_type === 'p2p' ? 'P2P' : member?.connection_type === 'relay' ? 'Relay' : undefined;
}
// A device unlocks with its device key or with a passkey, never both. lemond reports
// `unlock: device_key` once the key has unlocked the device; a `user` or `handoff`
// identity source means the key is the only option even before the first unlock. A
// `system` identity (a packaged server) reports `unlock: passkey` until its first
// key/unlock, which lemond accepts as long as no passkey is registered; only a registered
// passkey keeps that identity on the passkey path.
export function usesNexusDeviceKey(status: NexusStatus): boolean {
  if (status.registered && status.unlock === 'passkey') return false;
  if (status.setup_code_required) return true;
  return status.unlock === 'device_key' || status.identity_source === 'user' || status.identity_source === 'handoff'
    || (status.identity_source === 'system' && !status.registered);
}
export class NexusRequestError extends Error {
  constructor(message: string, readonly status: number, readonly retryable: boolean) { super(message); }
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
let networkEpoch = 0;
let networkPaused = false;
export function clearNexusNetwork() {
  networkEpoch += 1;
  networkPaused = true;
  clearTimeout(timer);
  eventStream?.abort();
  lastMemberIds = undefined;
  publish({ status: null, error: null, notice: null });
}
const listeners = new Set<() => void>();
const peerKeys = new Map<string, string>();
let subscribers = 0;
let timer: ReturnType<typeof setTimeout> | undefined;
let pollRunning = false;
let eventStream: AbortController | undefined;
interface Controller { mode: NexusManagementMode; source: string; endpoint: string; key: string }
let knownController: Controller | undefined;
const API_KEY_ERROR = 'The server requires a valid API key. Check it in Settings > Server, then try again.';
let home: { endpoint: string; key: string } | undefined;
export const relayEndpoint = (endpoint: string, nodeId: string) => `${endpoint}/v1/nexus/peers/${encodeURIComponent(nodeId)}`;
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
// lemond names its controller only to a local operator: a loopback client, or a client on
// the server's local network (RFC 1918). Remote status omits controller_url, and the server
// is then managed through the mesh with its own authorization policy.
async function discoverController(endpoint: string, key: string): Promise<Controller | undefined> {
  const loopback = isLoopbackUrl(endpoint);
  const response = await fetch(`${endpoint}/v1/nexus/status`, { headers: key ? { Authorization: `Bearer ${key}` } : {}, signal: AbortSignal.timeout(10000) });
  if (response.status === 401) throw new Error(API_KEY_ERROR);
  if (!response.ok) {
    if (loopback) throw new Error(`Cannot access the local network controller (${response.status}).`);
    return undefined;
  }
  const status = await response.json() as NexusStatus;
  if (!loopback && !status.controller_url) return undefined;
  const control = new URL(status.controller_url || endpoint);
  if (control.protocol !== 'http:' || control.hostname !== '127.0.0.1') throw new Error('The server returned an invalid local network controller.');
  // A controller on the public port is the server itself: keep the origin that already reached
  // it, which stays same-origin for a page lemond serves and inside an SSH or container forward.
  // A local-network client can reach no other port, so a controller elsewhere stays remote.
  const reached = new URL(endpoint);
  const publicPort = reached.protocol === 'http:' && reached.port === control.port;
  if (!loopback && !publicPort) return undefined;
  return { mode: 'local', source: endpoint, endpoint: publicPort ? endpoint : control.origin, key };
}
async function controller(): Promise<Controller> {
  const { default: api } = await import('../../api');
  await api.loadConnectionSettings();
  if (home && !state.switchingServer && !api.baseUrl.startsWith(`${home.endpoint}/v1/nexus/peers/`)) {
    home = undefined;
    if (state.selectedServerId) publish({ ...state, selectedServerId: undefined });
  }
  const base = home?.endpoint ?? api.baseUrl;
  const baseKey = home ? home.key : api.apiKey;
  if (knownController && knownController.source !== base) knownController = undefined;
  if (knownController) knownController.key = baseKey;
  if (!knownController) {
    knownController = await discoverController(base, baseKey) ?? { mode: 'remote', source: base, endpoint: base, key: baseKey };
  }
  return knownController;
}
export async function nexusAccountController(): Promise<{ source: string; local: boolean; request: typeof nexusRequest }> {
  const target = { ...await controller() };
  return { source: target.source, local: target.mode === 'local', request: <T>(path: string, body?: unknown) => nexusRequest<T>(path, body, target) };
}
export async function nexusRequest<T>(path: string, body?: unknown, pinnedController?: Controller): Promise<T> {
  const send = async () => {
    const target = pinnedController || await controller();
    if (state.mode !== target.mode) publish({ ...state, mode: target.mode });
    return { target, response: await fetch(`${target.endpoint}/v1/nexus/${path}`, {
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
    // A lost POST response may already have changed the server; only reads can be retried
    // safely. A read goes out once more: to a rediscovered controller, or to the pinned one
    // of an account flow. The desktop webview has no warm same-origin connection pool, so a
    // read there often opens a fresh connection, and one that the server closes must not
    // end the Enable Mesh dialog by itself.
    const previousController = knownController;
    knownController = undefined;
    if (body !== undefined || (!pinnedController && !previousController)) throw error;
    ({ response } = await send());
  }
  if (body === undefined && response.status === 429) {
    await new Promise(resolve => setTimeout(resolve, 5000));
    ({ response } = await send());
  }
  if (response.status === 401) throw new Error(API_KEY_ERROR);
  const data = await response.json();
  if (!response.ok) throw new NexusRequestError(typeof data?.error === 'string' ? data.error : `Network request failed (${response.status}).`, response.status, data?.retryable === true);
  return data as T;
}
export async function refreshNexus(): Promise<void> {
  if (pollRunning || networkPaused) return;
  const epoch = networkEpoch;
  pollRunning = true;
  try {
    const status = await nexusRequest<NexusStatus>('status');
    if (epoch !== networkEpoch) return;
    let notice = state.notice;
    if (lastMemberIds && state.status?.mesh_up && status.mesh_up) {
      const joined = status.members.filter(member => !lastMemberIds!.has(member.node_id));
      if (joined.length) notice = `${joined.map(member => member.device_name || 'A device').join(', ')} joined your group.`;
    }
    lastMemberIds = status.mesh_up ? new Set(status.members.map(member => member.node_id)) : undefined;
    publish({ ...state, status, error: null, notice });
  } catch (error) {
    if (epoch !== networkEpoch) return;
    publish({ ...state, error: error instanceof Error ? error.message : String(error) });
  } finally { pollRunning = false; }
}
async function streamEvents() {
  const abort = new AbortController();
  eventStream = abort;
  try {
    const local = await controller();
    if (local.mode === 'remote') return;
    const response = await fetch(`${local.endpoint}/v1/nexus/events?after=${state.status?.event_sequence || 0}`, {
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
  if (subscribers && !networkPaused) timer = setTimeout(poll, 5000);
}
const subscribe = (listener: () => void) => { listeners.add(listener); return () => { listeners.delete(listener); }; };
export function useNexusNetwork(active = true): NetworkState {
  useEffect(() => {
    if (!active) return;
    networkPaused = false;
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
  beforeUnlock?: (status: NexusStatus) => void, desktop = isNexusDesktop(), setupCode?: string) {
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
  // Only the desktop app can verify a passkey. A browser unlocks with the device key, and
  // lemond retires a passkey the device registered earlier (`replace_passkey`).
  const retirePasskey = !desktop && !usesNexusDeviceKey(status);
  if (usesNexusDeviceKey(status) || retirePasskey) {
    // An unlocked device without a network still needs the unlock call: it is what
    // creates the network. A join leaves that to the invite code.
    if (!status.locked && (status.mesh_up || joining)) return;
    try { await request('key/unlock', { defer_join: joining, ...(retirePasskey ? { replace_passkey: true } : {}),
      ...(setupCode?.trim() ? { setup_code: setupCode.trim() } : {}) }); }
    catch (error) {
      if (error instanceof NexusRequestError && error.retryable) {
        const reason = error.message.trim();
        throw new Error(`${reason}${/[.!?]$/.test(reason) ? '' : '.'} The network could not be reached. Try again shortly.`);
      }
      throw error;
    }
    return;
  }
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

async function homeServer(): Promise<{ endpoint: string; key: string }> {
  if (home) return home;
  const origin = await controller();
  return { endpoint: origin.source, key: origin.key };
}
export class NexusHostError extends Error {
  constructor(message: string, readonly status: number, readonly code = '') {
    super(message);
    this.name = 'NexusHostError';
  }
}
// Whether a later attempt may answer: nothing answered, the relay found no answer from the
// device (502) or the mesh was busy or not up yet (503, 504). The first request over a fresh
// mesh path, as after a lemond restart, often fails this way while the connection forms.
export function nexusHostErrorRetryable(error: unknown): boolean {
  if (error instanceof NexusHostError) return error.status === 0 || error.status === 502 || error.status === 503 || error.status === 504;
  return false;
}
const PEER_PERMISSION_ERROR = 'That device does not allow this device to use its models. Ask the network administrator for inference access.';
export async function nexusHostRequest<T>(hostId: string, path: string): Promise<T> {
  const server = await homeServer();
  const peerKey = hostId === 'local' ? '' : peerKeys.get(hostId) ?? '';
  const base = hostId === 'local' ? server.endpoint : relayEndpoint(server.endpoint, hostId);
  let response: Response;
  try {
    response = await fetch(`${base}/${path}`, {
      headers: { ...(server.key ? { Authorization: `Bearer ${server.key}` } : {}), ...(peerKey ? { 'X-Lemonade-Peer-Authorization': `Bearer ${peerKey}` } : {}) },
      cache: 'no-store',
      signal: AbortSignal.timeout(35000),
    });
  } catch (error) {
    throw new NexusHostError(error instanceof Error && error.name === 'TimeoutError' ? 'The device did not answer in time.' : 'Could not reach this device\'s server.', 0);
  }
  if (response.status === 401) throw new NexusHostError(hostId === 'local' ? API_KEY_ERROR : 'That server requires an API key. Enter it in Settings > Devices & mesh.', 401);
  const data = await response.json().catch(() => undefined);
  if (!response.ok) {
    const code = typeof data?.code === 'string' ? data.code : '';
    const message = data?.error?.message ?? data?.error;
    throw new NexusHostError(code === 'nexus_permission_denied' ? PEER_PERMISSION_ERROR
      : typeof message === 'string' ? message : `Request failed (${response.status}).`, response.status, code);
  }
  return data as T;
}

export async function selectNexusInferenceServer(nodeId: string, suppliedKey?: string): Promise<void> {
  if (state.switchingServer) throw new Error('A server switch is already in progress.');
  const { default: api } = await import('../../api');
  if (nodeId === 'local' && !home) {
    publish({ ...state, selectedServerId: 'local' });
    return;
  }
  const server = await homeServer();
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

export type NexusMemberRole = 'administrator' | 'moderator' | 'user';
export function setNexusMemberRole(nodeId: string, role: NexusMemberRole): Promise<NexusDevice> {
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
export function acceptNexusInvite(code: string, request = nexusRequest): Promise<{ status?: string; group_id?: string; error?: string }> {
  return request('invites/accept', { code: code.trim() });
}
export function getNexusMemberships(request = nexusRequest): Promise<{ memberships: NexusMembership[]; group_node_id: string }> {
  return request('memberships');
}
export function leaveNexusMembership(groupId: string, request = nexusRequest): Promise<{ left: boolean; node_id: string }> {
  return request(`memberships/${encodeURIComponent(groupId)}/leave`, {});
}
