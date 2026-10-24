import React, { useEffect, useId, useRef, useState } from 'react';
import { useFocusTrap } from '../hooks/useFocusTrap';
import { useI18n } from '../i18n';
import {
  loadNexusAccountLabel, validNexusInvitation,
  type NexusAccountInspection, type NexusAccountService, type NexusSignInInput, type SavedNexusAccount, type VerifiedNexusAccount,
} from '../features/nexus/accountSession';
import type { NexusInvite } from '../features/nexus/nexus';
import { Icon } from './Icon';
import { WorkspaceActionButton } from './WorkspacePanels';

export function NexusSignInDialog({ service, onClose, onSignedIn, passkeyActive }: {
  service: NexusAccountService; onClose: () => void; onSignedIn: (session: VerifiedNexusAccount) => void; passkeyActive: boolean;
}) {
  const { t } = useI18n();
  const ref = useRef<HTMLDivElement>(null);
  const id = useId();
  const [inspection, setInspection] = useState<NexusAccountInspection>();
  const [saved, setSaved] = useState<SavedNexusAccount>();
  const [action, setAction] = useState<NexusSignInInput['action']>('create');
  const [displayName, setDisplayName] = useState('');
  const [deviceName, setDeviceName] = useState('');
  const [invitation, setInvitation] = useState('');
  const [checking, setChecking] = useState(true);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const request = useRef(0);
  const errorRef = useRef<HTMLParagraphElement>(null);
  useFocusTrap(ref, !passkeyActive);
  useEffect(() => { if (!busy && !checking && error) errorRef.current?.focus(); }, [busy, checking, error]);
  const inspect = async () => {
    const revision = ++request.current;
    setChecking(true); setError('');
    try {
      const result = await service.inspect();
      if (revision !== request.current) return;
      const profile = loadNexusAccountLabel(result.source);
      setInspection(result); setSaved(profile);
      setAction(profile || result.status.registered ? 'return' : 'create');
      setDisplayName(profile?.displayName || '');
      setDeviceName(result.status.device_name || t('This computer'));
    } catch (error) { if (revision === request.current) setError(error instanceof TypeError ? t('Cannot reach the Lemonade server. Start it or check the connection in Settings.') : error instanceof Error ? error.message : String(error)); }
    finally { if (revision === request.current) setChecking(false); }
  };
  useEffect(() => { void inspect(); return () => { request.current += 1; }; }, [service]);
  useEffect(() => {
    const escape = (event: KeyboardEvent) => { if (event.key === 'Escape' && !busy && !passkeyActive) { event.preventDefault(); onClose(); } };
    document.addEventListener('keydown', escape);
    return () => document.removeEventListener('keydown', escape);
  }, [busy, passkeyActive, onClose]);
  const available = inspection?.status.available && inspection.local && inspection.desktop;
  const nameTooLong = [displayName, deviceName].some(value => new TextEncoder().encode(value.trim()).length > 128);
  const submit = async (event: React.FormEvent) => {
    event.preventDefault();
    if (busy || !available || !displayName.trim() || !deviceName.trim() || nameTooLong) return;
    setBusy(true); setError('');
    const revision = request.current;
    try {
      const session = await service.signIn({ action, displayName, deviceName, invitation,
        source: inspection.source, expectedId: saved?.id });
      if (revision === request.current) onSignedIn(session);
    } catch (error) { if (revision === request.current) setError(error instanceof Error ? error.message : String(error)); }
    finally { if (revision === request.current) setBusy(false); }
  };
  return <div className="nexus-account-dialog-backdrop" aria-hidden={passkeyActive || undefined} onPointerDown={event => { if (event.target === event.currentTarget && !busy && !passkeyActive) onClose(); }}>
    <div ref={ref} className="nexus-account-dialog" role="dialog" aria-modal="true" aria-labelledby={`${id}-title`} aria-busy={busy || checking}>
      <div className="nexus-account-dialog__header"><span className="nexus-tree-detail__icon"><Icon name="user-round" size={23} /></span><div><h2 id={`${id}-title`}>{t('Sign in to Lemonade')}</h2><p>{t('One account for your devices.')}</p></div><WorkspaceActionButton iconOnly icon="x" size="small" appearance="quiet" aria-label={t('Close sign-in')} disabled={busy || passkeyActive} onClick={onClose} /></div>
      {checking ? <p role="status">{t('Checking account access…')}</p> : <>
        {inspection && !available && <p className="nexus-hint">{!inspection.status.available ? t('This server does not include Nexus account support.') : t('Open the Lemonade desktop app on this computer to verify your passkey.')}</p>}
        {available && <form onSubmit={event => void submit(event)}>
          <p className="nexus-hint">{t('Nexus accounts use a device key and passkey. Your account name is a label saved on this app.')}</p>
          <fieldset className="nexus-sign-in__fields" disabled={busy}>
            <legend className="sr-only">{t('Account access')}</legend>
            <div className="nexus-sign-in__choices">
              <label><input type="radio" name={id} checked={action === 'return'} onChange={() => { setAction('return'); setError(''); }} />{t('Sign in')}</label>
              {!saved && <>{!inspection.status.registered && <label><input type="radio" name={id} checked={action === 'create'} onChange={() => { setAction('create'); setError(''); }} />{t('Create account')}</label>}<label><input type="radio" name={id} checked={action === 'join'} onChange={() => { setAction('join'); setError(''); }} />{t('Join by invitation')}</label></>}
            </div>
            <label htmlFor={`${id}-name`}>{t('Account name')}</label><input autoFocus id={`${id}-name`} className="input" autoComplete="nickname" value={displayName} maxLength={128} placeholder={t('Enter an account name')} onChange={event => setDisplayName(event.target.value)} />
            {action !== 'return' && <><label htmlFor={`${id}-device`}>{t('Device name')}</label><input id={`${id}-device`} className="input" value={deviceName} maxLength={128} onChange={event => setDeviceName(event.target.value)} /></>}
            {action === 'join' && <><label htmlFor={`${id}-invite`}>{t('Invitation code or token')}</label><input id={`${id}-invite`} className="input" autoComplete="off" spellCheck={false} maxLength={68} value={invitation} onChange={event => setInvitation(event.target.value.trim())} /><p className="nexus-hint">{t('Get an invitation from a device already linked to the account.')}</p></>}
            {action === 'create' && <p className="nexus-hint">{t('This device will create the account. Administrator access is confirmed by Nexus after joining.')}</p>}
          </fieldset>
          {nameTooLong && <p className="nexus-error" role="alert">{t('This name is too long. Try a shorter name.')}</p>}
          <WorkspaceActionButton type="submit" disabled={busy || !displayName.trim() || !deviceName.trim() || nameTooLong || (action === 'join' && !validNexusInvitation(invitation))}>{busy ? t('Signing in…') : action === 'create' ? t('Create with passkey') : action === 'join' ? t('Join with passkey') : t('Continue with passkey')}</WorkspaceActionButton>
          <p className="nexus-hint">{t('Use the passkey registered for this device. New devices will register a passkey first.')}</p>
        </form>}
        {error && <p ref={errorRef} className="nexus-error" role="alert" tabIndex={-1}>{t(error)}</p>}
        <div className="nexus-group-actions">{!busy && <WorkspaceActionButton size="small" appearance="quiet" icon="rotate-ccw" onClick={() => void inspect()}>{t('Check again')}</WorkspaceActionButton>}<WorkspaceActionButton appearance="quiet" disabled={busy || passkeyActive} onClick={onClose}>{t('Cancel')}</WorkspaceActionButton></div>
      </>}
    </div>
  </div>;
}

