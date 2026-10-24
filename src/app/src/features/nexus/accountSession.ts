import type { NexusAccount, NexusDevice } from './NexusAccountContext';
import {
  isNexusDesktop, nexusAccountController, prepareNexus,
  type NexusDevice as NetworkDevice, type NexusInvite, type NexusStatus,
} from './nexus';

export interface SavedNexusAccount { id: string; displayName: string }
export type NexusAvatarState = { state: 'ready'; image: string | null; revision: number; can_write: boolean; pending?: boolean }
  | { state: 'unsupported' | 'unavailable' };
export interface VerifiedNexusAccount {
  account: NexusAccount;
  devices: readonly NexusDevice[];
  memberId: string;
  deviceNodeId: string;
  source: string;
  canInvite: boolean;
  avatar: NexusAvatarState;
}
export interface NexusAccountInspection { status: NexusStatus; source: string; local: boolean; desktop: boolean }
export interface NexusSignInInput {
  action: 'create' | 'join' | 'return';
  displayName: string;
  deviceName: string;
  source: string;
  invitation?: string;
  expectedId?: string;
}
export interface NexusAccountService {
  inspect: () => Promise<NexusAccountInspection>;
  signIn: (input: NexusSignInInput) => Promise<VerifiedNexusAccount>;
  refresh: (session: VerifiedNexusAccount) => Promise<VerifiedNexusAccount>;
  invite: (session: VerifiedNexusAccount, deviceName: string) => Promise<NexusInvite>;
  setAvatar: (session: VerifiedNexusAccount, image: string | null) => Promise<NexusAvatarState & { state: 'ready' }>;
}

export function verifyAvatarReply(value: unknown): NexusAvatarState {
  const reply = value as Partial<NexusAvatarState & { image: string | null; revision: number; can_write: boolean; pending?: boolean }> | null;
  if (!reply || reply.state === 'unsupported') return { state: 'unsupported' };
  if (reply.state === 'unavailable') return { state: 'unavailable' };
  if (reply.state !== 'ready' || !Number.isSafeInteger(reply.revision) || reply.revision! < 0 || typeof reply.can_write !== 'boolean'
    || (reply.pending !== undefined && (typeof reply.pending !== 'boolean' || (reply.pending && reply.image !== null)))
    || (reply.image !== null && (typeof reply.image !== 'string' || reply.image.length > 128 * 1024
      || !/^data:image\/png;base64,[A-Za-z0-9+/]+={0,2}$/.test(reply.image)))) {
    throw new Error('The mesh returned invalid profile image data.');
  }
  return reply as NexusAvatarState & { state: 'ready' };
}

interface AccountReply { id: string; member_id: string; device_node_id: string; role: 'admin' | 'user'; devices: NetworkDevice[]; avatar?: unknown }
export function verifyAccountReply(value: unknown, displayName: string, source: string, expectedId?: string): VerifiedNexusAccount {
  const reply = value as AccountReply | null;
  if (!reply || typeof reply.id !== 'string' || !reply.id || typeof reply.member_id !== 'string' || !reply.member_id
    || typeof reply.device_node_id !== 'string' || !reply.device_node_id || !['admin', 'user'].includes(reply.role) || !Array.isArray(reply.devices)) {
    throw new Error('The server did not return a verified account. Update the server and try again.');
  }
  if (expectedId && reply.id !== expectedId) throw new Error('This device belongs to a different account. The saved account has been kept.');
  const self = reply.devices.find(device => device?.node_id === reply.device_node_id);
  if (!self || self.permissions_known !== true || (reply.role === 'admin') !== (self.is_admin === true)) {
    throw new Error('Account permissions could not be verified. Try again.');
  }
  const devices = reply.devices.filter(device => device && typeof device.node_id === 'string' && typeof device.device_name === 'string').map(device => ({
    id: device.node_id, name: device.device_name, kind: 'computer' as const,
    current: device.node_id === reply.device_node_id, connected: device.is_online === true,
  }));
  let avatar: NexusAvatarState;
  try { avatar = verifyAvatarReply(reply.avatar); } catch { avatar = { state: 'unavailable' }; }
  return { account: { id: reply.id, displayName, role: reply.role, avatarUrl: avatar.state === 'ready' ? avatar.image ?? undefined : undefined },
    devices, memberId: reply.member_id, deviceNodeId: reply.device_node_id, source, avatar,
    canInvite: Boolean(self.permissions?.includes('admin') || self.permissions?.includes('add_child')) };
}

export const validNexusInvitation = (value: string) => /^(?:[0-9]{8}|lnk_[0-9a-f]{64})$/.test(value.trim());
const boundedName = (name: string) => Boolean(name.trim()) && new TextEncoder().encode(name.trim()).length <= 128;

