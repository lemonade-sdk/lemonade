import React, { createContext, useContext, useEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { NexusAccountInviteDialog, NexusDeleteAccountDialog, NexusJoinNetworkDialog, NexusSignInDialog } from '../../components/NexusSignInDialog';
import { NexusPasskeyDialog } from '../../components/NexusPasskeyDialog';
import { nexusAccountService, type FoundServerInvite, type NexusAccountService, type VerifiedNexusAccount } from './accountSession';
import { clearNexusNetwork, NexusRequestError, refreshNexus, useNexusNetwork, type NexusMembership } from './nexus';
import type { NexusRole } from './routePermissions';

export interface NexusAccount {
  id: string;
  displayName: string;
  role?: NexusRole;
  avatarUrl?: string;
}

export interface NexusDevice {
  id: string;
  name: string;
  kind: 'computer' | 'phone';
  current?: boolean;
  connected: boolean;
}

export interface NexusAccountConnection {
  account: NexusAccount | null;
  source?: string;
  devices: readonly NexusDevice[];
  onSignIn?: () => void;
  onSignOut?: () => void;
  onConnectDevice?: () => void;
  // Invite a Lemonade server found on this network (#1221) and join it from here.
  onInviteFoundServer?: (endpoint: string, deviceName: string) => Promise<FoundServerInvite>;
  // Joining another network and the groups this identity belongs to, while the mesh is up.
  onJoinNetwork?: () => void;
  memberships?: readonly NexusMembership[];
  onLeaveNetwork?: (groupId: string) => Promise<void>;
  onDeleteAccount?: () => void;
  onAvatarChange?: (image: string | null) => Promise<void>;
  avatarSaving?: boolean;
  avatarNotice?: 'unsupported' | 'unavailable' | 'readonly' | 'pending';
  notice?: string;
}

const SIGNED_OUT: NexusAccountConnection = { account: null, devices: [] };

const NexusAccountContext = createContext<NexusAccountConnection & {
  avatarUrl?: string;
  setAvatar: (image: string | null) => Promise<void>;
}>({ ...SIGNED_OUT, setAvatar: async () => { throw new Error('Enable Mesh before changing your image.'); } });

export function NexusAccountProvider({ connection, service = nexusAccountService, children }: {
  connection?: NexusAccountConnection;
  service?: NexusAccountService;
  children: React.ReactNode;
}) {
  return connection !== undefined
    ? <AccountStateProvider connection={connection}>{children}</AccountStateProvider>
    : <ConnectedAccountProvider service={service}>{children}</ConnectedAccountProvider>;
}

function AccountStateProvider({ connection, children }: { connection: NexusAccountConnection; children: React.ReactNode }) {
  return <NexusAccountContext.Provider value={{
    ...connection,
    avatarUrl: connection.account?.avatarUrl,
    setAvatar: connection.onAvatarChange || (async () => { throw new Error('Profile images cannot be changed right now.'); }),
  }}>{children}</NexusAccountContext.Provider>;
}

function ConnectedAccountProvider({ service, children }: { service: NexusAccountService; children: React.ReactNode }) {
  const [session, setSession] = useState<VerifiedNexusAccount>();
  const [dialog, setDialog] = useState<'signin' | 'invite' | 'join' | 'delete'>();
  const [memberships, setMemberships] = useState<NexusMembership[]>();
  const [membershipRevision, setMembershipRevision] = useState(0);
  const [notice, setNotice] = useState('');
  const disconnecting = useRef(false);
  const generation = useRef(0);
  const avatarGeneration = useRef(0);
  const avatarWriting = useRef(false);
  const [avatarSaving, setAvatarSaving] = useState(false);
  const origin = useRef<HTMLElement | null>(null);
  const network = useNexusNetwork(Boolean(session) || dialog === 'signin');
  // A device-key server whose mesh is already up needs no Enable Mesh click: the page
  // picks its account up on load, as it would after that click.
  useEffect(() => {
    let stopped = false;
    service.resume().then(found => {
      if (stopped || !found) return;
      generation.current += 1;
      setSession(current => current || found);
      void refreshNexus();
    }).catch(() => {});
    return () => { stopped = true; };
  }, [service]);
  const restoreFocus = () => window.requestAnimationFrame(() => {
    const target = origin.current?.isConnected ? origin.current : document.querySelector<HTMLElement>('.nexus-account-trigger');
    target?.focus({ preventScroll: true });
  });
  const close = () => { setDialog(undefined); restoreFocus(); };
  const open = (next: 'signin' | 'invite' | 'join' | 'delete') => {
    origin.current = document.activeElement as HTMLElement | null;
    setNotice(''); setDialog(next);
  };
  const signOut = async () => {
    if (!session || disconnecting.current) return;
    disconnecting.current = true;
    try {
      await service.disconnect(session);
      generation.current += 1;
      clearNexusNetwork();
      setSession(undefined); setDialog(undefined); setNotice(''); restoreFocus();
    } catch (error) {
      setNotice(error instanceof Error ? error.message : 'The device did not disconnect. Try again.');
    } finally { disconnecting.current = false; }
  };
  const meshUp = Boolean(network.status?.mesh_up && !network.status.locked);
  useEffect(() => {
    if (!session || !meshUp) { setMemberships(undefined); return; }
    let stopped = false;
    service.memberships(session).then(list => { if (!stopped) setMemberships(list); }).catch(() => { if (!stopped) setMemberships(undefined); });
    return () => { stopped = true; };
  }, [session?.account.id, session?.source, meshUp, membershipRevision, service]);
  useEffect(() => {
    if (!session) return;
    const revision = generation.current;
    let running = false;
    let stopped = false;
    const refresh = async () => {
      if (running || avatarWriting.current || disconnecting.current) return;
      running = true;
      const imageRevision = avatarGeneration.current;
      try {
        const next = await service.refresh(session);
        if (!stopped && !disconnecting.current && revision === generation.current && imageRevision === avatarGeneration.current) {
          setSession(current => next.avatar.state === 'unavailable' && current
            ? { ...next, account: { ...next.account, avatarUrl: current.account.avatarUrl } } : next);
          setNotice('');
        }
      } catch (error) {
        if (!stopped && !disconnecting.current && revision === generation.current && imageRevision === avatarGeneration.current) {
          if ((error instanceof NexusRequestError && (error.retryable || error.status === 429 || error.status >= 500))
              || error instanceof TypeError || (error instanceof Error && error.name === 'TimeoutError')) {
            setNotice('reconnecting');
            return;
          }
          generation.current += 1; setSession(undefined); setDialog(undefined);
          setNotice('session-ended');
        }
      } finally { running = false; }
    };
    const timer = window.setInterval(() => void refresh(), 15000);
    return () => { stopped = true; window.clearInterval(timer); };
  }, [session?.account.id, session?.memberId, session?.deviceNodeId, session?.source, service]);
  return <AccountStateProvider connection={{
    account: session?.account || null, source: session?.source, devices: session?.devices || [], notice,
    onSignIn: () => open('signin'), onSignOut: signOut,
    onConnectDevice: session?.canInvite ? () => open('invite') : undefined,
    onInviteFoundServer: session?.canInvite ? async (endpoint, deviceName) => {
      const result = await service.inviteFoundServer(session, endpoint, deviceName);
      if (result.joined) {
        void refreshNexus();
        service.refresh(session).then(setSession).catch(() => undefined);
      }
      return result;
    } : undefined,
    onJoinNetwork: session && meshUp ? () => open('join') : undefined,
    memberships,
    onLeaveNetwork: session && meshUp ? async groupId => {
      await service.leaveNetwork(session, groupId);
      setMembershipRevision(current => current + 1);
      void refreshNexus();
    } : undefined,
    onDeleteAccount: session ? () => open('delete') : undefined,
    avatarSaving,
    avatarNotice: session?.avatar.state === 'ready' ? session.avatar.pending ? 'pending' : session.avatar.can_write ? undefined : 'readonly' : session?.avatar.state,
    onAvatarChange: session?.avatar.state === 'ready' && session.avatar.can_write ? async image => {
      if (avatarWriting.current) throw new Error('A profile image change is already being saved.');
      const revision = generation.current;
      avatarWriting.current = true; avatarGeneration.current += 1; setAvatarSaving(true);
      try {
        const avatar = await service.setAvatar(session, image);
        if (revision !== generation.current) throw new Error('The account changed before the image was saved.');
        setSession(current => current ? { ...current, avatar, account: { ...current.account, avatarUrl: avatar.image ?? undefined } } : current);
      } finally {
        avatarGeneration.current += 1; avatarWriting.current = false; setAvatarSaving(false);
      }
    } : undefined,
  }}>
    {children}
    {dialog === 'signin' && createPortal(<NexusSignInDialog service={service} onClose={close} passkeyActive={Boolean(network.passkey)} onSignedIn={verified => {
      generation.current += 1; setSession(verified); void refreshNexus(); close();
    }} />, document.body)}
    {dialog === 'invite' && session && createPortal(<NexusAccountInviteDialog service={service} session={session} onClose={close} />, document.body)}
    {dialog === 'join' && session && createPortal(<NexusJoinNetworkDialog service={service} session={session} onClose={close}
      onJoined={() => { setMembershipRevision(current => current + 1); void refreshNexus(); }} />, document.body)}
    {dialog === 'delete' && session && createPortal(<NexusDeleteAccountDialog service={service} session={session} onClose={close} onDeleted={() => {
      generation.current += 1; clearNexusNetwork(); setSession(undefined); setDialog(undefined); setNotice('account-deleted'); restoreFocus();
    }} />, document.body)}
    {network.passkey && createPortal(<NexusPasskeyDialog prompt={network.passkey} />, document.body)}
  </AccountStateProvider>;
}

export function useNexusAccount() {
  return useContext(NexusAccountContext);
}