export function NexusAccountInviteDialog({ service, session, onClose }: {
  service: NexusAccountService; session: VerifiedNexusAccount; onClose: () => void;
}) {
  const { t } = useI18n();
  const ref = useRef<HTMLDivElement>(null);
  const id = useId();
  const [deviceName, setDeviceName] = useState('');
  const [invite, setInvite] = useState<NexusInvite>();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const [now, setNow] = useState(Date.now());
  const copyRef = useRef<HTMLButtonElement>(null);
  useFocusTrap(ref, true);
  useEffect(() => { if (invite) copyRef.current?.focus(); }, [invite]);
  useEffect(() => {
    const timer = window.setInterval(() => setNow(Date.now()), 1000);
    const escape = (event: KeyboardEvent) => { if (event.key === 'Escape' && !busy) onClose(); };
    document.addEventListener('keydown', escape);
    return () => { window.clearInterval(timer); document.removeEventListener('keydown', escape); };
  }, [busy, onClose]);
  const expires = invite ? Math.max(0, Math.min(600, Math.ceil(invite.expires_at - now / 1000))) : 0;
  return <div className="nexus-account-dialog-backdrop"><div ref={ref} className="nexus-account-dialog" role="dialog" aria-modal="true" aria-labelledby={`${id}-title`}>
    <div className="nexus-account-dialog__header"><h2 id={`${id}-title`}>{t('Connect another device')}</h2><WorkspaceActionButton iconOnly icon="x" size="small" appearance="quiet" aria-label={t('Close invitation')} disabled={busy} onClick={onClose} /></div>
    {invite ? <><p>{t('On the other device, choose Join by invitation and enter this code or token.')}</p><div className="nexus-account-invitation" aria-label={t('Invitation code or token')}>{invite.code || invite.link_token}</div><p role="timer">{expires ? t('Expires in {minutes}:{seconds}', { minutes: Math.floor(expires / 60), seconds: String(expires % 60).padStart(2, '0') }) : t('This invitation has expired.')}</p><WorkspaceActionButton ref={copyRef} disabled={!expires} onClick={() => {
      void navigator.clipboard.writeText(invite.code || invite.link_token).catch(() => setError(t('Could not copy. Select the code and copy it manually.')));
    }}>{t('Copy invitation')}</WorkspaceActionButton><p className="nexus-hint">{t('The invitation works once and expires after 10 minutes. Closing this dialog does not revoke it.')}</p></> : <form onSubmit={event => {
      event.preventDefault(); if (busy || !deviceName.trim()) return; setBusy(true); setError('');
      void service.invite(session, deviceName).then(setInvite).catch(error => setError(error instanceof Error ? error.message : String(error))).finally(() => setBusy(false));
    }}><label htmlFor={id}>{t('Device you are inviting')}</label><input autoFocus id={id} className="input" value={deviceName} maxLength={128} onChange={event => setDeviceName(event.target.value)} placeholder={t('For example, my phone')} /><WorkspaceActionButton type="submit" disabled={busy || !deviceName.trim()}>{busy ? t('Creating invitation…') : t('Create invitation')}</WorkspaceActionButton></form>}
    {error && <p className="nexus-error" role="alert">{t(error)}</p>}<div className="nexus-group-actions"><WorkspaceActionButton appearance="quiet" disabled={busy} onClick={onClose}>{t('Close')}</WorkspaceActionButton></div>
  </div></div>;
}
