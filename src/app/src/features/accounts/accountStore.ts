export type AccountRole = 'guest' | 'user' | 'admin';

export interface AccountRecord {
  id: string;
  name: string;
  role: Exclude<AccountRole, 'guest'>;
  passwordSalt: string;
  passwordHash: string;
  iterations: number;
  createdAt: number;
  updatedAt: number;
}

export interface AccountSession {
  id: string;
  name: string;
  role: AccountRole;
  storageScope: string;
  isGuest: boolean;
}

const ACCOUNTS_KEY = 'lemonade_accounts_v1';
const SESSION_KEY = 'lemonade_account_session_v1';
export const ACCOUNT_SESSION_CHANGED_EVENT = 'lemonade:account-session-changed';
const STORAGE_PREFIX = 'lemonade:';
const GUEST_SCOPE = 'guest:shared';
const LEGACY_GLOBAL_KEYS = [
  'lemonade_conversations',
  'lemonade_active_conversation',
  'lemonade_persist_conversations',
  'lemonade_user_presets',
  'lemonade_applied_presets',
  'lemonade_use_tools',
  'lemonade_custom_models',
];
const HASH_ITERATIONS = 120_000;

const encoder = new TextEncoder();

function isBrowserStorageAvailable(store: Storage | undefined): store is Storage {
  return typeof store !== 'undefined';
}

function generateId(): string {
  if (typeof crypto !== 'undefined' && 'randomUUID' in crypto) return crypto.randomUUID();
  return `${Date.now().toString(36)}${Math.random().toString(36).slice(2, 10)}`;
}

function bytesToBase64(bytes: Uint8Array): string {
  let binary = '';
  bytes.forEach(b => { binary += String.fromCharCode(b); });
  return btoa(binary);
}

function base64ToBytes(base64: string): Uint8Array {
  const binary = atob(base64);
  const out = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) out[i] = binary.charCodeAt(i);
  return out;
}

function timingSafeEqual(a: string, b: string): boolean {
  if (a.length !== b.length) return false;
  let mismatch = 0;
  for (let i = 0; i < a.length; i++) mismatch |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return mismatch === 0;
}

async function derivePasswordHash(password: string, saltBase64: string, iterations = HASH_ITERATIONS): Promise<string> {
  if (typeof crypto === 'undefined' || !crypto.subtle) {
    return bytesToBase64(encoder.encode(`${saltBase64}:${password}`));
  }
  const key = await crypto.subtle.importKey('raw', encoder.encode(password), 'PBKDF2', false, ['deriveBits']);
  const saltBytes = base64ToBytes(saltBase64);
  const salt = saltBytes.buffer.slice(saltBytes.byteOffset, saltBytes.byteOffset + saltBytes.byteLength) as ArrayBuffer;
  const bits = await crypto.subtle.deriveBits(
    { name: 'PBKDF2', hash: 'SHA-256', salt, iterations },
    key,
    256,
  );
  return bytesToBase64(new Uint8Array(bits));
}

function readAccounts(): AccountRecord[] {
  try {
    const raw = localStorage.getItem(ACCOUNTS_KEY);
    if (!raw) return [];
    const parsed = JSON.parse(raw);
    return Array.isArray(parsed?.accounts) ? parsed.accounts.filter(isAccountRecord) : [];
  } catch { return []; }
}

function writeAccounts(accounts: AccountRecord[]): void {
  localStorage.setItem(ACCOUNTS_KEY, JSON.stringify({ version: 1, accounts }));
}

function isAccountRecord(value: unknown): value is AccountRecord {
  if (!value || typeof value !== 'object') return false;
  const obj = value as Record<string, unknown>;
  return typeof obj.id === 'string'
    && typeof obj.name === 'string'
    && (obj.role === 'user' || obj.role === 'admin')
    && typeof obj.passwordSalt === 'string'
    && typeof obj.passwordHash === 'string';
}

function normalizeName(name: string): string {
  return name.trim().replace(/\s+/g, ' ');
}

function accountToSession(account: AccountRecord): AccountSession {
  return { id: account.id, name: account.name, role: account.role, storageScope: `user:${account.id}`, isGuest: false };
}

export function guestSession(): AccountSession {
  return { id: 'guest', name: 'Guest', role: 'guest', storageScope: GUEST_SCOPE, isGuest: true };
}

export function listAccounts(): AccountRecord[] {
  return readAccounts().sort((a, b) => a.name.localeCompare(b.name));
}

export function accountCount(): number { return readAccounts().length; }

export function currentSession(): AccountSession {
  try {
    const raw = localStorage.getItem(SESSION_KEY) || sessionStorage.getItem(SESSION_KEY);
    if (!raw) return guestSession();
    const parsed = JSON.parse(raw) as { id?: string };
    if (!parsed.id) return guestSession();
    const account = readAccounts().find(a => a.id === parsed.id);
    return account ? accountToSession(account) : guestSession();
  } catch { return guestSession(); }
}

