import type { NexusAccount, NexusDevice } from './NexusAccountContext';
import {
  acceptNexusInvite, getNexusMemberships, hasNexusPermission, isNexusDesktop, isNexusGrantList, isNexusGroupRole, isNexusMembershipState, leaveNexusMembership,
  nexusAccountController, NexusRequestError, normalizeNexusInvitation, prepareNexus, usesNexusDeviceKey,
  NEXUS_INVITE_CODE, NEXUS_INVITE_TOKEN, NEXUS_INVITE_TTL_SEC, nexusNameTooLong,
  type NexusDevice as NetworkDevice, type NexusInvite, type NexusJoinMembership, type NexusMemberRole, type NexusMembership, type NexusStatus,
} from './nexus';
import { isNexusRole } from './routePermissions';

export const NEXUS_AVATAR_MAX_BYTES = 128 * 1024;
export const NEXUS_JOINED_AS_MEMBER = 'This device joined the invited network as a member and keeps its own network.';
const INVITATION_FORMAT_ERROR = 'Enter an 8-digit invitation code or a full invitation token.';
const MEMBERSHIP_CHANGED_ERROR = 'The account membership changed. Enable Mesh again.';

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
export interface NexusAccountService {
  disconnect: (session: VerifiedNexusAccount) => Promise<void>;
  inspect: () => Promise<NexusAccountInspection>;
  resume: () => Promise<VerifiedNexusAccount | undefined>;
  signIn: (input: NexusSignInInput) => Promise<VerifiedNexusAccount>;
  refresh: (session: VerifiedNexusAccount) => Promise<VerifiedNexusAccount>;
  invite: (session: VerifiedNexusAccount, role?: Exclude<NexusMemberRole, 'administrator'>) => Promise<NexusInvite>;
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
  return { error: false, message: NEXUS_JOINED_AS_MEMBER };
}
export interface NexusFailureNotice { message: string; detail?: string }
const INVITE_FAILURES: readonly (readonly [RegExp, string])[] = [
  [/group no longer exists/i, 'The device that created this invitation is no longer in a network. Create a new invitation on a device that is in the mesh.'],
  [/invalid or expired link token|link token.*\b(?:expired|invalid|spent|unknown)\b|\b(?:expired|invalid|spent|unknown)\b.*link token/i, 'This code is wrong, already used, or older than 10 minutes. Create a new invitation.'],
  [/could not establish connection|connection error|unreachable/i, 'Could not reach the Nexus servers. Check the network and try again.'],
  [/not allowed|permission|forbidden/i, 'The inviting device may not add devices. Ask an administrator of the network for an invitation.'],
];
// Server and SDK invitation failures are terse.
export function nexusInviteFailure(error: unknown): NexusFailureNotice {
  const detail = error instanceof Error ? error.message : String(error);
  const known = INVITE_FAILURES.find(([pattern]) => pattern.test(detail))
    ?? (error instanceof NexusRequestError && error.status === 403 ? INVITE_FAILURES[3] : undefined);
  return known ? { message: known[1], detail } : { message: detail };
}

export function verifyAvatarReply(value: unknown): NexusAvatarState {
  const reply = value as Partial<NexusAvatarState & { image: string | null; revision: number; can_write: boolean; pending?: boolean }> | null;
  if (!reply || reply.state === 'unsupported') return { state: 'unsupported' };
  if (reply.state === 'unavailable') return { state: 'unavailable' };
  if (reply.state !== 'ready' || !Number.isSafeInteger(reply.revision) || reply.revision! < 0 || typeof reply.can_write !== 'boolean'
    || (reply.pending !== undefined && (typeof reply.pending !== 'boolean' || (reply.pending && reply.image !== null)))
    || (reply.image !== null && (typeof reply.image !== 'string' || reply.image.length > NEXUS_AVATAR_MAX_BYTES
      || !/^data:image\/png;base64,[A-Za-z0-9+/]+={0,2}$/.test(reply.image)))) {
    throw new Error('The mesh returned invalid profile image data.');
  }
  return reply as NexusAvatarState & { state: 'ready' };
}

