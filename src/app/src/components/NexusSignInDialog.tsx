import React, { useEffect, useId, useRef, useState } from 'react';
import { useFocusTrap } from '../hooks/useFocusTrap';
import { useI18n } from '../i18n';
import {
  nexusJoinNotice, validNexusInvitation,
  type NexusAccountInspection, type NexusAccountService, type NexusSignInInput, type VerifiedNexusAccount,
} from '../features/nexus/accountSession';
import { usesNexusDeviceKey, type NexusInvite } from '../features/nexus/nexus';
import { WorkspaceActionButton } from './WorkspacePanels';

export function NexusSignInDialog({ service, onClose, onSignedIn, passkeyActive }: {
  service: NexusAccountService; onClose: () => void; onSignedIn: (session: VerifiedNexusAccount) => void; passkeyActive: boolean;
}) {
  const { t } = useI18n();
  const ref = useRef<HTMLDivElement>(null);
  const id = useId();
  const [inspection, setInspection] = useState<NexusAccountInspection>();
  const [action, setAction] = useState<NexusSignInInput['action']>('create');
  const [deviceName, setDeviceName] = useState('');
  const [invitation, setInvitation] = useState('');
  const [setupCode, setSetupCode] = useState('');
  const [checking, setChecking] = useState(true);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const [joined, setJoined] = useState<{ session: VerifiedNexusAccount; message: string; error: boolean }>();
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
      setInspection(result);
      setAction(result.status.registered && result.status.has_network !== false ? 'return' : 'create');
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
  const available = inspection?.status.available && inspection.ready;
  const remote = Boolean(inspection && !inspection.local);
  const waitingForKey = Boolean(available && inspection.status.needs_user_key);
  const deviceKey = Boolean(available && (usesNexusDeviceKey(inspection.status) || !inspection.desktop));
  const retiresPasskey = Boolean(available && !inspection.desktop && !usesNexusDeviceKey(inspection.status));
  const nameTooLong = new TextEncoder().encode(deviceName.trim()).length > 128;
  const needsSetupCode = Boolean(inspection?.status.setup_code_required);
  const submit = async (event: React.FormEvent) => {
    event.preventDefault();
    if (busy || !available || waitingForKey || !deviceName.trim() || nameTooLong) return;
    setBusy(true); setError('');
    const revision = request.current;
    try {
      const session = await service.signIn({ action, deviceName, invitation,
        source: inspection.source, ...(needsSetupCode ? { setupCode } : {}) });
      if (revision !== request.current) return;
      if (action === 'join') {
        const notice = nexusJoinNotice((await service.inspect().catch(() => undefined))?.status.join_membership);
        if (revision !== request.current) return;
        if (notice) { setJoined({ session, ...notice }); return; }
      }
      onSignedIn(session);
    } catch (error) {
      if (revision !== request.current) return;
      setError(error instanceof Error ? error.message : String(error));
      // A headless server reports that it needs its setup code once Mesh is on: show the field.
      const fresh = await service.inspect().catch(() => undefined);
      if (fresh && revision === request.current && fresh.status.setup_code_required) setInspection(fresh);
    }
    finally { if (revision === request.current) setBusy(false); }
  };
  if (joined) {
    return <div className="nexus-account-dialog-backdrop"><div ref={ref} className="nexus-account-dialog" role="dialog" aria-modal="true" aria-labelledby={`${id}-title`}>
      <div className="nexus-account-dialog__header"><h2 id={`${id}-title`}>{t(joined.error ? 'Invite not accepted' : 'Joined as a member')}</h2></div>
      <p className={joined.error ? 'nexus-error' : undefined} role={joined.error ? 'alert' : 'status'}>{t(joined.message)}</p>
      <div className="nexus-group-actions"><WorkspaceActionButton onClick={() => onSignedIn(joined.session)}>{t('Done')}</WorkspaceActionButton></div>
    </div></div>;
  }
  return <div className="nexus-account-dialog-backdrop" aria-hidden={passkeyActive || undefined} onPointerDown={event => { if (event.target === event.currentTarget && !busy && !passkeyActive) onClose(); }}>
    <div ref={ref} className="nexus-account-dialog" role="dialog" aria-modal="true" aria-labelledby={`${id}-title`} aria-busy={busy || checking}>
      <div className="nexus-account-dialog__header"><h2 id={`${id}-title`}>{t('Enable Mesh')}</h2><WorkspaceActionButton iconOnly icon="x" size="small" appearance="quiet" aria-label={t('Close Enable Mesh')} disabled={busy || passkeyActive} onClick={onClose} /></div>
      {checking ? <p role="status">{t('Checking account access…')}</p> : <>
        {remote && inspection && <p className="nexus-server-target"><span>{t('Managing server')}</span><strong>{new URL(inspection.source).origin}</strong></p>}
        {inspection && !available && <p className="nexus-hint">{!inspection.status.available ? t('This server does not include Nexus account support.') : t('Set up this server through its local controller or SSH forwarding. After setup, manage it through the authenticated Nexus mesh.')}</p>}
        {waitingForKey && (inspection?.status.identity_source === 'user' ? <>
          <p role="status">{t('The server cannot open this device’s key yet. It tries again by itself.')}</p>
          {inspection.status.error && <p className="nexus-error" role="alert">{inspection.status.error}</p>}
        </> : <p role="status">{t('Waiting for your device key from the Lemonade tray.')}</p>)}
        {available && !waitingForKey && <form onSubmit={event => void submit(event)}>
          {retiresPasskey && <p className="nexus-hint">{t('This device registered a passkey, which only the Lemonade desktop app can verify. Enable Mesh here moves it to its device key.')}</p>}
          <fieldset className="nexus-sign-in__fields" disabled={busy}>
            <legend className="sr-only">{t('Account access')}</legend>
            <div className="nexus-sign-in__choices">
              {inspection.status.registered && inspection.status.has_network !== false && <label><input type="radio" name={id} checked={action === 'return'} onChange={() => { setAction('return'); setError(''); }} />{t('Use this device’s key')}</label>}
              {(!inspection.status.registered || inspection.status.has_network === false) && <label><input type="radio" name={id} checked={action === 'create'} onChange={() => { setAction('create'); setError(''); }} />{t('Create account')}</label>}
              <label><input type="radio" name={id} checked={action === 'join'} onChange={() => { setAction('join'); setError(''); }} />{t('Join by invitation')}</label>
            </div>
            <label htmlFor={`${id}-device`}>{t('Device name')}</label><input autoFocus id={`${id}-device`} className="input" value={deviceName} maxLength={128} onChange={event => setDeviceName(event.target.value)} />
            <p className="nexus-hint">{t('Other devices in the mesh see this computer by this name.')}</p>
            {action === 'join' && <><label htmlFor={`${id}-invite`}>{t('Invitation code or token')}</label><input id={`${id}-invite`} className="input" autoComplete="off" spellCheck={false} maxLength={68} value={invitation} onChange={event => setInvitation(event.target.value.trim())} /><p className="nexus-hint">{t(inspection.status.registered && inspection.status.has_network !== false ? 'This device keeps its own network and joins the invited one as a member.' : 'Create the invitation with Connect another device on a device that is already in the mesh.')}</p></>}
            {needsSetupCode && <><label htmlFor={`${id}-setup`}>{t('Setup code')}</label><input id={`${id}-setup`} className="input" autoComplete="off" spellCheck={false} maxLength={32} placeholder="XXXX-XXXX-XXXX" value={setupCode} onChange={event => setSetupCode(event.target.value)} /><p className="nexus-hint">{t('This server runs without a desktop, so it proves you own it with a one-time setup code. On the server: {command}', { command: `sudo cat ${inspection?.status.setup_code_file || 'the setup-code file'}` })}</p></>}
          </fieldset>
          {nameTooLong && <p className="nexus-error" role="alert">{t('This name is too long. Try a shorter name.')}</p>}
          <WorkspaceActionButton type="submit" disabled={busy || !deviceName.trim() || nameTooLong || (action === 'join' && !validNexusInvitation(invitation)) || (needsSetupCode && !setupCode.trim())}>{busy ? t('Enabling Mesh…') : t('Enable Mesh')}</WorkspaceActionButton>
          {!deviceKey && <p className="nexus-hint">{t('Enable Mesh asks for this device’s passkey.')}</p>}
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
  const [role, setRole] = useState<'user' | 'moderator'>('user');
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
  // Older servers return only the token; the 8-digit code is easier to read out, so it leads when present.
  const code = invite && /^[0-9]{8}$/.test(invite.code || '') ? invite.code! : '';
  const copy = (value: string) => void navigator.clipboard.writeText(value).catch(() => setError(t('Could not copy. Select the code and copy it manually.')));
  return <div className="nexus-account-dialog-backdrop"><div ref={ref} className="nexus-account-dialog" role="dialog" aria-modal="true" aria-labelledby={`${id}-title`}>
    <div className="nexus-account-dialog__header"><h2 id={`${id}-title`}>{t('Connect another device')}</h2><WorkspaceActionButton iconOnly icon="x" size="small" appearance="quiet" aria-label={t('Close invitation')} disabled={busy} onClick={onClose} /></div>
    {invite ? <><p>{t('On the other device, choose Join by invitation and enter this code or token.')}</p>
      {code ? <div className="nexus-account-invitation nexus-account-invitation--code" aria-label={t('Invitation code')}>{`${code.slice(0, 4)} ${code.slice(4)}`}</div>
        : <div className="nexus-account-invitation" aria-label={t('Invitation code or token')}>{invite.link_token}</div>}
      <p role="timer">{expires ? t('Expires in {minutes}:{seconds}', { minutes: Math.floor(expires / 60), seconds: String(expires % 60).padStart(2, '0') }) : t('This invitation has expired.')}</p>
      <WorkspaceActionButton ref={copyRef} disabled={!expires} onClick={() => copy(code || invite.link_token)}>{code ? t('Copy invitation code') : t('Copy invitation')}</WorkspaceActionButton>
      {code && invite.link_token && <WorkspaceActionButton appearance="quiet" size="small" disabled={!expires} onClick={() => copy(invite.link_token)}>{t('Copy full invite token')}</WorkspaceActionButton>}
      <p className="nexus-hint">{t('The invitation works once. Closing this dialog does not revoke it.')}</p></> : <form onSubmit={event => {
      event.preventDefault(); if (busy || !deviceName.trim()) return; setBusy(true); setError('');
      void service.invite(session, deviceName, role).then(setInvite).catch(error => setError(error instanceof Error ? error.message : String(error))).finally(() => setBusy(false));
    }}><label htmlFor={id}>{t('Device name')}</label><input autoFocus id={id} className="input" value={deviceName} disabled={busy} maxLength={128} onChange={event => setDeviceName(event.target.value)} placeholder={t('For example, my phone')} />
      <label htmlFor={`${id}-role`}>{t('Role')}</label>
      <select id={`${id}-role`} className="input" value={role} disabled={busy} onChange={event => setRole(event.target.value as 'user' | 'moderator')}><option value="user">{t('User')}</option><option value="moderator">{t('Moderator')}</option></select>
      <p className="nexus-hint">{role === 'user' ? t('Connect to the mesh and run inference.') : t('Run inference, use MCP tools, manage models and jobs, and invite devices.')}</p>
      <p className="nexus-hint">{t('An invitation grants at most your own permissions.')}</p>
      <WorkspaceActionButton type="submit" disabled={busy || !deviceName.trim()}>{busy ? t('Creating invitation…') : t('Create invitation')}</WorkspaceActionButton></form>}
    {error && <p className="nexus-error" role="alert">{t(error)}</p>}<div className="nexus-group-actions"><WorkspaceActionButton appearance="quiet" disabled={busy} onClick={onClose}>{t('Close')}</WorkspaceActionButton></div>
  </div></div>;
}

export function NexusJoinNetworkDialog({ service, session, onClose, onJoined }: {
  service: NexusAccountService; session: VerifiedNexusAccount; onClose: () => void; onJoined: () => void;
}) {
  const { t } = useI18n();
  const ref = useRef<HTMLDivElement>(null);
  const id = useId();
  const [invitation, setInvitation] = useState('');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const [result, setResult] = useState<{ message: string; error: boolean }>();
  useFocusTrap(ref, true);
  useEffect(() => {
    const escape = (event: KeyboardEvent) => { if (event.key === 'Escape' && !busy) onClose(); };
    document.addEventListener('keydown', escape);
    return () => document.removeEventListener('keydown', escape);
  }, [busy, onClose]);
  const submit = async (event: React.FormEvent) => {
    event.preventDefault();
    if (busy || !validNexusInvitation(invitation)) return;
    setBusy(true); setError('');
    try {
      const membership = await service.joinNetwork(session, invitation);
      setResult(nexusJoinNotice(membership) || { message: 'This device joined the invited network as a member and keeps its own network.', error: false });
      if (membership.status !== 'refused') onJoined();
    } catch (error) { setError(error instanceof Error ? error.message : String(error)); }
    finally { setBusy(false); }
  };
  return <div className="nexus-account-dialog-backdrop"><div ref={ref} className="nexus-account-dialog" role="dialog" aria-modal="true" aria-labelledby={`${id}-title`}>
    <div className="nexus-account-dialog__header"><h2 id={`${id}-title`}>{t('Join another network')}</h2><WorkspaceActionButton iconOnly icon="x" size="small" appearance="quiet" aria-label={t('Close Join another network')} disabled={busy} onClick={onClose} /></div>
    {result ? <>
      <p className={result.error ? 'nexus-error' : undefined} role={result.error ? 'alert' : 'status'}>{t(result.message)}</p>
      <div className="nexus-group-actions"><WorkspaceActionButton onClick={onClose}>{t('Done')}</WorkspaceActionButton></div>
    </> : <form onSubmit={event => void submit(event)}>
      <p className="nexus-hint">{t('This device keeps its own network and joins the invited one as a member.')}</p>
      <label htmlFor={`${id}-invite`}>{t('Invitation code or token')}</label>
      <input autoFocus id={`${id}-invite`} className="input" autoComplete="off" spellCheck={false} maxLength={68} value={invitation} disabled={busy} onChange={event => setInvitation(event.target.value.trim())} />
      {error && <p className="nexus-error" role="alert">{t(error)}</p>}
      <WorkspaceActionButton type="submit" disabled={busy || !validNexusInvitation(invitation)}>{busy ? t('Joining…') : t('Join network')}</WorkspaceActionButton>
      <div className="nexus-group-actions"><WorkspaceActionButton appearance="quiet" disabled={busy} onClick={onClose}>{t('Cancel')}</WorkspaceActionButton></div>
    </form>}
  </div></div>;
}

export function NexusDeleteAccountDialog({ service, session, onClose, onDeleted }: {
  service: NexusAccountService; session: VerifiedNexusAccount; onClose: () => void; onDeleted: () => void;
}) {
  const { t } = useI18n();
  const ref = useRef<HTMLDivElement>(null);
  const id = useId();
  const [confirmation, setConfirmation] = useState('');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const [pending, setPending] = useState(false);
  useFocusTrap(ref, true);
  useEffect(() => {
    const escape = (event: KeyboardEvent) => { if (event.key === 'Escape' && !busy) onClose(); };
    document.addEventListener('keydown', escape);
    return () => document.removeEventListener('keydown', escape);
  }, [busy, onClose]);
  const expected = session.account.deviceName.trim();
  const confirmed = confirmation.trim() === expected;
  const submit = async (event: React.FormEvent) => {
    event.preventDefault();
    if (busy || !confirmed) return;
    setBusy(true); setError('');
    try {
      const status = await service.deleteAccount(session);
      if (status === 'deleted') onDeleted();
      else setPending(true);
    } catch (error) { setError(error instanceof Error ? error.message : String(error)); }
    finally { setBusy(false); }
  };
  return <div className="nexus-account-dialog-backdrop"><div ref={ref} className="nexus-account-dialog" role="dialog" aria-modal="true" aria-labelledby={`${id}-title`}>
    <div className="nexus-account-dialog__header"><h2 id={`${id}-title`}>{t('Delete account')}</h2><WorkspaceActionButton iconOnly icon="x" size="small" appearance="quiet" aria-label={t('Close Delete account')} disabled={busy} onClick={onClose} /></div>
    {pending ? <>
      <p role="status">{t('The network has not confirmed the deletion yet. This device stays enabled until it does; check again later.')}</p>
      <div className="nexus-group-actions"><WorkspaceActionButton onClick={onClose}>{t('Close')}</WorkspaceActionButton></div>
    </> : <form onSubmit={event => void submit(event)}>
      <p>{t('This permanently deletes the account, its network and every device in it from Nexus. Devices that joined other networks lose that access. This cannot be undone.')}</p>
      <label htmlFor={`${id}-confirm`}>{t('To confirm, type this device’s name: {name}', { name: expected })}</label>
      <input autoFocus id={`${id}-confirm`} className="input" autoComplete="off" spellCheck={false} value={confirmation} disabled={busy} onChange={event => setConfirmation(event.target.value)} />
      {error && <p className="nexus-error" role="alert">{t(error)}</p>}
      <WorkspaceActionButton type="submit" appearance="danger" disabled={busy || !confirmed}>{busy ? t('Deleting…') : t('Delete account permanently')}</WorkspaceActionButton>
      <div className="nexus-group-actions"><WorkspaceActionButton appearance="quiet" disabled={busy} onClick={onClose}>{t('Cancel')}</WorkspaceActionButton></div>
    </form>}
  </div></div>;
}