function notifySessionChanged(session: AccountSession): void {
  try {
    window.dispatchEvent(new CustomEvent(ACCOUNT_SESSION_CHANGED_EVENT, { detail: session }));
  } catch { /* ignore non-browser callers */ }
}

function saveSession(session: AccountSession): void {
  try {
    if (session.isGuest) localStorage.removeItem(SESSION_KEY);
    else localStorage.setItem(SESSION_KEY, JSON.stringify({ id: session.id }));
    sessionStorage.removeItem(SESSION_KEY);
  } catch { /* ignore */ }
  notifySessionChanged(session);
}

export function subscribeAccountSessionChanges(listener: (session: AccountSession) => void): () => void {
  const notifyCurrent = () => listener(currentSession());
  const onCustom = (event: Event) => {
    const next = (event as CustomEvent<AccountSession>).detail || currentSession();
    listener(next);
  };
  const onStorage = (event: StorageEvent) => {
    if (event.key === SESSION_KEY || event.key === ACCOUNTS_KEY) notifyCurrent();
  };
  window.addEventListener(ACCOUNT_SESSION_CHANGED_EVENT, onCustom as EventListener);
  window.addEventListener('storage', onStorage);
  return () => {
    window.removeEventListener(ACCOUNT_SESSION_CHANGED_EVENT, onCustom as EventListener);
    window.removeEventListener('storage', onStorage);
  };
}

export async function createAccount(name: string, password: string): Promise<AccountSession> {
  const cleanName = normalizeName(name);
  if (cleanName.length < 2) throw new Error('Name must contain at least 2 characters.');
  if (password.length < 8) throw new Error('Password must contain at least 8 characters.');
  const accounts = readAccounts();
  if (accounts.some(a => a.name.toLowerCase() === cleanName.toLowerCase())) throw new Error('An account with this name already exists.');
  if (typeof crypto === 'undefined' || !crypto.getRandomValues) throw new Error('This browser does not support secure local accounts.');
  const salt = new Uint8Array(16);
  crypto.getRandomValues(salt);
  const saltBase64 = bytesToBase64(salt);
  const now = Date.now();
  const account: AccountRecord = {
    id: generateId(),
    name: cleanName,
    role: accounts.length === 0 ? 'admin' : 'user',
    passwordSalt: saltBase64,
    passwordHash: await derivePasswordHash(password, saltBase64),
    iterations: HASH_ITERATIONS,
    createdAt: now,
    updatedAt: now,
  };
  writeAccounts([...accounts, account]);
  const session = accountToSession(account);
  saveSession(session);
  return session;
}

export async function signIn(name: string, password: string): Promise<AccountSession> {
  const cleanName = normalizeName(name);
  const account = readAccounts().find(a => a.name.toLowerCase() === cleanName.toLowerCase());
  if (!account) throw new Error('Account not found.');
  const candidate = await derivePasswordHash(password, account.passwordSalt, account.iterations || HASH_ITERATIONS);
  if (!timingSafeEqual(candidate, account.passwordHash)) throw new Error('Password is incorrect.');
  const session = accountToSession(account);
  saveSession(session);
  return session;
}

export function signOut(): AccountSession {
  const guest = guestSession();
  saveSession(guest);
  return guest;
}

export function scopedStorageKey(scope: string, key: string): string {
  return `${STORAGE_PREFIX}${scope}:${key}`;
}

export function clearScopedData(scope: string): void {
  for (const store of [localStorage, sessionStorage]) {
    if (!isBrowserStorageAvailable(store)) continue;
    Object.keys(store).filter(k => k.startsWith(`${STORAGE_PREFIX}${scope}:`)).forEach(k => store.removeItem(k));
  }
}

export function deleteOwnAccount(session: AccountSession): AccountSession {
  if (session.isGuest) throw new Error('Guest mode has no private account to delete.');
  writeAccounts(readAccounts().filter(a => a.id !== session.id));
  clearScopedData(session.storageScope);
  return signOut();
}

export function clearAllAccountsAndScopedData(): AccountSession {
  for (const store of [localStorage, sessionStorage]) {
    if (!isBrowserStorageAvailable(store)) continue;
    Object.keys(store)
      .filter(k => k === ACCOUNTS_KEY || k === SESSION_KEY || k.startsWith(STORAGE_PREFIX) || LEGACY_GLOBAL_KEYS.includes(k))
      .forEach(k => store.removeItem(k));
  }
  const guest = guestSession();
  notifySessionChanged(guest);
  return guest;
}

export function clearCurrentSessionData(session: AccountSession): void {
  clearScopedData(session.storageScope);
  if (session.isGuest) {
    LEGACY_GLOBAL_KEYS.forEach(k => { try { localStorage.removeItem(k); sessionStorage.removeItem(k); } catch { /* ignore */ } });
  }
}

export function describeSession(session: AccountSession): string {
  if (session.isGuest) return 'Guest shared space';
  return `${session.name}${session.role === 'admin' ? ' · Admin' : ''}`;
}
