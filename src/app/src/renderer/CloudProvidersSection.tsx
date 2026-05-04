import React, { useCallback, useEffect, useState } from 'react';
import { createPortal } from 'react-dom';
import { ChevronRight } from './components/Icons';
import { serverConfig, getServerBaseUrl } from './utils/serverConfig';
import CloudProviderModal, { CloudProviderInitialValues } from './CloudProviderModal';

interface CloudProviderState {
  name: string;
  baseUrl: string;
  hasApiKey: boolean;
  modelCount: number | null; // null = not yet fetched
}

interface CloudProvidersSectionProps {
  searchQuery: string;
  showError: (msg: string) => void;
  showSuccess: (msg: string) => void;
}

const isPlainObject = (x: unknown): x is Record<string, unknown> =>
  typeof x === 'object' && x !== null && !Array.isArray(x);

const CloudProvidersSection: React.FC<CloudProvidersSectionProps> = ({
  searchQuery, showError, showSuccess
}) => {
  const [enabled, setEnabled] = useState<boolean>(false);
  const [providers, setProviders] = useState<CloudProviderState[]>([]);
  const [isLoading, setIsLoading] = useState(true);
  const [isExpanded, setIsExpanded] = useState(true);
  const [modal, setModal] = useState<
    | { mode: 'add' }
    | { mode: 'edit'; initialValues: CloudProviderInitialValues }
    | null
  >(null);
  const [refreshNonce, setRefreshNonce] = useState(0);
  const triggerRefresh = useCallback(() => setRefreshNonce((n) => n + 1), []);

  // Fetch /internal/config + /v1/models in parallel; combine into per-provider rows.
  useEffect(() => {
    let cancelled = false;
    const load = async () => {
      setIsLoading(true);
      try {
        const configUrl = `${getServerBaseUrl()}/internal/config`;
        const [configResp, modelsResp] = await Promise.all([
          serverConfig.fetch(configUrl),
          serverConfig.fetch('/models?show_all=true'),
        ]);
        if (cancelled) return;

        let cloudOffload: Record<string, unknown> = {};
        if (configResp.ok) {
          const body = await configResp.json();
          if (isPlainObject(body) && isPlainObject(body['cloud_offload'])) {
            cloudOffload = body['cloud_offload'];
          }
        }
        const enabledFlag = cloudOffload['enabled'] === true;
        const providersObj = isPlainObject(cloudOffload['providers'])
          ? (cloudOffload['providers'] as Record<string, unknown>)
          : {};

        // Per-provider model counts come from /v1/models, where dynamically
        // discovered cloud entries have ids like "<provider>/<model>".
        const counts: Record<string, number> = {};
        if (modelsResp.ok) {
          const modelsBody = await modelsResp.json();
          const list = Array.isArray(modelsBody) ? modelsBody : modelsBody?.data || [];
          list.forEach((m: any) => {
            if (!m?.id || typeof m.id !== 'string') return;
            const slash = m.id.indexOf('/');
            if (slash <= 0) return;
            const prefix = m.id.substring(0, slash);
            if (Object.prototype.hasOwnProperty.call(providersObj, prefix)) {
              counts[prefix] = (counts[prefix] || 0) + 1;
            }
          });
        }

        const rows: CloudProviderState[] = Object.entries(providersObj).map(([name, cfg]) => {
          const cfgObj = isPlainObject(cfg) ? cfg : {};
          const baseUrl = typeof cfgObj['base_url'] === 'string' ? cfgObj['base_url'] : '';
          const apiKey = typeof cfgObj['api_key'] === 'string' ? cfgObj['api_key'] : '';
          return {
            name,
            baseUrl,
            // Server may also have an env var supplying the key — we can't see
            // env vars from here, so "hasApiKey" only reflects the config.json
            // value. The Edit modal makes this clear.
            hasApiKey: apiKey.length > 0,
            modelCount: counts[name] ?? 0,
          };
        });
        rows.sort((a, b) => a.name.localeCompare(b.name));

        setEnabled(enabledFlag);
        setProviders(rows);
      } catch (e) {
        console.error('Failed to load cloud providers:', e);
      } finally {
        if (!cancelled) setIsLoading(false);
      }
    };
    load();
    return () => { cancelled = true; };
  }, [refreshNonce]);

  const handleToggleEnabled = useCallback(async (next: boolean) => {
    const prev = enabled;
    setEnabled(next);
    try {
      // GET /internal/config reads the snapshot; the matching write endpoint
      // is POST /internal/set, not POST /internal/config (which 404s).
      const url = `${getServerBaseUrl()}/internal/set`;
      const resp = await serverConfig.fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ cloud_offload: { enabled: next } }),
      });
      if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
      showSuccess(next ? 'Cloud offload enabled.' : 'Cloud offload disabled.');
      triggerRefresh();
    } catch (e: any) {
      setEnabled(prev);
      showError(`Failed to toggle cloud offload: ${e?.message || e}`);
    }
  }, [enabled, showError, showSuccess, triggerRefresh]);

  const query = searchQuery.trim().toLowerCase();
  const filteredProviders = query
    ? providers.filter((p) => `${p.name} ${p.baseUrl}`.toLowerCase().includes(query))
    : providers;

  // When searching, hide the section entirely if it has no matches and the
  // user isn't searching for the word "cloud" itself.
  if (query && filteredProviders.length === 0 && !'cloud'.includes(query) && !'cloud providers'.includes(query)) {
    return null;
  }

  return (
    <>
      <div className="model-category">
        <div
          className="model-category-header"
          onClick={() => setIsExpanded((v) => !v)}
          style={{ cursor: 'pointer' }}
        >
          <span className="category-label-wrap">
            <span
              style={{
                display: 'inline-flex',
                transform: isExpanded ? 'rotate(90deg)' : 'rotate(0deg)',
                transition: 'transform 0.15s',
              }}
            >
              <ChevronRight size={14} />
            </span>
            <span className="category-label">Cloud Providers</span>
            <span className="category-count">({filteredProviders.length})</span>
          </span>
          <button
            className="settings-reset-button"
            style={{ fontSize: '11px', padding: '2px 8px', whiteSpace: 'nowrap' }}
            title="Add a new cloud provider"
            onClick={(e) => { e.stopPropagation(); setModal({ mode: 'add' }); }}
          >
            + Add
          </button>
        </div>

        {isExpanded && (
          <div className="model-list">
            {/* Master toggle. Sized to match local backend rows: the label
                uses .backend-name (0.74rem), helper text uses
                .backend-status-message (0.62rem). */}
            <div
              className="backend-row-item"
              style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '4px 12px' }}
            >
              <div style={{ minWidth: 0 }}>
                <span className="backend-name">Cloud offload</span>
                <div className="backend-status-message" style={{ marginLeft: 0, whiteSpace: 'normal' }}>
                  When off, configured providers are hidden and no remote requests are made.
                </div>
              </div>
              <label style={{ display: 'flex', alignItems: 'center', gap: '6px', cursor: 'pointer' }}>
                <input
                  type="checkbox"
                  className="settings-checkbox"
                  checked={enabled}
                  disabled={isLoading}
                  onChange={(e) => handleToggleEnabled(e.target.checked)}
                />
                <span style={{ fontSize: '0.7rem' }}>{enabled ? 'Enabled' : 'Disabled'}</span>
              </label>
            </div>

            {isLoading ? (
              <div className="backend-row-item" style={{ padding: '6px 12px', opacity: 0.7, fontSize: '0.74rem' }}>
                Loading…
              </div>
            ) : filteredProviders.length === 0 ? (
              <div className="backend-row-item" style={{ padding: '6px 12px', opacity: 0.7, fontSize: '0.74rem' }}>
                {query
                  ? 'No providers match your search.'
                  : 'No providers configured. Click "+ Add" to connect Fireworks, OpenAI, Together, OpenRouter, or any OpenAI-compatible endpoint.'}
              </div>
            ) : (
              filteredProviders.map((p) => {
                // The api_key may also come from a LEMONADE_<NAME>_API_KEY env
                // var, which we can't see from here. If discovery returned
                // models, auth is working regardless of what's in config.
                const envKeyLikely = !p.hasApiKey && (p.modelCount ?? 0) > 0;
                const status: { label: string; bg: string; fg: string; title: string } = p.hasApiKey
                  ? { label: '✓ Key set',  bg: 'rgba(34,197,94,0.18)', fg: '#22c55e', title: 'API key set in config.json' }
                  : envKeyLikely
                    ? { label: '✓ Env key',  bg: 'rgba(34,197,94,0.18)', fg: '#22c55e', title: `Auth working — likely via the LEMONADE_${p.name.toUpperCase()}_API_KEY env var` }
                    : { label: '⚠ No key',  bg: 'rgba(234,179,8,0.18)', fg: '#ca8a04', title: `No API key found. Set one in this UI or use the LEMONADE_${p.name.toUpperCase()}_API_KEY env var.` };
                return (
                  <div key={p.name} className="backend-row-item" style={{ padding: '4px 12px' }}>
                    <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: '8px' }}>
                      <span className="backend-name">{p.name}</span>
                      <button
                        className="settings-reset-button"
                        style={{ fontSize: '0.7rem', padding: '2px 10px' }}
                        onClick={() => setModal({
                          mode: 'edit',
                          initialValues: { name: p.name, baseUrl: p.baseUrl, hasApiKey: p.hasApiKey },
                        })}
                      >
                        Edit
                      </button>
                    </div>
                    <div style={{ display: 'flex', alignItems: 'center', gap: '6px', marginTop: '2px', flexWrap: 'wrap', paddingLeft: '14px' }}>
                      <span
                        style={{
                          fontSize: '0.62rem',
                          padding: '1px 8px',
                          borderRadius: '10px',
                          background: status.bg,
                          color: status.fg,
                        }}
                        title={status.title}
                      >
                        {status.label}
                      </span>
                      {enabled && (
                        <span style={{ fontSize: '0.62rem', opacity: 0.7 }}>
                          {p.modelCount ?? 0} model{p.modelCount === 1 ? '' : 's'}
                        </span>
                      )}
                    </div>
                    <div className="backend-status-message" style={{ wordBreak: 'break-all', whiteSpace: 'normal', overflow: 'visible', textOverflow: 'clip' }}>
                      {p.baseUrl || <em>(no base URL)</em>}
                    </div>
                  </div>
                );
              })
            )}
          </div>
        )}
      </div>

      {modal && createPortal(
        <div
          className="settings-overlay"
          onMouseDown={(e: React.MouseEvent<HTMLDivElement>) => {
            if (e.target === e.currentTarget) setModal(null);
          }}
        >
          <div className="settings-modal" onMouseDown={(e: React.MouseEvent) => e.stopPropagation()}>
            <CloudProviderModal
              mode={modal.mode}
              initialValues={modal.mode === 'edit' ? modal.initialValues : undefined}
              onClose={() => setModal(null)}
              onSaved={triggerRefresh}
              showError={showError}
              showSuccess={showSuccess}
            />
          </div>
        </div>,
        document.body
      )}
    </>
  );
};

export default CloudProvidersSection;
