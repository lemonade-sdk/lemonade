import React, { useEffect, useId, useRef, useState } from 'react';
import { copyTextToClipboard } from '../clipboard';
import { useFocusTrap } from '../hooks/useFocusTrap';
import { useI18n } from '../i18n';
import { useNexusAccount, type NexusDevice as AccountDevice } from '../features/nexus/NexusAccountContext';
import {
  NEXUS_CONNECTION_LABELS, dismissNexusNotice, nexusDeviceConnection, nexusDeviceTags, nexusInferenceServers, nexusMeshPortUrl, nexusMeshSummary,
  nexusRequest, nexusServerUnavailable, refreshNexus, relayEndpoint, selectNexusInferenceServer, useNexusNetwork,
  type NexusConnection, type NexusDevice, type NexusMeshState, type NexusStatus,
} from '../features/nexus/nexus';
import { Icon } from './Icon';
import { WorkspaceActionButton } from './WorkspacePanels';

type Tone = 'good' | 'warn' | 'accent' | 'info' | 'muted' | 'faint';
const CONNECTION_TONES: Record<NexusConnection, Tone> = { p2p: 'good', relay: 'warn', connecting: 'accent', offline: 'muted' };
const STATE_TONES: Record<NexusMeshState, Tone> = { off: 'faint', locked: 'faint', connecting: 'accent', connected: 'good', offline: 'muted' };
const tagTone = (tag: string): Tone => tag === 'This device' || tag === 'Admin' ? 'accent'
  : tag === 'Server' || tag === 'Joined network' ? 'info' : tag === 'Member' ? 'warn' : 'muted';

function Dot({ tone }: { tone: Tone }) {
  return <span className={`nexus-mesh-dot nexus-mesh-dot--${tone}`} aria-hidden="true" />;
}

export function NexusPill({ label, tone, connection = false }: { label: string; tone: Tone; connection?: boolean }) {
  const { t } = useI18n();
  return <span className={`nexus-pill nexus-pill--${tone}${connection ? ' nexus-pill--connection' : ''}`}>{t(label)}</span>;
}

// The account's devices stand in for the live list while the mesh is locked or down: the
// server reports no path, tier or network for them then.
const placeholderDevice = (device: AccountDevice): NexusDevice => ({
  node_id: device.id, device_name: device.name, tunnel_ip: '', management_pubkey: '', permissions: [], can_remove: false,
  is_online: device.connected ? true : undefined,
});
export function nexusListedDevices(status: NexusStatus | null | undefined, accountDevices: readonly AccountDevice[]): readonly NexusDevice[] {
  return status?.mesh_up && !status.locked ? status.members : accountDevices.map(placeholderDevice);
}

// Every device of this identity's networks, in the order lemond lists them: its own network
// first, then the networks it joined with an invite.
export function NexusDeviceRows({ status, devices }: { status: NexusStatus | null | undefined; devices: readonly NexusDevice[] }) {
  const { t } = useI18n();
  if (!devices.length) return <p className="nexus-mesh__empty">{t('No devices listed yet.')}</p>;
  return <ul className="nexus-devices nexus-mesh-list">
    {devices.map(device => {
      const connection = nexusDeviceConnection(status, device);
      return <li key={`${device.group_id || 'home'}:${device.node_id}`} className="nexus-mesh-row">
        <span className="nexus-mesh-row__body">
          <span className="nexus-mesh-row__name">{device.device_name || t('Unnamed device')}</span>
          {device.tunnel_ip && <span className="nexus-mesh-row__sub">{device.tunnel_ip.split('/')[0]}</span>}
          <span className="nexus-mesh-row__pills">
            {nexusDeviceTags(status, device).map(tag => <NexusPill key={tag} label={tag} tone={tagTone(tag)} />)}
            {connection && <NexusPill label={NEXUS_CONNECTION_LABELS[connection]} tone={CONNECTION_TONES[connection]} connection />}
          </span>
        </span>
      </li>;
    })}
  </ul>;
}

// The row is one button that selects the server; the info control beside it must not nest
// inside it, so both sit in a wrapper.
function ServerChoice({ selected, title, subtitle, connection, disabled, onChoose, onInfo }: {
  selected: boolean; title: string; subtitle?: string; connection?: NexusConnection; disabled: boolean; onChoose: () => void; onInfo: () => void;
}) {
  const { t } = useI18n();
  return <div className="nexus-mesh-choice">
    <button type="button" className={`nexus-mesh-row nexus-mesh-row--choice${selected ? ' is-selected' : ''}`} aria-pressed={selected} disabled={disabled} onClick={onChoose}>
      <span className="nexus-mesh-row__body">
        <span className="nexus-mesh-row__name">{title}</span>
        {subtitle && <span className="nexus-mesh-row__sub">{subtitle}</span>}
      </span>
      {connection && <NexusPill label={NEXUS_CONNECTION_LABELS[connection]} tone={CONNECTION_TONES[connection]} connection />}
      {selected && <Icon name="check" size={16} className="nexus-mesh-row__check" />}
    </button>
    <WorkspaceActionButton iconOnly icon="info" appearance="quiet" aria-label={t('How to use {name} from other apps', { name: title })} onClick={onInfo} />
  </div>;
}

