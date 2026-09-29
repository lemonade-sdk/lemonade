import React, { useEffect, useRef, useState } from 'react';
import { dismissNexusNotice, nexusStatusLabel, selectNexusInferenceServer, useNexusNetwork } from '../features/nexus/nexus';
import { friendlyErrorMessage } from '../api';
import { useFocusTrap } from '../hooks/useFocusTrap';
import { WorkspaceActionButton } from './WorkspacePanels';
import '../styles/nexus.css';

const NexusShortcut: React.FC<{ onOpen: () => void }> = ({ onOpen }) => {
  const network = useNexusNetwork();
  const { status, notice } = network;
  const [error, setError] = useState('');
  const [needsKey, setNeedsKey] = useState<string | null>(null);
  const [key, setKey] = useState('');
  const dialogRef = useRef<HTMLDivElement>(null);
  const selector = useRef<HTMLSelectElement>(null);
  useFocusTrap(dialogRef, Boolean(needsKey));
  const close = () => { setNeedsKey(null); setKey(''); selector.current?.focus(); };
  useEffect(() => {
    if (!needsKey) return;
    const escape = (event: KeyboardEvent) => { if (event.key === 'Escape' && !network.switchingServer) close(); };
    document.addEventListener('keydown', escape);
    return () => document.removeEventListener('keydown', escape);
  }, [needsKey, network.switchingServer]);
  const choose = async (nodeId: string, apiKey?: string) => {
    setError('');
    try { await selectNexusInferenceServer(nodeId, apiKey); close(); }
    catch (error) {
      const message = friendlyErrorMessage(error);
      setError(message);
      if (nodeId !== 'local' && /api key|401|403|unauthorized|authentication/i.test(message)) setNeedsKey(nodeId);
    }
  };
  const peers = status?.members.filter(member => member.node_id !== status.node_id) || [];
  return <>
    <button className="nexus-shortcut" type="button" onClick={onOpen} aria-label={`Devices and mesh: ${nexusStatusLabel(status)}`} title={nexusStatusLabel(status)} data-tauri-drag-region="false">
      <span className={`nexus-shortcut__dot${status?.mesh_up ? ' is-online' : ''}`} aria-hidden="true" />
      <span className="nexus-shortcut__label">Nexus</span>
    </button>
    {(peers.length > 0 || network.selectedServerId && network.selectedServerId !== 'local') && <select ref={selector} className="nexus-server-select" aria-label="Inference server" title="Inference server" data-tauri-drag-region="false"
      value={network.selectedServerId || 'local'} disabled={network.switchingServer} onChange={event => void choose(event.target.value)}>
      <option value="local">This device</option>
      {peers.map(device => <option key={device.node_id} value={device.node_id} disabled={!status?.mesh_up || device.is_online === false}>{device.device_name || 'Mesh device'}{device.is_online === false ? ' (offline)' : ''}</option>)}
      {network.selectedServerId && network.selectedServerId !== 'local' && !peers.some(peer => peer.node_id === network.selectedServerId) && <option value={network.selectedServerId} disabled>Mesh server disconnected</option>}
    </select>}
    {(notice || error) && !needsKey && <div className="nexus-toast" role={error ? 'alert' : 'status'}><span>{error || notice}</span><WorkspaceActionButton appearance="quiet" size="small" onClick={onOpen}>View devices</WorkspaceActionButton><WorkspaceActionButton appearance="quiet" size="small" icon="x" aria-label="Dismiss notification" onClick={() => { setError(''); dismissNexusNotice(); }} /></div>}
    {needsKey && <div className="nexus-dialog__backdrop" data-tauri-drag-region="false"><div ref={dialogRef} className="nexus-dialog" role="dialog" aria-modal="true" aria-labelledby="nexus-server-key-title">
      <h2 id="nexus-server-key-title">Server API key</h2><p>{peers.find(peer => peer.node_id === needsKey)?.device_name || 'This server'} requires an API key.</p>
      {error && <p className="connect__error" role="alert">{error}</p>}
      <form onSubmit={event => { event.preventDefault(); const value = key; setKey(''); void choose(needsKey, value); }}>
        <label htmlFor="nexus-selector-key" className="form-field__label">Remote server API key</label>
        <input id="nexus-selector-key" className="input" type="password" autoComplete="off" value={key} onChange={event => setKey(event.target.value)} />
        <WorkspaceActionButton type="submit" appearance="primary" disabled={network.switchingServer || !key.trim()}>Connect</WorkspaceActionButton>
        <WorkspaceActionButton appearance="quiet" disabled={network.switchingServer} onClick={close}>Cancel</WorkspaceActionButton>
      </form>
    </div></div>}
  </>;
};
export default NexusShortcut;
