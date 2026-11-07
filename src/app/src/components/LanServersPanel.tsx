import React, { useCallback, useEffect, useState } from 'react';
import {
  WorkspaceActionButton,
  WorkspaceActionGroup,
  WorkspaceMetadataChip,
  WorkspaceResourceList,
  WorkspaceResourceRow,
} from './WorkspacePanels';
import { useI18n } from '../i18n';
import { tauriReady } from '../tauriShim';

// Lemonade servers found on the local network (#1221). The desktop app's beacon
// listener keeps every beacon from another machine as a list; nothing switches
// by itself. The user presses Use (connects through the normal Connect flow and
// remembers the choice) or Ignore (hidden until "Show ignored").
// Only the desktop app has a beacon listener, so in the web app this renders nothing.

export type LanServerState = 'new' | 'accepted' | 'ignored';
export type LanServerZone = 'home' | 'tailscale' | 'away';

export interface LanServer {
  id: string;
  hostname: string;
  urls: string[];
  zone: LanServerZone | null;
  seen: boolean;
  secondsSinceSeen: number | null;
  state: LanServerState;
}

interface LanServerHostApi {
  listLanServers?: () => Promise<LanServer[]>;
  setLanServerState?: (id: string, state: LanServerState, url?: string) => Promise<void>;
  onLanServersUpdated?: (callback: () => void) => () => void;
}

function hostApi(): LanServerHostApi | null {
  const api = (window as unknown as { api?: LanServerHostApi }).api;
  return api && typeof api.listLanServers === 'function' ? api : null;
}

/** The app keeps a server as its origin (http://host:port); beacons announce http://host:port/api/v1/. */
export function serverOrigin(url: string): string | null {
  try {
    const parsed = new URL(url);
    if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') return null;
    return parsed.origin;
  } catch {
    return null;
  }
}

/** The server's origins, home addresses first, then Tailscale, then anything else. */
export function candidateOrigins(server: LanServer): string[] {
  const score = (u: string) => {
    const host = (() => { try { return new URL(u).hostname; } catch { return ''; } })();
    if (/^(10\.|192\.168\.|172\.(1[6-9]|2\d|3[01])\.)/.test(host)) return 0;
    if (/^100\.(6[4-9]|[7-9]\d|1[01]\d|12[0-7])\./.test(host)) return 1;
    return 2;
  };
  const out: string[] = [];
  for (const u of [...server.urls].sort((a, b) => score(a) - score(b))) {
    const origin = serverOrigin(u);
    if (origin && !out.includes(origin)) out.push(origin);
  }
  return out;
}

export function preferredUrl(server: LanServer): string | null {
  return candidateOrigins(server)[0] ?? null;
}

/**
 * A server announces every interface it has, and not all of them are reachable from here (another
 * subnet, a firewall, a point-to-point link). Use the first address whose /api/v1/health answers.
 */
export async function firstReachableOrigin(origins: string[], timeoutMs = 2500): Promise<string | null> {
  for (const origin of origins) {
    const ctrl = new AbortController();
    const timer = window.setTimeout(() => ctrl.abort(), timeoutMs);
    try {
      const res = await fetch(`${origin}/api/v1/health`, { signal: ctrl.signal });
      if (res.ok) return origin;
    } catch {
      // try the next address
    } finally {
      window.clearTimeout(timer);
    }
  }
  return null;
}

const ZONE_LABEL: Record<LanServerZone, string> = {
  home: 'Home network',
  tailscale: 'Tailscale',
  away: 'Outside network',
};

interface LanServersPanelProps {
  /** The server the app is connected to now (an origin), to mark that row; empty while not connected. */
  currentBaseUrl: string;
  /** Connect to this origin through the normal Connect flow. Resolves true when it connected. */
  onUse: (origin: string) => Promise<boolean>;
  busy?: boolean;
}

