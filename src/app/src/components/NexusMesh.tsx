import React, { useEffect, useId, useRef, useState } from 'react';
import { friendlyErrorMessage } from '../api';
import { copyTextToClipboard } from '../clipboard';
import { useFocusTrap } from '../hooks/useFocusTrap';
import { useI18n } from '../i18n';
import { useNexusAccount, type NexusDevice as AccountDevice } from '../features/nexus/NexusAccountContext';
import {
  NEXUS_CONNECTION_LABELS, NEXUS_LOCAL_HOST, NEXUS_MAPPING_PROTOCOL_LABELS, dismissNexusNotice, nexusDeviceConnection, nexusDeviceTags, nexusLemonadeServers, nexusMeshPortUrl, nexusMeshSummary,
  nexusRequest, nexusServerUnavailable, refreshNexus, relayEndpoint, selectNexusLemonadeServer, setNexusMeshUdpPort, useNexusNetwork,
  type NexusConnection, type NexusDevice, type NexusMeshState, type NexusStatus,
} from '../features/nexus/nexus';
import { WorkspaceActionButton } from './WorkspacePanels';

type Tone = 'good' | 'warn' | 'accent' | 'info' | 'muted' | 'faint';
const CONNECTION_TONES: Record<NexusConnection, Tone> = { p2p: 'good', relay: 'warn', connecting: 'accent', offline: 'muted' };
const STATE_TONES: Record<NexusMeshState, Tone> = { off: 'faint', locked: 'faint', connecting: 'accent', connected: 'good', offline: 'muted' };
const COPIED_FEEDBACK_MS = 1200;
const tagTone = (tag: string): Tone => tag === 'This device' || tag === 'Admin' ? 'accent'
  : tag === 'Server' || tag === 'Joined network' ? 'info' : tag === 'Member' ? 'warn' : 'muted';

function Dot({ tone }: { tone: Tone }) {
  return <span className={`nexus-mesh-dot nexus-mesh-dot--${tone}`} aria-hidden="true" />;
}

export function NexusPill({ label, tone, connection = false }: { label: string; tone: Tone; connection?: boolean }) {
  const { t } = useI18n();
  return <span className={`nexus-pill nexus-pill--${tone}${connection ? ' nexus-pill--connection' : ''}`}>{t(label)}</span>;
}

const placeholderDevice = (device: AccountDevice): NexusDevice => ({
  node_id: device.id, device_name: device.name, tunnel_ip: '', management_pubkey: '', permissions: [], can_remove: false,
  is_online: device.connected ? true : undefined,
});
export function nexusListedDevices(status: NexusStatus | null | undefined, accountDevices: readonly AccountDevice[]): readonly NexusDevice[] {
  return status?.mesh_up && !status.locked ? status.members : accountDevices.map(placeholderDevice);
}

// `selected` is NEXUS_LOCAL_HOST or a node id; `onChoose` and `onInfo` receive the same target form.
export interface NexusChatHost { selected: string; disabled: boolean; onChoose: (target: string) => void; onInfo?: (target: string) => void }