function Endpoint({ label, value, copied, onCopy }: { label: string; value: string; copied: boolean; onCopy: () => void }) {
  const { t } = useI18n();
  return <div className="nexus-endpoint">
    <code>{value}</code>
    <WorkspaceActionButton size="small" appearance="quiet" icon={copied ? 'check' : 'copy'} aria-label={t('Copy {label}', { label })} onClick={onCopy}>{copied ? t('Copied') : t('Copy')}</WorkspaceActionButton>
  </div>;
}

// How other apps (pi, curl, any OpenAI-compatible client) reach one inference server:
// its mesh-port hostname on this computer, and the local API path that needs none.
// `nodeId` is `local` for this device's own server.
function ServerInfoDialog({ status, nodeId, onClose }: { status: NexusStatus; nodeId: string; onClose: () => void }) {
  const { t } = useI18n();
  const ref = useRef<HTMLDivElement>(null);
  const id = useId();
  const [copied, setCopied] = useState<string>();
  const [error, setError] = useState('');
  useFocusTrap(ref, true);
  useEffect(() => {
    const escape = (event: KeyboardEvent) => { if (event.key === 'Escape') onClose(); };
    document.addEventListener('keydown', escape);
    return () => document.removeEventListener('keydown', escape);
  }, [onClose]);
  const self = nodeId === 'local';
  const device = self ? undefined : status.members.find(member => member.node_id === nodeId);
  const serverId = self ? status.node_id || '' : nodeId;
  const name = self ? t('This device') : device?.device_name || t('Unnamed device');
  const meshUrl = nexusMeshPortUrl(status, serverId);
  const localBase = status.controller_url ? `${self ? status.controller_url : relayEndpoint(status.controller_url, serverId)}/v1` : undefined;
  const base = meshUrl ? `${meshUrl}/v1` : localBase;
  const connection = device && nexusDeviceConnection(status, device);
  const address = (self ? status.tunnel_ip : device?.tunnel_ip)?.split('/')[0];
  const copy = (value: string) => void copyTextToClipboard(value)
    .then(() => { setError(''); setCopied(value); window.setTimeout(() => setCopied(current => current === value ? undefined : current), 1200); })
    .catch(() => setError(t('Could not copy. Select the address and copy it manually.')));
  const example = base && `curl ${base}/chat/completions \\\n  -H 'Content-Type: application/json' \\\n  -d '{"model":"<model id>","messages":[{"role":"user","content":"Hello"}]}'`;
  return <div className="nexus-account-dialog-backdrop" onPointerDown={event => { if (event.target === event.currentTarget) onClose(); }}>
    <div ref={ref} className="nexus-account-dialog nexus-server-info" role="dialog" aria-modal="true" aria-labelledby={`${id}-title`}>
      <div className="nexus-account-dialog__header"><div><h2 id={`${id}-title`}>{name}</h2><p>{t('Use this server from pi, curl or any OpenAI-compatible client.')}</p></div><WorkspaceActionButton iconOnly icon="x" size="small" appearance="quiet" aria-label={t('Close server details')} onClick={onClose} /></div>
      <div className="nexus-server-info__section">
        <p className="nexus-mesh__label">{t('Hostname (mesh port on this computer)')}</p>
        {meshUrl ? <>
          <Endpoint label={t('hostname')} value={meshUrl} copied={copied === meshUrl} onCopy={() => copy(meshUrl)} />
          <p className="nexus-hint">{t('This name resolves to 127.0.0.1. Lemonade on this computer carries the request over the mesh to {name}.', { name })}</p>
        </> : <p className="nexus-hint">{t('The mesh port is not running on this computer, so this server has no hostname here.')}</p>}
      </div>
      {meshUrl && <div className="nexus-server-info__section">
        <p className="nexus-mesh__label">{t('OpenAI-compatible base URL')}</p>
        <Endpoint label={t('base URL')} value={`${meshUrl}/v1`} copied={copied === `${meshUrl}/v1`} onCopy={() => copy(`${meshUrl}/v1`)} />
        <p className="nexus-mesh__label">{t('Models')}</p>
        <Endpoint label={t('models URL')} value={`${meshUrl}/v1/models`} copied={copied === `${meshUrl}/v1/models`} onCopy={() => copy(`${meshUrl}/v1/models`)} />
      </div>}
      {localBase && <div className="nexus-server-info__section">
        <p className="nexus-mesh__label">{t('Through the local API (no hostname needed)')}</p>
        <Endpoint label={t('local API URL')} value={localBase} copied={copied === localBase} onCopy={() => copy(localBase)} />
      </div>}
      <div className="nexus-server-info__section">
        <p className="nexus-mesh__label">{t('Details')}</p>
        <dl>
          <dt>{t('Node ID')}</dt><dd>{serverId || '—'}</dd>
          <dt>{t('Mesh address')}</dt><dd>{address || '—'}</dd>
          <dt>{t('Path')}</dt><dd>{self ? t('This device') : connection ? t(NEXUS_CONNECTION_LABELS[connection]) : '—'}</dd>
        </dl>
      </div>
      <div className="nexus-server-info__section">
        <p className="nexus-mesh__label">{t('API key')}</p>
        <p className="nexus-hint">{t('No API key is needed unless that computer sets LEMONADE_API_KEY. Then send that key as the bearer token.')}</p>
      </div>
      {example && <div className="nexus-server-info__section">
        <p className="nexus-mesh__label">{t('Example')}</p>
        <pre><code>{example}</code></pre>
        <p className="nexus-hint">{t('Replace <model id> with a name from the models URL.')}</p>
        <WorkspaceActionButton size="small" appearance="quiet" icon={copied === example ? 'check' : 'copy'} onClick={() => copy(example)}>{copied === example ? t('Copied') : t('Copy example')}</WorkspaceActionButton>
      </div>}
      {error && <p className="nexus-error" role="alert">{error}</p>}
      <div className="nexus-group-actions"><WorkspaceActionButton appearance="quiet" onClick={onClose}>{t('Close')}</WorkspaceActionButton></div>
    </div>
  </div>;
}

