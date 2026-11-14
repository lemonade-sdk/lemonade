import type { NexusAccount, NexusDevice } from './NexusAccountContext';
import {
  acceptNexusInvite, getNexusMemberships, isNexusDesktop, leaveNexusMembership, nexusAccountController, nexusRequestAt, prepareNexus, usesNexusDeviceKey,
  type NexusDevice as NetworkDevice, type NexusInvite, type NexusJoinMembership, type NexusMembership, type NexusStatus,
} from './nexus';

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
export interface NexusAccountInspection { status: NexusStatus; source: string; local: boolean; desktop: boolean; ready: boolean }
export interface NexusSignInInput {
  action: 'create' | 'join' | 'return';
  deviceName: string;
  source: string;
  invitation?: string;
}
// What inviting a Lemonade server found on this network did (#1221). `joined` false keeps the
// code, so it can still be entered on that device.
export interface FoundServerInvite { joined: boolean; code: string; expiresAt: number; membership?: NexusJoinMembership; error?: string }
export interface NexusAccountService {
  disconnect: (session: VerifiedNexusAccount) => Promise<void>;
  inspect: () => Promise<NexusAccountInspection>;
  resume: () => Promise<VerifiedNexusAccount | undefined>;
  signIn: (input: NexusSignInInput) => Promise<VerifiedNexusAccount>;
  refresh: (session: VerifiedNexusAccount) => Promise<VerifiedNexusAccount>;
  invite: (session: VerifiedNexusAccount, deviceName: string, role?: 'user' | 'moderator') => Promise<NexusInvite>;
  // Invites a server found on this network and joins it from here: the invite is made on this
  // device, then the other lemond is set up and joins with the code, as Join by invitation would.
  inviteFoundServer: (session: VerifiedNexusAccount, endpoint: string, deviceName: string) => Promise<FoundServerInvite>;
  joinNetwork: (session: VerifiedNexusAccount, invitation: string) => Promise<NexusJoinMembership>;
  memberships: (session: VerifiedNexusAccount) => Promise<NexusMembership[]>;
  leaveNetwork: (session: VerifiedNexusAccount, groupId: string) => Promise<void>;
  // Deletes the account on the network. `pending` means Tier 1 has not committed it yet
  // and the device stays signed in; `deleted` means lemond removed the local account too.
  deleteAccount: (session: VerifiedNexusAccount) => Promise<'deleted' | 'pending'>;
  setAvatar: (session: VerifiedNexusAccount, image: string | null) => Promise<NexusAvatarState & { state: 'ready' }>;
}
export function nexusJoinNotice(membership: NexusJoinMembership | undefined): { message: string; error: boolean } | undefined {
  if (!membership) return undefined;
  if (membership.status === 'refused') {
    return { error: true, message: membership.error?.trim() ? `The invite was not accepted (${membership.error.trim()}). This device stays in its own network.` : 'The invite was not accepted. This device stays in its own network.' };
  }
  if (membership.status === 'pending') return { error: false, message: 'The invite is waiting for the network to confirm it. This device stays in its own network and joins the invited one as a member once confirmed.' };
  return { error: false, message: 'This device joined the invited network as a member and keeps its own network.' };
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

interface AccountReply { id: string; member_id: string; device_node_id: string; role: NonNullable<NexusAccount['role']>; devices: NetworkDevice[]; avatar?: unknown }
// lemond reports no account name. `deviceName` is this device's name from the membership
// reply; `fallbackName` covers a reply without one (the name the user entered). `expectedId`
// is the account of a running session: lemond replaces the device key, and with it the
// account, when the network revokes it, so a changed id ends that session.
export function verifyAccountReply(value: unknown, source: string, expectedId?: string, fallbackName = ''): VerifiedNexusAccount {
  const reply = value as AccountReply | null;
  if (!reply || typeof reply.id !== 'string' || !reply.id || typeof reply.member_id !== 'string' || !reply.member_id
    || typeof reply.device_node_id !== 'string' || !reply.device_node_id || !['admin', 'moderator', 'user'].includes(reply.role) || !Array.isArray(reply.devices)) {
    throw new Error('The server did not return a verified account. Update the server and try again.');
  }
  if (expectedId && reply.id !== expectedId) throw new Error('The account on this device changed. Enable Mesh again.');
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
  const deviceName = [self.device_name, fallbackName].map(name => (typeof name === 'string' ? name : '').trim()).find(boundedName) || 'Lemonade device';
  return { account: { id: reply.id, deviceName, role: reply.role, avatarUrl: avatar.state === 'ready' ? avatar.image ?? undefined : undefined },
    devices, memberId: reply.member_id, deviceNodeId: reply.device_node_id, source, avatar,
    canInvite: Boolean(self.permissions?.includes('admin') || self.permissions?.includes('add_child')) };
}

export const validNexusInvitation = (value: string) => /^(?:[0-9]{8}|lnk_[0-9a-f]{64})$/.test(value.trim());
const boundedName = (name: string) => Boolean(name.trim()) && new TextEncoder().encode(name.trim()).length <= 128;

export function createNexusAccountService(dependencies: {
  controller: typeof nexusAccountController; desktop: typeof isNexusDesktop; prepare: typeof prepareNexus; requestAt?: typeof nexusRequestAt;
} = { controller: nexusAccountController, desktop: isNexusDesktop, prepare: prepareNexus, requestAt: nexusRequestAt }): NexusAccountService {
  const currentController = async (source: string) => {
    const controller = await dependencies.controller();
    if (controller.source !== source) throw new Error('The server connection changed. Close this dialog and enable Mesh again.');
    return controller;
  };
  const requireController = async (source: string) => {
    const controller = await currentController(source);
    if (!controller.local) throw new Error('Set up this server through its local controller or SSH forwarding. After setup, manage it through the authenticated Nexus mesh.');
    const status = await controller.request<NexusStatus>('status');
    return { ...controller, status };
  };
  const service: NexusAccountService = {
    async disconnect(session) {
      const controller = await requireController(session.source);
      const status = await controller.request<{ locked: boolean; mesh_up: boolean }>('lock', {});
      if (!status.locked || status.mesh_up) throw new Error('The device did not disconnect. Try again.');
    },
    async inspect() {
      const controller = await dependencies.controller();
      const status = await controller.request<NexusStatus>('status');
      return { status, source: controller.source, local: controller.local, desktop: dependencies.desktop(), ready: controller.local };
    },
    async resume() {
      const controller = await dependencies.controller();
      if (!controller.local) return undefined;
      const status = await controller.request<NexusStatus>('status');
      // A passkey device verifies its user when Mesh is enabled; a device key has nothing
      // to verify, so a server that is set up, unlocked and on its mesh is simply picked up.
      if (!status.available || !status.enabled || status.locked || !status.mesh_up || !usesNexusDeviceKey(status)) return undefined;
      const session = verifyAccountReply(await controller.request('account'), controller.source, undefined, status.device_name || '');
      await currentController(controller.source);
      return session;
    },
    async signIn(input) {
      if (!boundedName(input.deviceName)) throw new Error('Enter a device name of up to 128 bytes.');
      if (input.action === 'join' && !validNexusInvitation(input.invitation || '')) throw new Error('Enter an 8-digit invitation code or a full invitation token.');
      const controller = await requireController(input.source);
      const { status } = controller;
      if (!status.available) throw new Error('This server does not include Nexus account support.');
      let prepared: NexusStatus | undefined;
      await dependencies.prepare(input.deviceName.trim(), input.action === 'join', controller.request, current => {
        prepared = current;
        if (input.action === 'create' && current.registered && current.has_network !== false) throw new Error('This device already has a registered key. Choose Use this device’s key to continue.');
        if (input.action === 'return' && !current.registered) throw new Error('Create an account or join by invitation before enabling Mesh on this device.');
      }, dependencies.desktop());
      if (input.action === 'join') {
        const code = input.invitation!.trim();
        if (prepared?.mesh_up) await acceptNexusInvite(code, controller.request);
        else await controller.request('join', { code });
      }
      const session = verifyAccountReply(await controller.request('account'), controller.source, undefined, input.deviceName);
      await currentController(input.source);
      return session;
    },
    async joinNetwork(session, invitation) {
      if (!validNexusInvitation(invitation)) throw new Error('Enter an 8-digit invitation code or a full invitation token.');
      const controller = await requireController(session.source);
      const reply = await acceptNexusInvite(invitation, controller.request);
      const status = typeof reply?.status === 'string' ? reply.status : 'member';
      if (!['member', 'pending', 'refused'].includes(status)) throw new Error('The server returned an invalid membership. Refresh and check your networks.');
      return { status: status as NexusJoinMembership['status'], group_id: typeof reply.group_id === 'string' ? reply.group_id : undefined, ...(typeof reply.error === 'string' ? { error: reply.error } : {}) };
    },
    async memberships(session) {
      const controller = await requireController(session.source);
      const reply = await getNexusMemberships(controller.request);
      if (!reply || !Array.isArray(reply.memberships)) throw new Error('The server returned invalid memberships.');
      return reply.memberships.filter(group => group && typeof group.node_id === 'string' && (group.role === 'home' || group.role === 'member'))
        .map(group => ({ ...group, hostname: typeof group.hostname === 'string' ? group.hostname : '', permissions: Array.isArray(group.permissions) ? group.permissions : [] }));
    },
    async leaveNetwork(session, groupId) {
      const controller = await requireController(session.source);
      const reply = await leaveNexusMembership(groupId, controller.request);
      if (!reply?.left) throw new Error('The network did not confirm that this device left. Refresh and try again.');
    },
    async deleteAccount(session) {
      const controller = await requireController(session.source);
      const reply = await controller.request<{ status?: string }>('account/delete', {
        account_id: session.account.id, member_id: session.memberId, device_node_id: session.deviceNodeId, confirm: 'delete',
      });
      if (reply?.status !== 'deleted' && reply?.status !== 'pending') throw new Error('The network did not confirm the deletion. Refresh and try again.');
      return reply.status;
    },
    async refresh(session) {
      const controller = await requireController(session.source);
      const next = verifyAccountReply(await controller.request('account'), session.source, session.account.id, session.account.deviceName);
      if (next.memberId !== session.memberId || next.deviceNodeId !== session.deviceNodeId) throw new Error('The account membership changed. Enable Mesh again.');
      if (next.avatar.state === 'unavailable') next.account.avatarUrl = session.account.avatarUrl;
      return next;
    },
    async invite(session, deviceName, role) {
      if (!boundedName(deviceName)) throw new Error('Enter the name of the device you are inviting.');
      const controller = await requireController(session.source);
      const current = verifyAccountReply(await controller.request('account'), session.source, session.account.id, session.account.deviceName);
      if (current.memberId !== session.memberId || current.deviceNodeId !== session.deviceNodeId) throw new Error('The account membership changed. Enable Mesh again.');
      const invite = await controller.request<NexusInvite>('invites', { device_name: deviceName.trim(), ttl_sec: 600, ...(role ? { role } : {}) });
      if (!invite || !validNexusInvitation(invite.code || invite.link_token || '') || !Number.isFinite(invite.expires_at)) {
        throw new Error('The server returned an invalid invitation. Try again.');
      }
      return invite;
    },
    async inviteFoundServer(session, endpoint, deviceName) {
      const invite = await service.invite(session, deviceName);
      const code = (invite.code || invite.link_token).trim();
      const request = (dependencies.requestAt ?? nexusRequestAt)(endpoint);
      try {
        let status: NexusStatus;
        try { status = await request<NexusStatus>('status'); }
        catch { throw new Error(`${deviceName} runs a Lemonade without network support. Update it, or enter the code there.`); }
        if (!status.available) throw new Error(`${deviceName} runs a Lemonade without network support. Update it, or enter the code there.`);
        if (status.registered && !usesNexusDeviceKey(status)) throw new Error(`${deviceName} unlocks with a passkey, so the code has to be entered on that device.`);
        let prepared: NexusStatus | undefined;
        await dependencies.prepare(deviceName.trim(), true, request, current => { prepared = current; });
        if (prepared?.mesh_up) {
          const reply = await acceptNexusInvite(code, request);
          const status = reply?.status === 'pending' || reply?.status === 'refused' ? reply.status : 'member';
          const membership: NexusJoinMembership = { status, ...(reply?.group_id ? { group_id: reply.group_id } : {}), ...(reply?.error ? { error: reply.error } : {}) };
          if (status === 'refused') return { joined: false, code, expiresAt: invite.expires_at, membership, error: reply?.error || 'The invite was not accepted.' };
          return { joined: true, code, expiresAt: invite.expires_at, membership };
        }
        await request('join', { code });
        return { joined: true, code, expiresAt: invite.expires_at };
      } catch (error) {
        return { joined: false, code, expiresAt: invite.expires_at, error: error instanceof Error ? error.message : String(error) };
      }
    },
    async setAvatar(session, image) {
      if (session.avatar.state !== 'ready' || !session.avatar.can_write) throw new Error('Profile images cannot be changed on this account right now.');
      const controller = await requireController(session.source);
      const next = verifyAvatarReply(await controller.request('account/avatar', {
        account_id: session.account.id, member_id: session.memberId, device_node_id: session.deviceNodeId,
        revision: session.avatar.revision, image,
      }));
      await currentController(session.source);
      if (next.state !== 'ready' || next.revision <= session.avatar.revision) throw new Error('The mesh did not confirm the profile image change. Try again.');
      return next;
    },
  };
  return service;
}

export const nexusAccountService = createNexusAccountService();
