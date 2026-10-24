import React, { createContext, useContext, useEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { NexusAccountInviteDialog, NexusSignInDialog } from '../../components/NexusSignInDialog';
import { NexusPasskeyDialog } from '../../components/NexusPasskeyDialog';
import { nexusAccountService, saveNexusAccountLabel, type NexusAccountService, type VerifiedNexusAccount } from './accountSession';
import { refreshNexus, useNexusNetwork } from './nexus';

export interface NexusAccount {
  id: string;
  displayName: string;
  role?: 'admin' | 'user';
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
  devices: readonly NexusDevice[];
  onSignIn?: () => void;
  onSignOut?: () => void;
  onConnectDevice?: () => void;
  notice?: string;
}

const SIGNED_OUT: NexusAccountConnection = { account: null, devices: [] };

const NexusAccountContext = createContext<NexusAccountConnection & {
  avatarUrl?: string;
  setAvatar: (image: string | null) => void;
}>({ ...SIGNED_OUT, setAvatar: () => undefined });

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
  const accountId = connection.account?.id;
  const [avatar, setAvatar] = useState<{ accountId: string; image: string | null }>();
  useEffect(() => { setAvatar(undefined); }, [accountId]);
  return <NexusAccountContext.Provider value={{
    ...connection,
    avatarUrl: avatar && avatar.accountId === accountId ? avatar.image ?? undefined : connection.account?.avatarUrl,
    setAvatar: image => { if (accountId) setAvatar({ accountId, image }); },
  }}>{children}</NexusAccountContext.Provider>;
}

function ConnectedAccountProvider({ service, children }: { service: NexusAccountService; children: React.ReactNode }) {
  const [session, setSession] = useState<VerifiedNexusAccount>();
  const [dialog, setDialog] = useState<'signin' | 'invite'>();
  const [notice, setNotice] = useState('');
  const generation = useRef(0);
  const origin = useRef<HTMLElement | null>(null);
  const network = useNexusNetwork(Boolean(session) || dialog === 'signin');
  const restoreFocus = () => window.requestAnimationFrame(() => {
    const target = origin.current?.isConnected ? origin.current : document.querySelector<HTMLElement>('.nexus-account-trigger');
    target?.focus({ preventScroll: true });
  });
  const close = () => { setDialog(undefined); restoreFocus(); };
  const open = (next: 'signin' | 'invite') => {
    origin.current = document.activeElement as HTMLElement | null;
    setNotice(''); setDialog(next);
  };
  const signOut = () => {
    generation.current += 1; setSession(undefined); setDialog(undefined); setNotice(''); restoreFocus();
  };
  useEffect(() => {
    if (!session) return;
    const revision = generation.current;
    let running = false;
    let stopped = false;
    const refresh = async () => {
      if (running) return;
      running = true;
      try {
        const next = await service.refresh(session);
        if (!stopped && revision === generation.current) setSession(next);
      } catch {
        if (!stopped && revision === generation.current) {
          generation.current += 1; setSession(undefined); setDialog(undefined);
          setNotice('session-ended');
        }
      } finally { running = false; }
    };
    const timer = window.setInterval(() => void refresh(), 5000);
    return () => { stopped = true; window.clearInterval(timer); };
  }, [session?.account.id, session?.memberId, session?.source, service]);
  return <AccountStateProvider connection={{
    account: session?.account || null, devices: session?.devices || [], notice,
    onSignIn: () => open('signin'), onSignOut: signOut,
    onConnectDevice: session?.canInvite ? () => open('invite') : undefined,
  }}>
    {children}
    {dialog === 'signin' && createPortal(<NexusSignInDialog service={service} onClose={close} passkeyActive={Boolean(network.passkey)} onSignedIn={verified => {
      generation.current += 1; saveNexusAccountLabel(verified); setSession(verified); void refreshNexus(); close();
    }} />, document.body)}
    {dialog === 'invite' && session && createPortal(<NexusAccountInviteDialog service={service} session={session} onClose={close} />, document.body)}
    {network.passkey && createPortal(<NexusPasskeyDialog prompt={network.passkey} />, document.body)}
  </AccountStateProvider>;
}

export function useNexusAccount() {
  return useContext(NexusAccountContext);
}