// Settings → Devices & mesh: the mesh, the server that serves chat, every device of this
// identity's networks, the networks it joined, invites and joining another network.
export function NexusMeshPanel() {
  const { t } = useI18n();
  const network = useNexusNetwork();
  const { status } = network;
  const { account, devices, onConnectDevice, onJoinNetwork, memberships, onLeaveNetwork } = useNexusAccount();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const [key, setKey] = useState('');
  const [leaving, setLeaving] = useState<string>();
  // The server whose "use from other apps" details are open: `local` or a node id.
  const [info, setInfo] = useState<string>();
  const id = useId();
  const summary = nexusMeshSummary(status, network.error);
  const connected = Boolean(status?.mesh_up && !status.locked);
  const servers = nexusInferenceServers(status);
  const selected = network.selectedServerId || 'local';
  const choose = async (target: string, peerKey?: string) => {
    if (busy) return;
    setBusy(true); setError('');
    try {
      await selectNexusInferenceServer(target, peerKey);
      if (peerKey !== undefined) setKey('');
    } catch (error) { setError(error instanceof Error ? error.message : String(error)); }
    finally { setBusy(false); }
  };
  const leave = async (groupId: string) => {
    if (busy || !onLeaveNetwork) return;
    setBusy(true); setError('');
    try { await onLeaveNetwork(groupId); setLeaving(undefined); }
    catch (error) { setError(error instanceof Error ? error.message : String(error)); }
    finally { setBusy(false); }
  };
  // The servers serve a network founded again: the account here belongs to the earlier
  // one. Forgetting it clears the local account and floor; the user then joins again.
  const forget = async () => {
    if (busy) return;
    setBusy(true); setError('');
    try { await nexusRequest('forget', {}); await refreshNexus(); }
    catch (error) { setError(error instanceof Error ? error.message : String(error)); }
    finally { setBusy(false); }
  };
  return <section className="nexus-mesh" aria-labelledby={id}>
    <div className="nexus-mesh__header">
      <strong id={id} className="nexus-mesh__title">{t('Mesh')}</strong>
      <span className="nexus-mesh__state" role="status">
        <Dot tone={STATE_TONES[summary.state]} />
        {t(summary.label, summary.count === undefined ? undefined : { count: summary.count })}
      </span>
    </div>
    {network.error && <div className="nexus-mesh__problem" role="alert">
      <p className="nexus-error">{network.error}</p>
      <WorkspaceActionButton size="small" onClick={() => void refreshNexus()}>{t('Retry')}</WorkspaceActionButton>
    </div>}
    {status?.needs_rejoin && !network.error && <div className="nexus-mesh__problem" role="alert">
      <p className="nexus-error">{t('This network was founded again, so this device\'s account belongs to the earlier one. Forget it, then join again with a new invitation.')}</p>
      <WorkspaceActionButton size="small" appearance="danger" disabled={busy} onClick={() => void forget()}>{t('Forget this network')}</WorkspaceActionButton>
      {error && <p className="nexus-error" role="alert">{error}</p>}
    </div>}
    {status?.error && !network.error && !status.needs_rejoin && (status.error_retryable
      ? <p className="nexus-hint" role="status">{t('The network is temporarily unavailable. Reconnecting…')}</p>
      : <p className="nexus-error" role="alert">{status.error}</p>)}
    {network.notice && <p className="nexus-tree-notice" role="status">{network.notice} <button type="button" className="nexus-text-button" onClick={dismissNexusNotice}>{t('Dismiss')}</button></p>}
    {connected && status ? <>
      <div className="nexus-mesh__section">
        <p className="nexus-mesh__label">{t('Chat runs on')}</p>
        <ServerChoice selected={selected === 'local'} title={t('This device')} subtitle={t('Models on this computer')} disabled={busy} onChoose={() => void choose('local')} onInfo={() => setInfo('local')} />
        {servers.map(device => {
          // A server without a path cannot serve chat yet; its pill says why (Offline or Connecting).
          const unavailable = nexusServerUnavailable(status, device);
          return <ServerChoice key={device.node_id} selected={selected === device.node_id} title={device.device_name || t('Unnamed device')}
            subtitle={device.tunnel_ip.split('/')[0]} connection={unavailable ?? nexusDeviceConnection(status, device)} disabled={busy || Boolean(unavailable)}
            onChoose={() => void choose(device.node_id)} onInfo={() => setInfo(device.node_id)} />;
        })}
        {info && <ServerInfoDialog status={status} nodeId={info} onClose={() => setInfo(undefined)} />}
        {!servers.length && <p className="nexus-mesh__empty">{t('No other Lemonade servers in this mesh yet.')}</p>}
        {selected !== 'local' && <form className="nexus-mesh__key" onSubmit={event => { event.preventDefault(); void choose(selected, key); }}>
          <label className="sr-only" htmlFor={`${id}-key`}>{t('API key (only if that computer sets one)')}</label>
          <input id={`${id}-key`} className="input" type="password" autoComplete="off" placeholder={t('API key (only if that computer sets one)')}
            value={key} disabled={busy} onChange={event => setKey(event.target.value)} />
          <WorkspaceActionButton type="submit" size="small" disabled={busy}>{t('Save')}</WorkspaceActionButton>
        </form>}
        {error && <p className="nexus-error" role="alert">{error}</p>}
      </div>
      <div className="nexus-mesh__section">
        <p className="nexus-mesh__label">{t('Devices')}</p>
        <NexusDeviceRows status={status} devices={status.members} />
      </div>
      {memberships?.some(group => group.role === 'member') && <div className="nexus-mesh__section">
        <p className="nexus-mesh__label">{t('Networks')}</p>
        <ul className="nexus-mesh-list">
          {memberships.map(group => <li key={group.node_id} className="nexus-mesh-row">
            <span className="nexus-mesh-row__body">
              <span className="nexus-mesh-row__name">{group.hostname || t('Unnamed network')}</span>
              <span className="nexus-mesh-row__pills"><NexusPill label={group.role === 'home' ? 'Home' : 'Joined network'} tone={group.role === 'home' ? 'accent' : 'info'} /></span>
            </span>
            {group.role === 'member' && onLeaveNetwork && (leaving === group.node_id
              ? <span className="nexus-mesh-row__actions">
                <WorkspaceActionButton size="small" appearance="danger" disabled={busy} onClick={() => void leave(group.node_id)}>{t('Leave')}</WorkspaceActionButton>
                <WorkspaceActionButton size="small" appearance="quiet" disabled={busy} onClick={() => setLeaving(undefined)}>{t('Cancel')}</WorkspaceActionButton>
              </span>
              : <WorkspaceActionButton size="small" appearance="quiet" disabled={busy} onClick={() => { setError(''); setLeaving(group.node_id); }}>{t('Leave network')}</WorkspaceActionButton>)}
          </li>)}
        </ul>
      </div>}
    </> : account && <div className="nexus-mesh__section">
      <p className="nexus-mesh__label">{t('Devices')}</p>
      <NexusDeviceRows status={status} devices={nexusListedDevices(status, devices)} />
    </div>}
    {(onConnectDevice || onJoinNetwork) && <div className="nexus-mesh__actions">
      {onConnectDevice && <WorkspaceActionButton icon="plus" onClick={onConnectDevice}>{t('Connect another device')}</WorkspaceActionButton>}
      {onJoinNetwork && <WorkspaceActionButton appearance="quiet" icon="plug" onClick={onJoinNetwork}>{t('Join another network')}</WorkspaceActionButton>}
    </div>}
  </section>;
}
