import React, { useId, useState } from 'react';
import { useI18n } from '../i18n';
import { useNexusAccount, type NexusDevice as AccountDevice } from '../features/nexus/NexusAccountContext';
import {
  NEXUS_CONNECTION_LABELS, dismissNexusNotice, nexusDeviceConnection, nexusDeviceTags, nexusInferenceServers, nexusMeshSummary,
  refreshNexus, selectNexusInferenceServer, useNexusNetwork, type NexusConnection, type NexusDevice, type NexusMeshState, type NexusStatus,
} from '../features/nexus/nexus';
import { Icon } from './Icon';
import { WorkspaceActionButton } from './WorkspacePanels';

type Tone = 'good' | 'warn' | 'accent' | 'info' | 'muted' | 'faint';
const CONNECTION_TONES: Record<NexusConnection, Tone> = { p2p: 'good', relay: 'warn', connecting: 'accent', offline: 'muted' };
const STATE_TONES: Record<NexusMeshState, Tone> = { off: 'faint', locked: 'faint', connecting: 'accent', connected: 'good', offline: 'muted' };
const tagTone = (tag: string): Tone => tag === 'This device' || tag === 'Admin' ? 'accent'
  : tag === 'Server' || tag === 'Joined network' ? 'info' : tag === 'Member' ? 'warn' : 'muted';

function Dot({ tone, pulse = false }: { tone: Tone; pulse?: boolean }) {
  return <span className={`nexus-mesh-dot nexus-mesh-dot--${tone}${pulse ? ' nexus-mesh-dot--pulse' : ''}`} aria-hidden="true" />;
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
      const self = Boolean(status && device.node_id === status.node_id);
      const connection = nexusDeviceConnection(status, device);
      return <li key={`${device.group_id || 'home'}:${device.node_id}`} className="nexus-mesh-row">
        <Dot tone={self ? 'good' : connection ? CONNECTION_TONES[connection] : 'faint'} pulse={self || Boolean(connection && connection !== 'offline')} />
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

function ServerChoice({ selected, title, subtitle, connection, disabled, onChoose }: {
  selected: boolean; title: string; subtitle?: string; connection?: NexusConnection; disabled: boolean; onChoose: () => void;
}) {
  const tone = connection ? CONNECTION_TONES[connection] : 'good';
  return <button type="button" className={`nexus-mesh-row nexus-mesh-row--choice${selected ? ' is-selected' : ''}`} aria-pressed={selected} disabled={disabled} onClick={onChoose}>
    <Dot tone={tone} pulse={connection !== 'offline'} />
    <span className="nexus-mesh-row__body">
      <span className="nexus-mesh-row__name">{title}</span>
      {subtitle && <span className="nexus-mesh-row__sub">{subtitle}</span>}
    </span>
    {connection && <NexusPill label={NEXUS_CONNECTION_LABELS[connection]} tone={tone} connection />}
    {selected && <Icon name="check" size={16} className="nexus-mesh-row__check" />}
  </button>;
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
  const subtitle = [status?.device_name, status?.tunnel_ip?.split('/')[0]].filter(Boolean).join(' · ');
  return <section className="nexus-mesh" aria-labelledby={id}>
    <div className="nexus-mesh__header">
      <span className="nexus-mesh__icon"><Icon name="router" size={20} /></span>
      <div className="nexus-mesh__title">
        <strong id={id}>{t('Mesh network')}</strong>
        <span>{subtitle || t('Your Lemonade devices, connected over an encrypted mesh.')}</span>
      </div>
      <span className="nexus-mesh__state" role="status">
        <Dot tone={STATE_TONES[summary.state]} pulse={summary.state === 'connected' || summary.state === 'connecting'} />
        {t(summary.label, summary.count === undefined ? undefined : { count: summary.count })}
      </span>
    </div>
    {network.error && <div className="nexus-mesh__problem" role="alert">
      <p className="nexus-error">{network.error}</p>
      <WorkspaceActionButton size="small" onClick={() => void refreshNexus()}>{t('Retry')}</WorkspaceActionButton>
    </div>}
    {status?.error && !network.error && <p className="nexus-error" role="alert">{status.error}</p>}
    {network.notice && <p className="nexus-tree-notice" role="status">{network.notice} <button type="button" className="nexus-text-button" onClick={dismissNexusNotice}>{t('Dismiss')}</button></p>}
    {connected && status ? <>
      <div className="nexus-mesh__section">
        <p className="nexus-mesh__label">{t('Chat runs on')}</p>
        <ServerChoice selected={selected === 'local'} title={t('This device')} subtitle={t('Models on this computer')} disabled={busy} onChoose={() => void choose('local')} />
        {servers.map(device => <ServerChoice key={device.node_id} selected={selected === device.node_id} title={device.device_name || t('Unnamed device')}
          subtitle={device.tunnel_ip.split('/')[0]} connection={nexusDeviceConnection(status, device)} disabled={busy} onChoose={() => void choose(device.node_id)} />)}
        {!servers.length && <p className="nexus-mesh__empty">{t('No other Lemonade servers in this mesh yet. Invite a computer running Lemonade.')}</p>}
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
            <Dot tone={group.role === 'home' ? 'accent' : 'info'} />
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
