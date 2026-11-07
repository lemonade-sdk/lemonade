import React, { useEffect, useId, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { useNexusAccount } from '../features/nexus/NexusAccountContext';
import {
  NEXUS_CONNECTION_LABELS, isLoopbackUrl, nexusDeviceConnection, nexusInferenceServers, nexusServerUnavailable, selectNexusInferenceServer, useNexusNetwork, type NexusConnection,
} from '../features/nexus/nexus';
import { useI18n } from '../i18n';
import { Icon } from './Icon';
import { WorkspaceActionButton } from './WorkspacePanels';
import { NexusNetwork } from './NexusNetwork';
import { NexusDeviceRows, NexusMeshPanel, nexusListedDevices } from './NexusMesh';
import '../styles/nexus.css';

function AccountAvatar({ small = false }: { small?: boolean }) {
  const { account, avatarUrl } = useNexusAccount();
  const [failedUrl, setFailedUrl] = useState<string>();
  return <span className={`nexus-avatar${small ? ' nexus-avatar--small' : ''}`} aria-hidden="true">
    {avatarUrl && failedUrl !== avatarUrl
      ? <img src={avatarUrl} alt="" onError={() => setFailedUrl(avatarUrl)} />
      : account?.displayName.trim()
        ? Array.from(account.displayName.trim())[0].toLocaleUpperCase()
        : <Icon name="user-round" size={small ? 16 : 27} />}
  </span>;
}

function AvatarPicker({ errorId, onError }: { errorId: string; onError: (error: string | undefined) => void }) {
  const { t } = useI18n();
  const { avatarUrl, setAvatar, onAvatarChange, avatarSaving } = useNexusAccount();
  const inputRef = useRef<HTMLInputElement>(null);
  const buttonRef = useRef<HTMLButtonElement>(null);
  const requestRef = useRef(0);
  const [busy, setBusy] = useState(false);

  useEffect(() => () => { requestRef.current += 1; }, []);

  const chooseImage = async (file: File) => {
    const request = ++requestRef.current;
    onError(undefined);
    if (!['image/png', 'image/jpeg', 'image/webp'].includes(file.type) || file.size > 5 * 1024 * 1024) {
      onError(t('Choose a PNG, JPEG or WebP image under 5 MB.'));
      return;
    }
    setBusy(true);
    const url = URL.createObjectURL(file);
    let decoded = false;
    try {
      const image = new Image();
      image.src = url;
      await image.decode();
      const canvas = document.createElement('canvas');
      canvas.width = canvas.height = 128;
      const context = canvas.getContext('2d');
      if (!context) throw new Error('Canvas unavailable');
      const side = Math.min(image.naturalWidth, image.naturalHeight);
      context.drawImage(image, (image.naturalWidth - side) / 2, (image.naturalHeight - side) / 2, side, side, 0, 0, 128, 128);
      const data = canvas.toDataURL('image/png');
      if (data.length > 128 * 1024) throw new Error('Profile image is too large');
      decoded = true;
      if (request === requestRef.current) await setAvatar(data);
    } catch (error) {
      if (request === requestRef.current) onError(decoded && error instanceof Error ? error.message : t('This image could not be opened. Choose another image.'));
    } finally {
      URL.revokeObjectURL(url);
      if (request === requestRef.current) setBusy(false);
    }
  };

  const removeImage = async () => {
    const request = ++requestRef.current;
    onError(undefined); setBusy(true);
    try {
      await setAvatar(null);
      if (request === requestRef.current) buttonRef.current?.focus();
    } catch (error) {
      if (request === requestRef.current) onError(error instanceof Error ? error.message : t('The profile image could not be removed. Try again.'));
    } finally { if (request === requestRef.current) setBusy(false); }
  };

  const disabled = busy || avatarSaving || !onAvatarChange;

  return <div className="nexus-avatar-picker">
    <button ref={buttonRef} type="button" className="nexus-avatar-picker__button" disabled={disabled}
      aria-label={t('Change profile image')} aria-describedby={errorId}
      onClick={() => inputRef.current?.click()}>
      <span className="nexus-avatar-picker__preview"><AccountAvatar /><span className="nexus-avatar-picker__badge"><Icon name="camera" size={13} /></span></span>
      <span>{avatarSaving ? t('Saving image…') : busy ? t('Opening image…') : t('Change image')}</span>
    </button>
    <input ref={inputRef} type="file" accept="image/png,image/jpeg,image/webp" hidden tabIndex={-1}
      aria-label={t('Profile image file')}
      onChange={event => {
        const file = event.target.files?.[0];
        event.target.value = '';
        if (file) void chooseImage(file);
      }} />
    {avatarUrl && <button type="button" className="nexus-text-button" disabled={disabled} onClick={() => void removeImage()}>{t('Remove image')}</button>}
  </div>;
}

function AccountSummary() {
  const { t } = useI18n();
  const { account, avatarNotice, source } = useNexusAccount();
  const [error, setError] = useState<string>();
  const errorId = useId();
  useEffect(() => { setError(undefined); }, [account?.id]);
  return <div>
    <div className="nexus-account-summary">
      {account ? <AvatarPicker key={account.id} errorId={errorId} onError={setError} /> : <AccountAvatar />}
      <div className="nexus-account-summary__details">
        <div className="nexus-account-summary__name">
          <strong>{account?.displayName || t('Lemonade account')}</strong>
          {account?.role && account.role !== 'user' && <span className="nexus-role">{t(account.role === 'admin' ? 'Admin' : 'Moderator')}</span>}
        </div>
        <span className={`nexus-account-status${account ? ' nexus-account-status--signed-in' : ''}`}>
          {account && <Icon name="check" size={14} />}{account ? t('Mesh enabled') : t('Mesh disabled')}
        </span>
        {account && avatarNotice && <p className="nexus-hint" role="status">{t(avatarNotice === 'unsupported'
          ? 'This mesh needs a Nexus server update to sync profile images.' : avatarNotice === 'pending'
            ? 'Connect a device with this profile image to download it.' : avatarNotice === 'readonly'
            ? 'You can view this account image. Changing it requires write access.'
            : 'Profile images are temporarily unavailable. Try again shortly.')}</p>}
      </div>
    </div>
    {account && source && !isLoopbackUrl(source) && <p className="nexus-server-target"><span>{t('Managing server')}</span><strong>{new URL(source).origin}</strong></p>}
    <p id={errorId} className="nexus-error" role="alert">{error}</p>
  </div>;
}

function AccountAccess({ showSignOut = false }: { showSignOut?: boolean }) {
  const { t } = useI18n();
  const { account, onSignIn, onSignOut, onDeleteAccount, notice } = useNexusAccount();
  const hintId = useId();
  if (account) return <div className="nexus-account-access">
    {showSignOut && <div className="nexus-group-actions">
      {onSignOut && <WorkspaceActionButton onClick={onSignOut}>{t('Disable Mesh')}</WorkspaceActionButton>}
      {onDeleteAccount && <WorkspaceActionButton appearance="quiet" icon="trash" onClick={onDeleteAccount}>{t('Delete account')}</WorkspaceActionButton>}
    </div>}
    {notice && <p className="nexus-error" role="alert">{t(notice === 'reconnecting' ? 'The network is temporarily unavailable. Reconnecting…' : notice)}</p>}
  </div>;
  return <div className="nexus-account-access">
    <p>{t('Enable Mesh to bring your Lemonade devices together.')}</p>
    {notice === 'session-ended' && <p className="nexus-error" role="status">{t('Account access ended. Enable Mesh again to continue.')}</p>}
    {notice === 'account-deleted' && <p className="nexus-hint" role="status">{t('Your account was deleted. Enable Mesh to create or join a network again.')}</p>}
    <WorkspaceActionButton onClick={onSignIn} disabled={!onSignIn} aria-describedby={!onSignIn ? hintId : undefined}>{t('Enable Mesh')}</WorkspaceActionButton>
    {!onSignIn && <p id={hintId} className="nexus-hint">{t('Nexus is not available yet.')}</p>}
  </div>;
}

const CHAT_HOST_TONES: Record<NexusConnection, string> = { p2p: 'good', relay: 'warn', connecting: 'accent', offline: 'muted' };
function ChatHostRow({ selected, title, subtitle, connection, disabled, onChoose }: {
  selected: boolean; title: string; subtitle?: string; connection?: NexusConnection; disabled: boolean; onChoose: () => void;
}) {
  const { t } = useI18n();
  return <button type="button" className={`nexus-mesh-row nexus-mesh-row--choice nexus-chat-host${selected ? ' is-selected' : ''}`} aria-pressed={selected} disabled={disabled} onClick={onChoose}>
    <span className="nexus-mesh-row__body">
      <span className="nexus-mesh-row__name">{title}</span>
      {subtitle && <span className="nexus-mesh-row__sub">{subtitle}</span>}
    </span>
    {connection && <span className={`nexus-pill nexus-pill--${CHAT_HOST_TONES[connection]} nexus-pill--connection`}>{t(NEXUS_CONNECTION_LABELS[connection])}</span>}
    {selected && <Icon name="check" size={16} className="nexus-mesh-row__check" />}
  </button>;
}

// The account menu's chat host: the same choice Settings → Devices & mesh offers, in short rows.
function ChatRunsOn() {
  const { t } = useI18n();
  const network = useNexusNetwork(false);
  const { status } = network;
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const servers = nexusInferenceServers(status);
  const selected = network.selectedServerId || 'local';
  const disabled = busy || Boolean(network.switchingServer);
  const choose = async (target: string) => {
    if (disabled || target === selected) return;
    setBusy(true); setError('');
    try { await selectNexusInferenceServer(target); }
    catch (error) { setError(error instanceof Error ? error.message : String(error)); }
    finally { setBusy(false); }
  };
  return <div className="nexus-section nexus-chat-hosts">
    <p className="nexus-mesh__label">{t('Chat runs on')}</p>
    <ChatHostRow selected={selected === 'local'} title={t('This device')} subtitle={t('Models on this computer')} disabled={disabled} onChoose={() => void choose('local')} />
    {servers.map(device => {
      // A server without a path cannot serve chat yet; its pill says why (Offline or Connecting).
      const unavailable = nexusServerUnavailable(status, device);
      return <ChatHostRow key={device.node_id} selected={selected === device.node_id} title={device.device_name || t('Unnamed device')}
        subtitle={device.tunnel_ip.split('/')[0]} connection={unavailable ?? nexusDeviceConnection(status, device)} disabled={disabled || Boolean(unavailable)} onChoose={() => void choose(device.node_id)} />;
    })}
    {error && <p className="nexus-error" role="alert">{error}</p>}
  </div>;
}

// The account menu's device list: open by default, the same rows as the mesh panel.
function MyDevices() {
  const { t } = useI18n();
  const { account, devices, onConnectDevice } = useNexusAccount();
  const { status } = useNexusNetwork(false);
  const listed = account ? nexusListedDevices(status, devices) : [];
  return <details className="nexus-section nexus-device-disclosure" open>
    <summary><span>{t('My devices')}</span><Icon name="chevron-right" size={16} /></summary>
    <div className="nexus-device-disclosure__content">
    {listed.length ? <NexusDeviceRows status={status} devices={listed} /> : <div className="nexus-device-empty">
      <Icon name="smartphone" size={25} />
      <div><strong>{t('Your devices')}</strong><p>{account ? t('No devices connected yet.') : t('Enable Mesh to connect your phone or another computer.')}</p></div>
    </div>}
    {account && onConnectDevice && <WorkspaceActionButton icon="plus" onClick={onConnectDevice}>{t('Connect another device')}</WorkspaceActionButton>}
    </div>
  </details>;
}

export function NexusAccountPage({ foundServers }: { foundServers?: React.ReactNode } = {}) {
  const { t } = useI18n();
  const { account } = useNexusAccount();
  const modeId = useId();
  const [advanced, setAdvanced] = useState(() => {
    try { return localStorage.getItem('lemonade_nexus_view') === 'advanced'; } catch { return false; }
  });
  const changeMode = (next: boolean) => {
    setAdvanced(next);
    try { localStorage.setItem('lemonade_nexus_view', next ? 'advanced' : 'basic'); } catch { /* The view still works when storage is unavailable. */ }
  };
  return <div className="nexus-page">
    <section className="nexus-section" aria-labelledby="nexus-account-heading">
      <h3 id="nexus-account-heading">{t('Account')}</h3>
      <AccountSummary />
      <AccountAccess showSignOut />
    </section>
    {account && <React.Fragment key={account.id}>
    <div className="nexus-view-heading">
      <div><h3>{t('Your network')}</h3><p aria-hidden={!advanced}>{t('Explore the tree and manage group access.')}</p></div>
      <fieldset className="nexus-view-mode"><legend className="sr-only">{t('Management mode')}</legend>
        <label className={!advanced ? 'is-selected' : ''}><input type="radio" name={modeId} checked={!advanced} onChange={() => changeMode(false)} />{t('Basic')}</label>
        <label className={advanced ? 'is-selected' : ''}><input type="radio" name={modeId} checked={advanced} onChange={() => changeMode(true)} />{t('Advanced')}</label>
      </fieldset>
    </div>
    <NexusMeshPanel />
    <NexusNetwork advanced={advanced} showCompact={false} />
    {foundServers}
    </React.Fragment>}
  </div>;
}

export function NexusAccountMenu({ open, onOpenChange, onOpenPage }: {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  onOpenPage: () => void;
}) {
  const { t } = useI18n();
  const { account, onSignOut } = useNexusAccount();
  const triggerRef = useRef<HTMLButtonElement>(null);
  const panelRef = useRef<HTMLDivElement>(null);
  const closeRef = useRef<HTMLButtonElement>(null);
  const panelId = useId();
  const titleId = useId();
  const close = (restoreFocus: boolean) => {
    onOpenChange(false);
    if (restoreFocus) triggerRef.current?.focus();
  };

  useEffect(() => {
    if (!open) return;
    closeRef.current?.focus();
    const outside = (event: Event) => {
      const target = event.target as Node;
      if (target instanceof Element && target.closest('[data-header-menu-trigger="downloads"]')) return;
      if (!panelRef.current?.contains(target) && !triggerRef.current?.contains(target)) onOpenChange(false);
    };
    const escape = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        event.preventDefault();
        onOpenChange(false);
        triggerRef.current?.focus();
      }
    };
    document.addEventListener('pointerdown', outside);
    document.addEventListener('focusin', outside);
    document.addEventListener('keydown', escape);
    return () => {
      document.removeEventListener('pointerdown', outside);
      document.removeEventListener('focusin', outside);
      document.removeEventListener('keydown', escape);
    };
  }, [open, onOpenChange]);

  return <>
    <button ref={triggerRef} type="button" className="nexus-account-trigger" data-tauri-drag-region="false"
      aria-label={account ? t('User: {name}', { name: account.displayName }) : t('User: Mesh disabled')}
      aria-haspopup="dialog" aria-expanded={open} aria-controls={open ? panelId : undefined}
      onClick={() => onOpenChange(!open)}>
      <AccountAvatar small /><span className="nexus-account-trigger__label">{account ? t('User: {name}', { name: account.displayName }) : t('User')}</span><Icon name="chevron-down" size={13} />
    </button>
    {open && createPortal(<div ref={panelRef} id={panelId} className="nexus-account-menu" role="dialog" aria-labelledby={titleId}>
      <div className="nexus-account-menu__header">
        <h2 id={titleId} className="sr-only">{t('Lemonade Account')}</h2>
        <AccountSummary />
        <WorkspaceActionButton ref={closeRef} appearance="quiet" size="small" iconOnly icon="x" aria-label={t('Close account menu')} onClick={() => close(true)} />
      </div>
      <AccountAccess />
      {account && <ChatRunsOn />}
      <MyDevices />
      <button type="button" className="nexus-account-menu__link" onClick={() => { close(true); onOpenPage(); }}>
        <span>{t('Open Accounts')}</span><Icon name="chevron-right" size={16} />
      </button>
      {account && onSignOut && <WorkspaceActionButton className="nexus-account-menu__sign-out" appearance="quiet" onClick={onSignOut}>{t('Disable Mesh')}</WorkspaceActionButton>}
    </div>, document.body)}
  </>;
}