interface AccountReply { id: string; member_id: string; device_node_id: string; role: NonNullable<NexusAccount['role']>; devices: NetworkDevice[]; avatar?: unknown }
// lemond reports no account name. `expectedId` is the account of a running session: lemond
// replaces the device key, and with it the account, when the network revokes it, so a
// changed id ends that session.
export function verifyAccountReply(value: unknown, source: string, expectedId?: string, fallbackName = ''): VerifiedNexusAccount {
  const reply = value as AccountReply | null;
  if (!reply || typeof reply.id !== 'string' || !reply.id || typeof reply.member_id !== 'string' || !reply.member_id
    || typeof reply.device_node_id !== 'string' || !reply.device_node_id || !isNexusRole(reply.role) || !Array.isArray(reply.devices)) {
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
    canInvite: hasNexusPermission(self.permissions, 'add_child') };
}

export const validNexusInvitation = (value: string) => {
  const code = normalizeNexusInvitation(value);
  return NEXUS_INVITE_CODE.test(code) || NEXUS_INVITE_TOKEN.test(code);
};
const boundedName = (name: string) => Boolean(name.trim()) && !nexusNameTooLong(name);

export function createNexusAccountService(dependencies = { controller: nexusAccountController, desktop: isNexusDesktop, prepare: prepareNexus }): NexusAccountService {
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
  return {
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
      if (input.action === 'join' && !validNexusInvitation(input.invitation || '')) throw new Error(INVITATION_FORMAT_ERROR);
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
        const code = normalizeNexusInvitation(input.invitation!);
        if (prepared?.mesh_up) await acceptNexusInvite(code, controller.request);
        else await controller.request('join', { code });
      }
      const session = verifyAccountReply(await controller.request('account'), controller.source, undefined, input.deviceName);
      await currentController(input.source);
      return session;
    },
    async joinNetwork(session, invitation) {
      if (!validNexusInvitation(invitation)) throw new Error(INVITATION_FORMAT_ERROR);
      const controller = await requireController(session.source);
      const reply = await acceptNexusInvite(invitation, controller.request);
      const status = reply?.status ?? 'member';
      if (!isNexusMembershipState(status)) throw new Error('The server returned an invalid membership. Refresh and check your networks.');
      return { status, group_id: typeof reply.group_id === 'string' ? reply.group_id : undefined, ...(typeof reply.error === 'string' ? { error: reply.error } : {}) };
    },
    async memberships(session) {
      const controller = await requireController(session.source);
      const reply = await getNexusMemberships(controller.request);
      if (!reply || !Array.isArray(reply.memberships)) throw new Error('The server returned invalid memberships.');
      return reply.memberships.filter(group => group && typeof group.node_id === 'string' && isNexusGroupRole(group.role)
          && (group.permissions === undefined || isNexusGrantList(group.permissions)))
        .map(group => ({ ...group, hostname: typeof group.hostname === 'string' ? group.hostname : '', permissions: group.permissions ?? [] }));
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
      if (next.memberId !== session.memberId || next.deviceNodeId !== session.deviceNodeId) throw new Error(MEMBERSHIP_CHANGED_ERROR);
      if (next.avatar.state === 'unavailable') next.account.avatarUrl = session.account.avatarUrl;
      return next;
    },
    async invite(session, role) {
      const controller = await requireController(session.source);
      const current = verifyAccountReply(await controller.request('account'), session.source, session.account.id, session.account.deviceName);
      if (current.memberId !== session.memberId || current.deviceNodeId !== session.deviceNodeId) throw new Error(MEMBERSHIP_CHANGED_ERROR);
      const invite = await controller.request<NexusInvite>('invites', { ttl_sec: NEXUS_INVITE_TTL_SEC, ...(role ? { role } : {}) });
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
      await currentController(session.source);
      if (next.state !== 'ready' || next.revision <= session.avatar.revision) throw new Error('The mesh did not confirm the profile image change. Try again.');
      return next;
    },
  };
}

export const nexusAccountService = createNexusAccountService();