export function NexusDeviceRows({ status, devices, chat }: { status: NexusStatus | null | undefined; devices: readonly NexusDevice[]; chat?: NexusChatHost }) {
  const { t } = useI18n();
  if (!devices.length) return <p className="nexus-mesh__empty">{t('No devices listed yet.')}</p>;
  const isLocal = (device: NexusDevice) => Boolean(status && device.node_id === status.node_id);
  const ordered = [...devices].sort((a, b) => Number(isLocal(b)) - Number(isLocal(a)));
  const servers = new Set(nexusLemonadeServers(status).map(device => device.node_id));
  return <ul className="nexus-devices nexus-mesh-list">
    {ordered.map(device => {
      const local = isLocal(device);
      const name = device.device_name || t('Unnamed device');
      const host = chat && (local || servers.has(device.node_id)) ? chat : undefined;
      const unavailable = host && !local ? nexusServerUnavailable(status, device) : undefined;
      const connection = unavailable ?? nexusDeviceConnection(status, device);
      const target = local ? NEXUS_LOCAL_HOST : device.node_id;
      const selected = host?.selected === target;
      return <li key={`${device.group_id || 'home'}:${device.node_id}`} className={`nexus-mesh-row${selected ? ' is-selected' : ''}`}>
        <span className="nexus-mesh-row__body">
          <span className="nexus-mesh-row__name">{name}</span>
          {local && status?.mapped_endpoint && <span className="nexus-mesh-row__sub">
            {status.mapping_protocol ? t('Reachable through your router ({protocol})', { protocol: NEXUS_MAPPING_PROTOCOL_LABELS[status.mapping_protocol] }) : t('Reachable through your router')}
            {' '}<code className="nexus-mesh-row__endpoint">{status.mapped_endpoint}</code>
          </span>}
          {device.tunnel_ip && <span className="nexus-mesh-row__sub">{device.tunnel_ip.split('/')[0]}</span>}
          <span className="nexus-mesh-row__pills">
            {nexusDeviceTags(status, device).map(tag => <NexusPill key={tag} label={tag} tone={tagTone(tag)} />)}
            {connection && <NexusPill label={NEXUS_CONNECTION_LABELS[connection]} tone={CONNECTION_TONES[connection]} connection />}
          </span>
        </span>
        {host && <span className="nexus-mesh-row__actions">
          <WorkspaceActionButton size="small" appearance={selected ? 'secondary' : 'quiet'} icon={selected ? 'check' : undefined} className="nexus-chat-here"
            aria-pressed={selected} aria-label={t('Run chat on {name}', { name })} disabled={host.disabled || Boolean(unavailable)} onClick={() => host.onChoose(target)}>
            {selected ? t('Chat runs here') : t('Run chat here')}
          </WorkspaceActionButton>
          {host.onInfo && <WorkspaceActionButton iconOnly icon="info" size="small" appearance="quiet" aria-label={t('How to use {name} from other apps', { name })} onClick={() => host.onInfo?.(target)} />}
        </span>}
      </li>;
    })}
  </ul>;
}

export function NexusPortSetting({ status }: { status: NexusStatus }) {
  const { t } = useI18n();
  const id = useId();
  const configured = status.mesh_udp_port ?? 0;
  const [draft, setDraft] = useState(String(configured));
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  useEffect(() => { setDraft(String(configured)); }, [configured]);
  const inUse = status.mesh_up && !status.locked && status.dataplane_port ? status.dataplane_port : undefined;
  const save = async () => {
    if (busy) return;
    setBusy(true); setError('');
    try { await setNexusMeshUdpPort(Number(draft)); await refreshNexus(); }
    catch (error) { setError(friendlyErrorMessage(error)); }
    finally { setBusy(false); }
  };
  return <form className="nexus-port-setting" onSubmit={event => { event.preventDefault(); void save(); }}>
    <div className="nexus-port-setting__header">
      <label htmlFor={`${id}-port`}>{t('Mesh UDP port')}</label>
      {inUse && <span className="nexus-port-setting__in-use">{t('In use: {port}', { port: inUse })}</span>}
    </div>
    <div className="nexus-port-setting__row">
      <input id={`${id}-port`} className="input" type="text" inputMode="numeric" pattern="[0-9]*" required autoComplete="off"
        value={draft} disabled={busy} aria-describedby={`${id}-hint ${id}-error`} onChange={event => setDraft(event.target.value.replace(/\D/g, ''))} />
      <WorkspaceActionButton type="submit" size="small" disabled={busy || draft === String(configured)}>{t('Save')}</WorkspaceActionButton>
    </div>
    <p id={`${id}-hint`} className="nexus-hint">{t('0 lets the system pick a port. Set a fixed port when you forward one on your router.')}</p>
    <p id={`${id}-error`} className="nexus-error" role="alert">{error}</p>
  </form>;
}