export function createNexusAccountService(dependencies = { controller: nexusAccountController, desktop: isNexusDesktop, prepare: prepareNexus }): NexusAccountService {
  const requireController = async (source: string) => {
    const controller = await dependencies.controller();
    if (controller.source !== source) throw new Error('The server connection changed. Close this dialog and sign in again.');
    if (!controller.local || !dependencies.desktop()) throw new Error('Open the Lemonade desktop app on this computer to verify your passkey.');
    return controller;
  };
  return {
    async inspect() {
      const controller = await dependencies.controller();
      return { status: await controller.request<NexusStatus>('status'), source: controller.source, local: controller.local, desktop: dependencies.desktop() };
    },
    async signIn(input) {
      if (!boundedName(input.displayName) || !boundedName(input.deviceName)) throw new Error('Enter an account name and device name of up to 128 bytes each.');
      if (input.action === 'join' && !validNexusInvitation(input.invitation || '')) throw new Error('Enter an 8-digit invitation code or a full invitation token.');
      const controller = await requireController(input.source);
      const status = await controller.request<NexusStatus>('status');
      if (!status.available) throw new Error('This server does not include Nexus account support.');
      await dependencies.prepare(input.deviceName.trim(), input.action === 'join', controller.request, prepared => {
        if (input.action === 'create' && prepared.registered) throw new Error('This device already has a registered key. Choose Sign in to continue with it.');
        if (input.action === 'return' && !prepared.registered) throw new Error('Create an account or join by invitation before signing in on this device.');
        if (input.action === 'join' && (prepared.mesh_up || prepared.group_node_id)) throw new Error('This device is already linked to an account. Use Sign in to continue.');
      });
      if (input.action === 'join') await controller.request('join', { code: input.invitation!.trim() });
      const session = verifyAccountReply(await controller.request('account'), input.displayName.trim(), controller.source, input.expectedId);
      await requireController(input.source);
      return session;
    },
    async refresh(session) {
      const controller = await requireController(session.source);
      const next = verifyAccountReply(await controller.request('account'), session.account.displayName, session.source, session.account.id);
      if (next.memberId !== session.memberId || next.deviceNodeId !== session.deviceNodeId) throw new Error('The account membership changed. Sign in again.');
      if (next.avatar.state === 'unavailable') next.account.avatarUrl = session.account.avatarUrl;
      return next;
    },
    async invite(session, deviceName) {
      if (!boundedName(deviceName)) throw new Error('Enter the name of the device you are inviting.');
      const controller = await requireController(session.source);
      const current = verifyAccountReply(await controller.request('account'), session.account.displayName, session.source, session.account.id);
      if (current.memberId !== session.memberId || current.deviceNodeId !== session.deviceNodeId) throw new Error('The account membership changed. Sign in again.');
      const invite = await controller.request<NexusInvite>('invites', { device_name: deviceName.trim(), ttl_sec: 600 });
      if (!invite || !validNexusInvitation(invite.code || invite.link_token || '') || !Number.isFinite(invite.expires_at)) {
        throw new Error('The server returned an invalid invitation. Try again.');
      }
      return invite;
    },
    async setAvatar(session, image) {
      if (session.avatar.state !== 'ready' || !session.avatar.can_write) throw new Error('Profile images cannot be changed on this account right now.');
      const controller = await requireController(session.source);
      const next = verifyAvatarReply(await controller.request('account/avatar', {
        account_id: session.account.id, member_id: session.memberId, device_node_id: session.deviceNodeId,
        revision: session.avatar.revision, image,
      }));
      await requireController(session.source);
      if (next.state !== 'ready' || next.revision <= session.avatar.revision) throw new Error('The mesh did not confirm the profile image change. Try again.');
      return next;
    },
  };
}

export const nexusAccountService = createNexusAccountService();

const PROFILE_KEY = 'lemonade_nexus_account_labels';
export function loadNexusAccountLabel(source: string): SavedNexusAccount | undefined {
  try {
    const entry = JSON.parse(localStorage.getItem(PROFILE_KEY) || '{}')[source] as SavedNexusAccount;
    return entry && typeof entry.id === 'string' && boundedName(entry.displayName) ? { id: entry.id, displayName: entry.displayName } : undefined;
  } catch { return undefined; }
}
export function saveNexusAccountLabel(session: VerifiedNexusAccount) {
  try {
    const current = JSON.parse(localStorage.getItem(PROFILE_KEY) || '{}');
    localStorage.setItem(PROFILE_KEY, JSON.stringify({ ...current, [session.source]: { id: session.account.id, displayName: session.account.displayName } }));
  } catch { /* Account access does not depend on retaining a display label. */ }
}