const LanServersPanel: React.FC<LanServersPanelProps> = ({ currentBaseUrl, onUse, busy }) => {
  const { t } = useI18n();
  // The desktop shim installs window.api asynchronously; look again once it is ready.
  const [api, setApi] = useState(hostApi);
  useEffect(() => {
    if (api) return undefined;
    let cancelled = false;
    void tauriReady.then(() => { if (!cancelled) setApi(hostApi()); });
    return () => { cancelled = true; };
  }, [api]);
  const [servers, setServers] = useState<LanServer[]>([]);
  const [showIgnored, setShowIgnored] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [checking, setChecking] = useState<string | null>(null);

  const refresh = useCallback(async () => {
    if (!api?.listLanServers) return;
    try {
      setServers(await api.listLanServers());
      setError(null);
    } catch (err) {
      setError(String((err as Error)?.message ?? err));
    }
  }, [api]);

  useEffect(() => {
    if (!api) return undefined;
    void refresh();
    const stop = api.onLanServersUpdated?.(() => { void refresh(); });
    // "Seen n s ago" and expiry are only pushed on change; a slow poll keeps the ages honest.
    const timer = window.setInterval(() => { void refresh(); }, 15000);
    return () => { stop?.(); window.clearInterval(timer); };
  }, [api, refresh]);

  if (!api) return null;

  const setState = async (server: LanServer, state: LanServerState, url?: string) => {
    try {
      await api.setLanServerState?.(server.id, state, url);
      await refresh();
    } catch (err) {
      setError(String((err as Error)?.message ?? err));
    }
  };

  const handleUse = async (server: LanServer) => {
    setError(null);
    setChecking(server.id);
    const origin = await firstReachableOrigin(candidateOrigins(server));
    setChecking(null);
    if (!origin) {
      setError(t('None of the addresses {server} announces answered from this device.', { server: server.hostname }));
      return;
    }
    if (await onUse(origin)) await setState(server, 'accepted', origin);
  };

  const ignoredCount = servers.filter(s => s.state === 'ignored').length;
  const visible = servers.filter(s => showIgnored || s.state !== 'ignored');
  const current = serverOrigin(currentBaseUrl);

  return (
    <section className="connect__section connect__section--lan-servers" aria-labelledby="lan-servers-title">
      <div className="connect__lan-head">
        <h3 id="lan-servers-title">{t('Found on this network')}</h3>
        <p className="connect__hint">
          {t('Lemonade servers that announce themselves on your network. Nothing is used until you press Use.')}
        </p>
      </div>
      {error && <div className="connect__error">{t('Warning')}: {error}</div>}
      <WorkspaceResourceList className="connect__lan-server-list" label={t('Found on this network')}>
        {visible.length === 0 ? (
          <div className="connect__empty">
            {ignoredCount > 0 ? t('Only ignored servers so far.') : t('No other Lemonade servers heard on this network yet.')}
          </div>
        ) : visible.map(server => {
          const origin = preferredUrl(server);
          const inUse = Boolean(current && candidateOrigins(server).includes(current));
          const all = server.urls.map(u => serverOrigin(u) ?? u);
          const shown = inUse && current ? current : origin;
          const others = all.filter(u => u !== shown).length;
          const where = shown
            ? <span title={all.join('\n')}>{others > 0 ? t('{url} (+{count} more)', { url: shown, count: String(others) }) : shown}</span>
            : null;
          return (
            <WorkspaceResourceRow
              key={server.id}
              title={server.hostname}
              description={where ?? t('No address yet')}
              metadata={<>
                {server.zone && <WorkspaceMetadataChip tone={server.zone === 'away' ? 'warning' : 'neutral'}>{t(ZONE_LABEL[server.zone])}</WorkspaceMetadataChip>}
                {inUse
                  ? <WorkspaceMetadataChip tone="success">{t('In use')}</WorkspaceMetadataChip>
                  : server.state === 'accepted' && <WorkspaceMetadataChip tone="accent">{t('Used before')}</WorkspaceMetadataChip>}
                {!server.seen && <WorkspaceMetadataChip tone="neutral">{t('Not heard right now')}</WorkspaceMetadataChip>}
              </>}
              actions={<WorkspaceActionGroup label={t('Actions for {server}', { server: server.hostname })}>
                {server.state === 'ignored' ? (
                  <WorkspaceActionButton appearance="quiet" size="small" onClick={() => { void setState(server, 'new'); }}>{t('Stop ignoring')}</WorkspaceActionButton>
                ) : (
                  <>
                    <WorkspaceActionButton appearance="primary" size="small" icon="plug" disabled={busy || checking !== null || inUse || !origin || !server.seen} onClick={() => { void handleUse(server); }}>{checking === server.id ? t('Checking...') : t('Use')}</WorkspaceActionButton>
                    <WorkspaceActionButton appearance="quiet" size="small" disabled={inUse} onClick={() => { void setState(server, 'ignored'); }}>{t('Ignore')}</WorkspaceActionButton>
                  </>
                )}
              </WorkspaceActionGroup>}
            />
          );
        })}
      </WorkspaceResourceList>
      {ignoredCount > 0 && (
        <WorkspaceActionButton appearance="quiet" size="small" onClick={() => setShowIgnored(v => !v)}>
          {showIgnored ? t('Hide ignored') : t('Show ignored ({count})', { count: String(ignoredCount) })}
        </WorkspaceActionButton>
      )}
    </section>
  );
};

export default LanServersPanel;
