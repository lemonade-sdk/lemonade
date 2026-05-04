import React, { useCallback, useEffect, useState } from 'react';
import { createPortal } from 'react-dom';
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
  const [providers, setProviders] = useState<CloudProviderState[]>([]);
  const [isLoading, setIsLoading] = useState(true);
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
            // The server may also pull the key from a LEMONADE_<NAME>_API_KEY
            // env var, which we can't see from here — so hasApiKey only
            // reflects config.json. If discovery still returned models for
            // this provider, the env-var path is working; we surface that as
            // a healthy state below.
            hasApiKey: apiKey.length > 0,
            modelCount: counts[name] ?? 0,
          };
        });
        rows.sort((a, b) => a.name.localeCompare(b.name));

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

  const query = searchQuery.trim().toLowerCase();
  const filteredProviders = query
    ? providers.filter((p) => `${p.name} ${p.baseUrl}`.toLowerCase().includes(query))
    : providers;

  // Hide the section entirely when a search has no matches and the user is
  // not searching the section name itself.
  if (query && filteredProviders.length === 0 && !'cloud'.includes(query)) {
    return null;
  }

  return (
    <>
      <div className="model-category">
        {/* Header matches the local recipe rows: static (no chevron, no
            collapse), label + count on the left, "+ Add" pill on the right. */}
        <div className="model-category-header static" style={{ justifyContent: 'space-between' }}>
          <span>
            <span className="category-label">Cloud</span>
            <span className="category-count">({filteredProviders.length})</span>
          </span>
          <button
            className="settings-reset-button"
            style={{ fontSize: '0.65rem', padding: '1px 8px', whiteSpace: 'nowrap' }}
            title="Add a new cloud provider"
            onClick={(e) => { e.stopPropagation(); setModal({ mode: 'add' }); }}
          >
            + Add
          </button>
        </div>

        <div className="model-list">
          {isLoading ? (
            <div className="backend-row-item" style={{ padding: '4px 12px', opacity: 0.7, fontSize: '0.74rem' }}>
              Loading…
            </div>
          ) : filteredProviders.length === 0 ? (
            <div className="backend-row-item" style={{ padding: '4px 12px', opacity: 0.7, fontSize: '0.74rem' }}>
              {query
                ? 'No providers match your search.'
                : 'No providers configured. Click "+ Add" to connect Fireworks, OpenAI, Together, OpenRouter, or any OpenAI-compatible endpoint.'}
            </div>
          ) : (
            filteredProviders.map((p) => {
              // Auth state is derived in priority order: config key > env-var-
              // implied (no config key but discovery returned models) > none.
              // The env-var case is detected at runtime — we can't read env
              // vars from the renderer, so a working count is the proof.
              const envKeyLikely = !p.hasApiKey && (p.modelCount ?? 0) > 0;
              const ok = p.hasApiKey || envKeyLikely;
              const dotClass = ok ? 'loaded' : 'update-required';
              const dotTitle = p.hasApiKey
                ? 'API key set in config.json'
                : envKeyLikely
                  ? `Auth working — likely via the LEMONADE_${p.name.toUpperCase()}_API_KEY env var`
                  : `No API key found. Set one in this UI or use the LEMONADE_${p.name.toUpperCase()}_API_KEY env var.`;

              return (
                <div
                  key={p.name}
                  className="model-item backend-row-item"
                  style={{ padding: '2px 12px' }}
                >
                  <div className="model-item-content">
                    <div className="model-info-left backend-row-main">
                      <div className="backend-row-head">
                        <span className="model-name backend-name">
                          <span
                            className={`model-status-indicator ${dotClass}`}
                            title={dotTitle}
                          >●</span>
                          {p.name}
                        </span>
                      </div>
                      <div className="backend-row-detail">
                        <div className="backend-inline-meta">
                          <span className="backend-version">
                            {p.modelCount ?? 0} model{p.modelCount === 1 ? '' : 's'}
                          </span>
                          {p.baseUrl && (
                            <>
                              <span className="backend-meta-separator">•</span>
                              <span className="backend-size" style={{ overflow: 'hidden', textOverflow: 'ellipsis' }}>
                                {p.baseUrl}
                              </span>
                            </>
                          )}
                        </div>
                        <div className="model-actions">
                          <button
                            className="settings-reset-button"
                            style={{ fontSize: '0.65rem', padding: '1px 8px' }}
                            onClick={() => setModal({
                              mode: 'edit',
                              initialValues: { name: p.name, baseUrl: p.baseUrl, hasApiKey: p.hasApiKey },
                            })}
                          >
                            Edit
                          </button>
                        </div>
                      </div>
                    </div>
                  </div>
                </div>
              );
            })
          )}
        </div>
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