function Endpoint({ label, value, copied, onCopy }: { label: string; value: string; copied: boolean; onCopy: () => void }) {
  const { t } = useI18n();
  return <div className="nexus-endpoint">
    <code>{value}</code>
    <WorkspaceActionButton size="small" appearance="quiet" icon={copied ? 'check' : 'copy'} aria-label={t('Copy {label}', { label })} onClick={onCopy}>{copied ? t('Copied') : t('Copy')}</WorkspaceActionButton>
  </div>;
}

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
  const self = nodeId === NEXUS_LOCAL_HOST;
  const device = self ? undefined : status.members.find(member => member.node_id === nodeId);
  const serverId = self ? status.node_id || '' : nodeId;
  const name = self ? t('This device') : device?.device_name || t('Unnamed device');
  const meshUrl = nexusMeshPortUrl(status, serverId);
  const localBase = status.controller_url ? `${self ? status.controller_url : relayEndpoint(status.controller_url, serverId)}/v1` : undefined;
  const base = meshUrl ? `${meshUrl}/v1` : localBase;
  const connection = device && nexusDeviceConnection(status, device);
  const address = (self ? status.tunnel_ip : device?.tunnel_ip)?.split('/')[0];
  const copy = (value: string) => void copyTextToClipboard(value)
    .then(() => { setError(''); setCopied(value); window.setTimeout(() => setCopied(current => current === value ? undefined : current), COPIED_FEEDBACK_MS); })
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

export function NexusMeshPanel() {
  const { t } = useI18n();
  const network = useNexusNetwork();
  const { status } = network;
  const { account, devices, onConnectDevice, onJoinNetwork, memberships, onLeaveNetwork } = useNexusAccount();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const [key, setKey] = useState('');
  const [leaving, setLeaving] = useState<string>();
  const [info, setInfo] = useState<string>();
  const id = useId();
  const summary = nexusMeshSummary(status, network.error);
  const connected = Boolean(status?.mesh_up && !status.locked);
  const selected = network.selectedServerId || NEXUS_LOCAL_HOST;
  const choose = async (target: string, peerKey?: string) => {
    if (busy) return;
    setBusy(true); setError('');
    try {
      await selectNexusLemonadeServer(target, peerKey);
      if (peerKey !== undefined) setKey('');
    } catch (error) { setError(friendlyErrorMessage(error)); }
    finally { setBusy(false); }
  };
  const leave = async (groupId: string) => {
    if (busy || !onLeaveNetwork) return;
    setBusy(true); setError('');
    try { await onLeaveNetwork(groupId); setLeaving(undefined); }
    catch (error) { setError(friendlyErrorMessage(error)); }
    finally { setBusy(false); }
  };
  const forget = async () => {
    if (busy) return;
    setBusy(true); setError('');
    try { await nexusRequest('forget', {}); await refreshNexus(); }
    catch (error) { setError(friendlyErrorMessage(error)); }
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
        <p className="nexus-mesh__label">{t('Devices')}</p>
        <NexusDeviceRows status={status} devices={status.members} chat={{ selected, disabled: busy, onChoose: target => void choose(target), onInfo: setInfo }} />
        {info && <ServerInfoDialog status={status} nodeId={info} onClose={() => setInfo(undefined)} />}
        {!nexusLemonadeServers(status).length && <p className="nexus-mesh__empty">{t('No other Lemonade servers in this mesh yet.')}</p>}
        {selected !== NEXUS_LOCAL_HOST && <form className="nexus-mesh__key" onSubmit={event => { event.preventDefault(); void choose(selected, key); }}>
          <label className="sr-only" htmlFor={`${id}-key`}>{t('API key (only if that computer sets one)')}</label>
          <input id={`${id}-key`} className="input" type="password" autoComplete="off" placeholder={t('API key (only if that computer sets one)')}
            value={key} disabled={busy} onChange={event => setKey(event.target.value)} />
          <WorkspaceActionButton type="submit" size="small" disabled={busy}>{t('Save')}</WorkspaceActionButton>
        </form>}
        {error && <p className="nexus-error" role="alert">{error}</p>}
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
