import React, { useId, useState } from 'react';
import { useI18n } from '../i18n';
import {
  isNexusDesktop, prepareNexus, refreshNexus, useNexusNetwork,
} from '../features/nexus/nexus';
import { NexusHierarchy } from './NexusHierarchy';
import { Icon } from './Icon';
import { WorkspaceActionButton } from './WorkspacePanels';

export function NexusNetwork({ advanced, showCompact = true }: { advanced: boolean; showCompact?: boolean }) {
  const { t } = useI18n();
  const network = useNexusNetwork();
  const { status } = network;
  const [deviceName, setDeviceName] = useState('');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const id = useId();
  const desktop = isNexusDesktop();
  const connected = Boolean(status?.mesh_up && !status.locked && !network.error);
  const prepare = async () => {
    setBusy(true); setError('');
    try { await prepareNexus(deviceName.trim() || status?.device_name || t('My Lemonade device'), false); }
    catch (error) { setError(error instanceof Error ? error.message : String(error)); }
    finally { await refreshNexus(); setBusy(false); }
  };
  return <>
    {connected && status ? (advanced || showCompact) && <NexusHierarchy key={`${status.group_node_id}:${status.node_id}`} status={status} advanced={advanced} /> : advanced && <div className="nexus-network-empty">
      <span className="nexus-network-empty__icon"><Icon name="layers" size={28} /></span>
      <h4>{status?.enabled && status.locked ? t('Unlock your network') : t('A place for every group and device')}</h4>
      <p>{t('Start with a group of your devices. Add teams, people or locations as your network grows.')}</p>
      {status?.available && desktop ? <form onSubmit={event => { event.preventDefault(); void prepare(); }}>
        {!status.enabled && <><label htmlFor={id}>{t('This device’s name')}</label><input id={id} className="input" value={deviceName} maxLength={128} placeholder={t('My Lemonade device')} onChange={event => setDeviceName(event.target.value)} /></>}
        <WorkspaceActionButton type="submit" disabled={busy}>{busy ? t('Connecting…') : status.enabled && status.locked ? t('Unlock with passkey') : t('Create my network')}</WorkspaceActionButton>
        <p className="nexus-hint">{t('Verify this device with a passkey to manage its network.')}</p>
      </form> : <p className="nexus-hint">{network.error || (status && !status.available) ? t('Nexus is not available on this server. Connect to a server with Nexus support to continue.') : !status ? t('Checking network status…') : t('Open the Lemonade desktop app on the server to set up or unlock its network.')}</p>}
      {error && <p className="nexus-error" role="alert">{error}</p>}
      {status?.error && <p className="nexus-error" role="alert">{status.error}</p>}
      <WorkspaceActionButton appearance="quiet" size="small" icon="rotate-ccw" disabled={busy} onClick={() => void refreshNexus()}>{t('Check again')}</WorkspaceActionButton>
    </div>}
  </>;
}
