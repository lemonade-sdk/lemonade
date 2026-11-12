import React, { useId, useState } from 'react';
import { useI18n } from '../i18n';
import { useNexusAccount } from '../features/nexus/NexusAccountContext';
import {
  isNexusDesktop, prepareNexus, refreshNexus, useNexusNetwork, usesNexusDeviceKey,
} from '../features/nexus/nexus';
import { NexusHierarchy } from './NexusHierarchy';
import { NexusRoutePermissions } from './NexusRoutePermissions';
import { WorkspaceActionButton } from './WorkspacePanels';

export function NexusNetwork({ advanced, showCompact = true }: { advanced: boolean; showCompact?: boolean }) {
  const { t } = useI18n();
  const network = useNexusNetwork();
  const { status } = network;
  const { account } = useNexusAccount();
  const [deviceName, setDeviceName] = useState('');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const id = useId();
  // A passkey is verified by the desktop app. A device key needs no verification, and a
  // browser unlocks with the device key even where a passkey was registered (prepareNexus).
  const deviceKey = Boolean(status && (usesNexusDeviceKey(status) || !isNexusDesktop()));
  const connected = Boolean(status?.mesh_up && !status.locked && !network.error);
  // Set up and unlocked, but the mesh is not up or the server is out of reach: nothing to
  // create or unlock here, the network comes back by itself.
  const reconnecting = Boolean(status?.enabled && !status.locked);
  const connecting = reconnecting && !network.error && (status?.status === 'starting' || status?.status === 'joining');
  const prepare = async () => {
    setBusy(true); setError('');
    try { await prepareNexus(deviceName.trim() || status?.device_name || t('My Lemonade device'), false); }
    catch (error) { setError(error instanceof Error ? error.message : String(error)); }
    finally { await refreshNexus(); setBusy(false); }
  };
  return <>
    {advanced && connected && status && account && account.id === status.group_node_id && account.role && <NexusRoutePermissions
      key={`${network.selectedServerId}:${status.group_node_id}:${status.node_id}`} viewerRole={account.role} />}
    {connected && status ? (advanced || showCompact) && <NexusHierarchy key={`${status.group_node_id}:${status.node_id}`} status={status} advanced={advanced} /> : advanced && <div className="nexus-network-empty">
      <h4>{!status?.enabled ? t('No network yet') : status.locked ? t('Unlock your network') : connecting ? t('Connecting to your network…') : t('Your network is offline')}</h4>
      {status && !status.enabled && <p>{t('Create a network for this device, then invite your other devices.')}</p>}
      {reconnecting ? <p>{t(connecting ? 'The server is joining its network.' : 'The server has a network but is not connected to it.')}</p>
        : status?.available ? <form onSubmit={event => { event.preventDefault(); void prepare(); }}>
        {!status.enabled && <><label htmlFor={id}>{t('Device name')}</label><input id={id} className="input" value={deviceName} maxLength={128} placeholder={t('My Lemonade device')} onChange={event => setDeviceName(event.target.value)} /><p className="nexus-hint">{t('Other devices in the mesh see this computer by this name.')}</p></>}
        <WorkspaceActionButton type="submit" disabled={busy}>{busy ? t('Connecting…') : status.enabled && status.locked ? (deviceKey ? t('Use this device’s key') : t('Unlock with passkey')) : t('Create my network')}</WorkspaceActionButton>
      </form> : <p className="nexus-hint">{network.error || (status && !status.available) ? t('Nexus is not available on this server. Connect to a server with Nexus support to continue.') : t('Checking network status…')}</p>}
      {error && <p className="nexus-error" role="alert">{error}</p>}
      <WorkspaceActionButton appearance="quiet" size="small" icon="rotate-ccw" disabled={busy} onClick={() => void refreshNexus()}>{t('Check again')}</WorkspaceActionButton>
    </div>}
  </>;
}
