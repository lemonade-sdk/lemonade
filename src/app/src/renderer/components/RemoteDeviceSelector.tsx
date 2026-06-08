import React, { useEffect, useRef, useState } from 'react';
import {
  ChatTarget,
  getChatTarget,
  getRemoteChatBaseUrl,
  onChatTargetChange,
  setChatTarget,
} from '../utils/serverConfig';
import { RemoteDevice } from '../../global';

// Spotify Connect-style picker. Hidden entirely when there are no peers, so
// solo users see no UI at all and the feature stays "zero configuration"
// (per the plan). When peers are detected, a pill appears in the chat header
// with the current target's hostname and a green dot for remote selections.
//
// Discovery is driven by the Tauri host's beacon listener
// (src/app/src-tauri/src/beacon.rs). The web app's mock returns an empty
// device list, which keeps this component invisible there too.

interface RemoteDeviceSelectorProps {
  disabled?: boolean;
}

const RemoteDeviceSelector: React.FC<RemoteDeviceSelectorProps> = ({ disabled = false }) => {
  const [devices, setDevices] = useState<RemoteDevice[]>([]);
  const [target, setTarget] = useState<ChatTarget>(() => getChatTarget());
  const [open, setOpen] = useState(false);
  const wrapperRef = useRef<HTMLDivElement>(null);

  // Initial fetch + live subscription. listRemoteDevices is undefined on
  // older shim builds; fall back to an empty list so the component stays
  // hidden rather than throwing.
  useEffect(() => {
    let cancelled = false;
    const load = async () => {
      try {
        if (!window.api?.listRemoteDevices) return;
        const list = await window.api.listRemoteDevices();
        if (!cancelled) setDevices(list);
      } catch (err) {
        console.warn('listRemoteDevices failed:', err);
      }
    };
    load();

    const unsubscribe = window.api?.onRemoteDevicesUpdated?.((list) => {
      setDevices(list);
    });

    return () => {
      cancelled = true;
      if (typeof unsubscribe === 'function') unsubscribe();
    };
  }, []);

  // React to target changes from anywhere (e.g. another tab through
  // localStorage, or programmatic setChatTarget calls).
  useEffect(() => onChatTargetChange(setTarget), []);

  // Close dropdown on outside click — same pattern as the audio menu.
  useEffect(() => {
    const handleClickOutside = (e: MouseEvent) => {
      if (wrapperRef.current && !wrapperRef.current.contains(e.target as Node)) {
        setOpen(false);
      }
    };
    document.addEventListener('mousedown', handleClickOutside);
    return () => document.removeEventListener('mousedown', handleClickOutside);
  }, []);

  // Auto-recover from a peer that disappeared: if the stored remote URL is
  // no longer in the registry, fall back to local. This is what makes the
  // "shut down a peer mid-chat" path safe — without it, the next chat send
  // would hang trying to talk to a vanished host.
  useEffect(() => {
    const remoteUrl = getRemoteChatBaseUrl();
    if (!remoteUrl) return;
    if (devices.length === 0) return; // Wait for the registry to be primed.
    const stillThere = devices.some((d) => !d.isLocal && d.baseUrl === remoteUrl);
    if (!stillThere) {
      console.info('Selected chat target is no longer on the LAN; reverting to local.');
      setChatTarget(null);
    }
  }, [devices]);

  // Only show the picker when there's at least one peer. With zero peers,
  // the local-only experience is identical to before this feature existed.
  const remotes = devices.filter((d) => !d.isLocal);
  if (remotes.length === 0) return null;

  // Web-app mode never gets remote devices (the mock returns []) so the
  // early-return above already handles it.

  const isRemote = !target.isLocal;
  const currentRemote = isRemote
    ? remotes.find((d) => d.baseUrl === getRemoteChatBaseUrl())
    : undefined;
  const label = isRemote
    ? currentRemote?.hostname ?? 'Remote device'
    : 'This device';

  const handlePick = (rawBaseUrl: string | null) => {
    setChatTarget(rawBaseUrl);
    setOpen(false);
  };

  return (
    <div className="run-on-picker" ref={wrapperRef}>
      <button
        type="button"
        className={`run-on-pill ${isRemote ? 'remote' : 'local'}`}
        onClick={() => !disabled && setOpen((v) => !v)}
        disabled={disabled}
        title={isRemote ? `Chat is running on ${label}` : 'Chat is running on this device'}
      >
        <span className={`run-on-dot ${isRemote ? 'remote' : 'local'}`} />
        <span className="run-on-label">{label}</span>
        <svg
          width="10" height="10" viewBox="0 0 24 24" fill="none"
          style={{ transform: open ? 'rotate(180deg)' : 'none', transition: 'transform 0.15s' }}
        >
          <path d="M6 9L12 15L18 9" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round" />
        </svg>
      </button>
      {open && (
        <div className="run-on-menu" role="menu">
          <div className="run-on-menu-header">Run chat on</div>
          <button
            type="button"
            role="menuitemradio"
            aria-checked={!isRemote}
            className={`run-on-menu-item ${!isRemote ? 'selected' : ''}`}
            onClick={() => handlePick(null)}
          >
            <span className="run-on-dot local" />
            <span className="run-on-menu-text">
              <span className="run-on-menu-name">This device</span>
              <span className="run-on-menu-sub">Local server</span>
            </span>
          </button>
          {remotes.map((d) => {
            const selected = isRemote && d.baseUrl === getRemoteChatBaseUrl();
            return (
              <button
                key={d.baseUrl}
                type="button"
                role="menuitemradio"
                aria-checked={selected}
                className={`run-on-menu-item ${selected ? 'selected' : ''}`}
                onClick={() => handlePick(d.baseUrl)}
              >
                <span className="run-on-dot remote" />
                <span className="run-on-menu-text">
                  <span className="run-on-menu-name">{d.hostname}</span>
                  <span className="run-on-menu-sub">{d.baseUrl.replace(/\/api\/v1\/?$/, '')}</span>
                </span>
              </button>
            );
          })}
        </div>
      )}
    </div>
  );
};

export default RemoteDeviceSelector;
