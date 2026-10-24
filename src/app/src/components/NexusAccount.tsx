import React, { useEffect, useId, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { useNexusAccount } from '../features/nexus/NexusAccountContext';
import { useI18n } from '../i18n';
import { Icon } from './Icon';
import { WorkspaceActionButton } from './WorkspacePanels';
import { NexusNetwork } from './NexusNetwork';
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
  const { account, avatarNotice } = useNexusAccount();
  const [error, setError] = useState<string>();
  const errorId = useId();
  useEffect(() => { setError(undefined); }, [account?.id]);
  return <div>
    <div className="nexus-account-summary">
      {account ? <AvatarPicker key={account.id} errorId={errorId} onError={setError} /> : <AccountAvatar />}
      <div className="nexus-account-summary__details">
        <div className="nexus-account-summary__name">
          <strong>{account?.displayName || t('Lemonade account')}</strong>
          {account?.role === 'admin' && <span className="nexus-role">{t('Admin')}</span>}
        </div>
        <span className={`nexus-account-status${account ? ' nexus-account-status--signed-in' : ''}`}>
          {account && <Icon name="check" size={14} />}{account ? t('Signed in') : t('Not signed in')}
        </span>
        {account && avatarNotice && <p className="nexus-hint" role="status">{t(avatarNotice === 'unsupported'
          ? 'This mesh needs a Nexus server update to sync profile images.' : avatarNotice === 'pending'
            ? 'Connect a device with this profile image to download it.' : avatarNotice === 'readonly'
            ? 'You can view this account image. Changing it requires write access.'
            : 'Profile images are temporarily unavailable. Try again shortly.')}</p>}
      </div>
    </div>
    <p id={errorId} className="nexus-error" role="alert">{error}</p>
  </div>;
}

function AccountAccess({ showSignOut = false }: { showSignOut?: boolean }) {
  const { t } = useI18n();
  const { account, onSignIn, onSignOut, notice } = useNexusAccount();
  const hintId = useId();
  if (account) return showSignOut && onSignOut ? <div className="nexus-account-access"><WorkspaceActionButton onClick={onSignOut}>{t('Sign out')}</WorkspaceActionButton></div> : null;
  return <div className="nexus-account-access">
    <p>{t('Sign in to bring your Lemonade devices together.')}</p>
    {notice === 'session-ended' && <p className="nexus-error" role="status">{t('Account access ended. Sign in again to continue.')}</p>}
    <WorkspaceActionButton onClick={onSignIn} disabled={!onSignIn} aria-describedby={!onSignIn ? hintId : undefined}>{t('Connect to Mesh')}</WorkspaceActionButton>
    {!onSignIn && <p id={hintId} className="nexus-hint">{t('Nexus sign-in is not available yet.')}</p>}
  </div>;
}

function MyDevices() {
  const { t } = useI18n();
  const { account, devices, onConnectDevice } = useNexusAccount();
  const headingId = useId();
  return <section className="nexus-section" aria-labelledby={headingId}>
    <h3 id={headingId}>{t('My devices')}</h3>
    {account && devices.length ? <ul className="nexus-devices">
      {devices.map(device => <li key={device.id}>
        <Icon name={device.kind === 'phone' ? 'smartphone' : 'monitor'} size={23} />
        <div><strong>{device.name}</strong><span>{device.current ? t('Current device') : device.connected ? t('Connected') : t('Offline')}</span></div>
      </li>)}
    </ul> : <div className="nexus-device-empty">
      <Icon name="smartphone" size={25} />
      <div><strong>{t('Your devices')}</strong><p>{account ? t('No devices connected yet.') : t('Sign in to connect your phone or another computer.')}</p></div>
    </div>}
    {account && onConnectDevice && <WorkspaceActionButton icon="plus" onClick={onConnectDevice}>{t('Connect another device')}</WorkspaceActionButton>}
  </section>;
}

export function NexusAccountPage() {
  const { t } = useI18n();
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
    <div className="nexus-view-heading">
      <div><h3>{t('Your network')}</h3><p aria-hidden={!advanced}>{t('Explore the tree and manage group access.')}</p></div>
      <fieldset className="nexus-view-mode"><legend className="sr-only">{t('Management mode')}</legend>
        <label className={!advanced ? 'is-selected' : ''}><input type="radio" name={modeId} checked={!advanced} onChange={() => changeMode(false)} />{t('Basic')}</label>
        <label className={advanced ? 'is-selected' : ''}><input type="radio" name={modeId} checked={advanced} onChange={() => changeMode(true)} />{t('Advanced')}</label>
      </fieldset>
    </div>
    {!advanced && <MyDevices />}
    <NexusNetwork advanced={advanced} showCompact={false} />
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
      aria-label={account ? t('User: {name}', { name: account.displayName }) : t('User: Not signed in')}
      aria-haspopup="dialog" aria-expanded={open} aria-controls={open ? panelId : undefined}
      onClick={() => onOpenChange(!open)}>
      <AccountAvatar small /><span className="nexus-account-trigger__label">{account ? t('User: {name}', { name: account.displayName }) : t('User')}</span><Icon name="chevron-down" size={13} />
    </button>
    {open && createPortal(<div ref={panelRef} id={panelId} className="nexus-account-menu" role="dialog" aria-labelledby={titleId}>
      <div className="nexus-account-menu__header">
        <h2 id={titleId}>{t('Nexus Account Manager')}</h2>
        <WorkspaceActionButton ref={closeRef} appearance="quiet" size="small" iconOnly icon="x" aria-label={t('Close account menu')} onClick={() => close(true)} />
      </div>
      <AccountSummary />
      <AccountAccess />
      <MyDevices />
      <button type="button" className="nexus-account-menu__link" onClick={() => { close(true); onOpenPage(); }}>
        <span>{t('Open Nexus')}</span><Icon name="chevron-right" size={16} />
      </button>
      {account && onSignOut && <WorkspaceActionButton className="nexus-account-menu__sign-out" appearance="quiet" onClick={onSignOut}>{t('Sign out')}</WorkspaceActionButton>}
    </div>, document.body)}
  </>;
}
