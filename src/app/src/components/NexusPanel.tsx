import React, { useEffect, useRef, useState } from 'react';
import { friendlyErrorMessage } from '../api';
import { useFocusTrap } from '../hooks/useFocusTrap';
import { Icon } from './Icon';
import { WorkspaceActionButton, WorkspaceActionGroup } from './WorkspacePanels';
import { isNexusDesktop, nexusRequest, nexusStatusLabel, prepareNexus, refreshNexus,
  cancelNexusPasskey, sendNexusPasskeyPin, unlockNexus, useNexusNetwork, type PasskeyPrompt, type NexusDevice, type NexusInvite } from '../features/nexus/nexus';
import '../styles/nexus.css';

interface Props { isActive: boolean; onConnect: (nodeId: string, apiKey: string) => Promise<void> }
const NexusPanel: React.FC<Props> = ({ isActive, onConnect }) => {
  const network = useNexusNetwork(isActive);
  const status = network.status;
  const desktop = isNexusDesktop();
  const [deviceName, setDeviceName] = useState('My Lemonade device');
  const [inviteName, setInviteName] = useState('');
  const [code, setCode] = useState('');
  const [remoteKey, setRemoteKey] = useState('');
  const [busy, setBusy] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [notice, setNotice] = useState<string | null>(null);
  const [dialog, setDialog] = useState<'invite' | 'join' | null>(null);
  const [invite, setInvite] = useState<NexusInvite | null>(null);
  const [removing, setRemoving] = useState<NexusDevice | null>(null);
  const [now, setNow] = useState(Date.now());
  const dialogRef = useRef<HTMLDivElement>(null);
  const returnFocus = useRef<HTMLElement | null>(null);
  useFocusTrap(dialogRef, Boolean((dialog || removing) && !network.passkey));
  useEffect(() => { if (status?.device_name) setDeviceName(status.device_name); }, [status?.device_name]);
  useEffect(() => {
    if (!invite) return;
    const timer = window.setInterval(() => setNow(Date.now()), 1000);
    return () => window.clearInterval(timer);
  }, [invite]);
  useEffect(() => {
    if (!dialog && !removing) { returnFocus.current?.focus(); return; }
    const escape = (event: KeyboardEvent) => { if (event.key === 'Escape' && !busy) closeDialog(); };
    document.addEventListener('keydown', escape);
    return () => document.removeEventListener('keydown', escape);
  }, [dialog, removing, busy]);
  const run = async (label: string, action: () => Promise<void>) => {
    setBusy(label); setError(null); setNotice(null);
    try { await action(); await refreshNexus(); }
    catch (err) { setError(friendlyErrorMessage(err)); }
    finally { setBusy(null); }
  };
  const rememberFocus = () => { returnFocus.current = document.activeElement as HTMLElement; };
  const closeDialog = () => { setDialog(null); setRemoving(null); setInvite(null); };
  const createInvite = () => run('Confirming passkey…', async () => {
    await unlockNexus();
    const result = await nexusRequest<NexusInvite>('invites', { device_name: inviteName.trim(), ttl_sec: 600 });
    if (!result.link_token) throw new Error('The network did not return an invite token.');
    setNow(Date.now()); setInvite(result);
  });
  const join = () => run('Joining your group…', async () => {
    await prepareNexus(deviceName.trim(), true);
    await nexusRequest('join', { code });
    closeDialog(); setNotice('Joined your group. Choose a device below to connect to its server.');
  });
  const connect = (device: NexusDevice) => run(`Connecting to ${device.device_name}…`, async () => {
    await onConnect(device.node_id, remoteKey);
    setRemoteKey('');
  });
  const seconds = invite ? Math.max(0, Math.ceil((invite.expires_at * 1000 - now) / 1000)) : 0;
  const members = status?.members || [];
  const self = members.find(member => member.node_id === status?.node_id);
  const owner = self?.permissions.includes('delete_node');
  return <section className="nexus-panel" aria-label="Devices and mesh">
    <div className="nexus-panel__overview">
      <div className="nexus-panel__identity">
        <span className={`nexus-panel__signal${status?.mesh_up ? ' is-online' : ''}`}><Icon name="router" size={24} /></span>
        <div><h2>Your Lemonade network</h2><p role="status">{nexusStatusLabel(status)}</p></div>
      </div>
      <p className="nexus-panel__intro">Link your devices to reach a Lemonade server from anywhere. Pair with a passkey and an 8-digit code.</p>
      {!desktop && <p className="form-field__hint">Open the Lemonade desktop app to add devices or manage your network.</p>}
      {status && !status.available && <p className="connect__error">This installation does not include the network service. Install a build with Nexus support.</p>}
      <dl className="nexus-panel__facts">
        <div><dt>This device</dt><dd>{status?.device_name || deviceName}</dd></div>
        <div><dt>Mesh address</dt><dd>{status?.tunnel_ip || 'Not connected'}</dd></div>
        <div><dt>Group members</dt><dd>{members.length}</dd></div>
        <div><dt>Connected peers</dt><dd>{status?.peer_count ?? 0}</dd></div>
      </dl>
      {status?.group_node_id && <details className="nexus-panel__details"><summary>Network details</summary>
        <dl><dt>Group ID</dt><dd>{status.group_node_id}</dd><dt>Device ID</dt><dd>{status.node_id}</dd><dt>Role</dt><dd>{owner ? 'Group owner' : 'Member'}</dd></dl>
      </details>}
      {desktop && <>
        <div className="form-field nexus-panel__name"><label className="form-field__label" htmlFor="nexus-device-name">Device name</label>
          <input id="nexus-device-name" className="input" value={deviceName} maxLength={128} onChange={event => setDeviceName(event.target.value)} />
          {status?.enabled && <WorkspaceActionButton appearance="quiet" disabled={Boolean(busy) || !deviceName.trim()} onClick={() => void run('Saving name…', async () => { await nexusRequest('device', { device_name: deviceName.trim() }); })}>Save name</WorkspaceActionButton>}
        </div>
        <WorkspaceActionGroup>
          {!status?.enabled && <WorkspaceActionButton appearance="primary" icon="router" disabled={!status?.available || Boolean(busy) || !deviceName.trim()} onClick={() => void run('Creating your network…', async () => { await prepareNexus(deviceName.trim(), false); setNotice('Your group is ready. Invite another device to get started.'); })}>Create a group</WorkspaceActionButton>}
          {status?.enabled && status.locked && <WorkspaceActionButton appearance="primary" icon="eye-off" disabled={Boolean(busy)} onClick={() => void run('Unlocking…', async () => { await prepareNexus(deviceName.trim(), false); })}>Unlock with passkey</WorkspaceActionButton>}
          {status?.mesh_up && <WorkspaceActionButton appearance="primary" icon="plus" disabled={Boolean(busy) || !self?.permissions.includes('add_child')} onClick={() => { rememberFocus(); setInviteName(''); setInvite(null); setDialog('invite'); }}>Add a device</WorkspaceActionButton>}
          {!status?.mesh_up && <WorkspaceActionButton appearance="secondary" disabled={!status?.available || Boolean(busy)} onClick={() => { rememberFocus(); setCode(''); setDialog('join'); }}>Join a group</WorkspaceActionButton>}
          {status?.enabled && !status.locked && <WorkspaceActionButton appearance="quiet" disabled={Boolean(busy)} onClick={() => void run('Locking…', async () => { await nexusRequest('lock', {}); })}>Lock network</WorkspaceActionButton>}
          {status?.enabled && <WorkspaceActionButton appearance="quiet" disabled={Boolean(busy)} onClick={() => void run('Turning mesh off…', async () => { await nexusRequest('disable', {}); })}>Turn mesh off</WorkspaceActionButton>}
        </WorkspaceActionGroup>
      </>}
    </div>
    {(error || network.error || status?.error) && <p className="connect__error" role="alert">{error || network.error || status?.error}</p>}
    {(notice || busy) && <p className="nexus-panel__notice" role="status">{busy || notice}</p>}
    <div className="nexus-panel__members">
      <div className="nexus-panel__members-heading"><h3>Devices in your group</h3><WorkspaceActionButton appearance="quiet" icon="rotate-ccw" disabled={Boolean(busy)} onClick={() => void refreshNexus()}>Refresh</WorkspaceActionButton></div>
      {!members.length && <p className="nexus-panel__empty">{status?.locked && status.enabled ? 'Unlock to see your group’s devices.' : 'Create a group or enter an invite code to link your first device.'}</p>}
      <ul className="nexus-panel__device-list">{members.map(device => <li key={device.node_id}>
        <Icon name="hard-drive" size={24} />
        <div className="nexus-panel__device"><strong>{device.device_name || 'Unnamed device'}{device.node_id === status?.node_id && <span className="nexus-panel__self">This device</span>}</strong><span>{device.tunnel_ip || 'No mesh address'}{device.is_online !== undefined && ` · ${device.is_online ? 'Online' : 'Offline'}`}{Boolean(device.is_online && device.latency_ms) && ` · ${Math.round(device.latency_ms!)} ms`}</span></div>
        {desktop && device.node_id !== status?.node_id && <WorkspaceActionGroup>
          <WorkspaceActionButton appearance="secondary" size="small" icon="plug" disabled={Boolean(busy) || !status?.mesh_up} onClick={() => void connect(device)}>Connect to server</WorkspaceActionButton>
          {device.can_remove && <WorkspaceActionButton appearance="danger" size="small" icon="trash" disabled={Boolean(busy)} onClick={() => { rememberFocus(); setRemoving(device); }} aria-label={`Remove ${device.device_name}`}>Remove</WorkspaceActionButton>}
        </WorkspaceActionGroup>}
      </li>)}</ul>
      {desktop && members.some(member => member.node_id !== status?.node_id) && <div className="form-field nexus-panel__remote-key"><label className="form-field__label" htmlFor="nexus-remote-key">Remote server API key (if required)</label><input id="nexus-remote-key" className="input" type="password" autoComplete="off" value={remoteKey} onChange={event => setRemoteKey(event.target.value)} /><span className="form-field__hint">Use the key configured on the device you connect to.</span></div>}
    </div>
    {network.passkey && <PasskeyDialog prompt={network.passkey} />}
    {(dialog || removing) && <div className="nexus-dialog__backdrop" onClick={event => { if (event.target === event.currentTarget && !busy) closeDialog(); }}>
      <div ref={dialogRef} className="nexus-dialog" role="dialog" aria-modal="true" aria-labelledby="nexus-dialog-title">
        <div className="nexus-dialog__heading"><h2 id="nexus-dialog-title">{removing ? 'Remove a device' : dialog === 'join' ? 'Join a group' : 'Invite a device'}</h2><WorkspaceActionButton appearance="quiet" icon="x" aria-label="Close dialog" disabled={Boolean(busy)} onClick={closeDialog} /></div>
        {error && <p className="connect__error" role="alert">{error}</p>}
        {busy && <p role="status">{busy}</p>}
        {removing ? <><p>Remove {removing.device_name} from your group? Its network access will end.</p><WorkspaceActionButton appearance="danger" disabled={Boolean(busy)} onClick={() => void run('Removing device…', async () => { await nexusRequest(`devices/${encodeURIComponent(removing.node_id)}/remove`, {}); closeDialog(); setNotice('Device removed from your group.'); })}>Remove device</WorkspaceActionButton></>
        : dialog === 'join' ? <form onSubmit={event => { event.preventDefault(); void join(); }}>
          <p>Ask a group owner to add this device, then enter their pairing code or invite token.</p><label className="form-field__label" htmlFor="nexus-join-code">Pairing code or invite token</label>
          <input id="nexus-join-code" className={`input nexus-dialog__code-input${code.length > 8 ? ' is-token' : ''}`} autoComplete="off" maxLength={68} value={code} onChange={event => setCode(event.target.value.trim())} />
          <WorkspaceActionButton type="submit" appearance="primary" disabled={Boolean(busy) || !(/^(?:[0-9]{8}|lnk_[0-9a-f]{64})$/.test(code)) || !deviceName.trim()}>Confirm with passkey and join</WorkspaceActionButton>
        </form> : invite ? <>
          <p>Give this invite to {invite.device_name}.</p>{invite.code && <div className="nexus-dialog__code" aria-label={`Pairing code ${invite.code}`}>{invite.code.slice(0, 4)} {invite.code.slice(4)}</div>}
          {!invite.code && <p>This federation uses full invite tokens. Copy the token and paste it on the other device.</p>}
          <p role="timer">{seconds ? `Expires in ${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}` : 'This code has expired.'}</p>
          <WorkspaceActionGroup>{invite.code && <WorkspaceActionButton appearance="primary" disabled={!seconds} onClick={() => void run('Copying…', async () => { await navigator.clipboard.writeText(invite.code!); setNotice('Pairing code copied.'); })}>Copy code</WorkspaceActionButton>}<WorkspaceActionButton appearance="quiet" disabled={!seconds} onClick={() => void run('Copying…', async () => { await navigator.clipboard.writeText(invite.link_token); setNotice('Full invite token copied.'); })}>Copy full token</WorkspaceActionButton></WorkspaceActionGroup>
          <p className="form-field__hint">The code works once. Closing this dialog does not revoke it; it expires after 10 minutes.</p>
          <WorkspaceActionButton appearance="quiet" disabled={Boolean(busy)} onClick={() => void run('Closing invite…', async () => { await nexusRequest('invites/cancel', {}); closeDialog(); })}>Close invite</WorkspaceActionButton>
        </> : <form onSubmit={event => { event.preventDefault(); void createInvite(); }}>
          <label className="form-field__label" htmlFor="nexus-invite-name">Device you’re inviting</label><input className="input" id="nexus-invite-name" value={inviteName} maxLength={128} placeholder="Alex’s laptop" onChange={event => setInviteName(event.target.value)} />
          {inviteName.trim() && <p>Would you like “{inviteName.trim()}” to join your group?</p>}
          <WorkspaceActionButton type="submit" appearance="primary" disabled={Boolean(busy) || !inviteName.trim()}>Confirm with passkey</WorkspaceActionButton>
        </form>}
      </div>
    </div>}
  </section>;
};
const PasskeyDialog: React.FC<{ prompt: PasskeyPrompt }> = ({ prompt }) => {
  const ref = useRef<HTMLDivElement>(null);
  const [pin, setPin] = useState('');
  const [error, setError] = useState('');
  useFocusTrap(ref, true);
  useEffect(() => {
    const escape = (event: KeyboardEvent) => { if (event.key === 'Escape') void cancelNexusPasskey(); };
    document.addEventListener('keydown', escape);
    return () => document.removeEventListener('keydown', escape);
  }, []);
  const sendPin = async () => {
    const value = pin; setPin(''); setError('');
    try { await sendNexusPasskeyPin(value); } catch (error) { setError(friendlyErrorMessage(error)); }
  };
  return <div className="nexus-dialog__backdrop nexus-passkey-backdrop"><div ref={ref} className="nexus-dialog" role="dialog" aria-modal="true" aria-labelledby="nexus-passkey-title">
    <h2 id="nexus-passkey-title">Verify with your security key</h2>
    <p>{prompt.rp_id}</p><p role="status">{prompt.message}</p>
    {error && <p role="alert" className="connect__error">{error}</p>}
    {prompt.needs_pin && <form onSubmit={event => { event.preventDefault(); void sendPin(); }}>
      <label htmlFor="nexus-security-pin" className="form-field__label">Security key PIN</label>
      <input autoFocus id="nexus-security-pin" className="input" type="password" autoComplete="off" minLength={4} maxLength={63} value={pin} onChange={event => setPin(event.target.value)} />
      <WorkspaceActionButton type="submit" appearance="primary" disabled={pin.length < 4}>Continue</WorkspaceActionButton>
    </form>}
    <WorkspaceActionButton appearance="quiet" onClick={() => void cancelNexusPasskey()}>Cancel verification</WorkspaceActionButton>
  </div></div>;
};
export default NexusPanel;
