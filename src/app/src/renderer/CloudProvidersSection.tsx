import React, { useCallback, useEffect, useState } from 'react';
import { createPortal } from 'react-dom';
import { serverConfig, getServerBaseUrl } from './utils/serverConfig';
import {
  AppSettings,
  CloudProviderConfig,
  mergeWithDefaultSettings,
} from './utils/appSettings';
import CloudProviderModal, {
  CloudProviderInitialValues,
  CloudProviderSaveResult,
} from './CloudProviderModal';

interface CloudProviderRow {
  name: string;
  baseUrl: string;
  hasApiKey: boolean;
  modelCount: number | null; // null = not yet fetched (or failed)
}

interface CloudProvidersSectionProps {
  searchQuery: string;
  showError: (msg: string) => void;
  showSuccess: (msg: string) => void;
}

// Read the freshest copy of app settings off the persistence layer, then
// project into the cloudProviders sub-map. We bounce through
// mergeWithDefaultSettings so a corrupt file doesn't blow up the section.
const loadProvidersFromSettings = async (): Promise<Record<string, CloudProviderConfig>> => {
  if (!window.api?.getSettings) return {};
  const stored = await window.api.getSettings();
  const merged = mergeWithDefaultSettings(stored as AppSettings | undefined);
  return merged.cloudProviders ?? {};
};

// Discovery is a server-side proxy to <base_url>/v1/models that lemond
// performs with the supplied creds (never persisted server-side). The
// response shape is {object: "list", data: [{id, ...}, ...]}.
//
// Side effect: populates serverConfig's model-name -> upstream-id cache
// so subsequent chat requests can attach X-Lemonade-Cloud-Upstream-Model.
// Without this, a freshly-added provider's first chat request would hit
// the slash-parse fallback (wrong upstream id for providers that clean
// their public ids).
const discoverModelCount = async (
  name: string,
  cfg: CloudProviderConfig,
): Promise<number | null> => {
  if (!cfg.apiKey) return null;
  try {
    const url = `${getServerBaseUrl()}/internal/cloud/discover`;
    const response = await serverConfig.fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ provider: name, base_url: cfg.baseUrl, api_key: cfg.apiKey }),
    });
    if (!response.ok) return null;
    const body = await response.json();
    const list = Array.isArray(body?.data) ? body.data : [];
    const checkpointEntries: Array<{ id: string; checkpoint: string }> = [];
    for (const entry of list) {
      if (typeof entry?.id === 'string' && typeof entry?.checkpoint === 'string' && entry.checkpoint.length > 0) {
        checkpointEntries.push({ id: entry.id, checkpoint: entry.checkpoint });
      }
    }
    serverConfig.setCloudModelCheckpoints(name, checkpointEntries);
    return list.length;
  } catch {
    return null;
  }
};

const CloudProvidersSection: React.FC<CloudProvidersSectionProps> = ({
  searchQuery, showError, showSuccess
}) => {
  const [rows, setRows] = useState<CloudProviderRow[]>([]);
  const [isLoading, setIsLoading] = useState(true);
  const [modal, setModal] = useState<
    | { mode: 'add' }
    | { mode: 'edit'; initialValues: CloudProviderInitialValues }
    | null
  >(null);
  const [refreshNonce, setRefreshNonce] = useState(0);
  const triggerRefresh = useCallback(() => setRefreshNonce((n) => n + 1), []);

  useEffect(() => {
    let cancelled = false;
    const load = async () => {
      setIsLoading(true);
      try {
        const providers = await loadProvidersFromSettings();
        if (cancelled) return;

        // Show provider rows immediately with modelCount=null, then update
        // counts as discovery completes (one async call per provider in
        // parallel). Keeps the section responsive even when a provider's
        // /v1/models is slow or unreachable.
        const initialRows: CloudProviderRow[] = Object.entries(providers)
          .map(([name, cfg]) => ({
            name,
            baseUrl: cfg.baseUrl,
            hasApiKey: cfg.apiKey.length > 0,
            modelCount: null,
          }))
          .sort((a, b) => a.name.localeCompare(b.name));
        setRows(initialRows);
        setIsLoading(false);

        await Promise.all(
          Object.entries(providers).map(async ([name, cfg]) => {
            const count = await discoverModelCount(name, cfg);
            if (cancelled) return;
            setRows((prev) =>
              prev.map((r) => (r.name === name ? { ...r, modelCount: count } : r))
            );
          })
        );
      } catch (e) {
        console.error('Failed to load cloud providers:', e);
        if (!cancelled) setIsLoading(false);
      }
    };
    load();
    return () => { cancelled = true; };
  }, [refreshNonce]);

  // Listen for settings updates pushed from other windows / panels so the
  // section reflects edits made elsewhere without a page refresh.
  useEffect(() => {
    if (!window.api?.onSettingsUpdated) return;
    const unsubscribe = window.api.onSettingsUpdated(() => triggerRefresh());
    return () => unsubscribe?.();
  }, [triggerRefresh]);

  const saveProvider = useCallback(async (result: CloudProviderSaveResult) => {
    if (!window.api?.getSettings || !window.api?.saveSettings) {
      throw new Error('Settings storage unavailable');
    }
    const stored = await window.api.getSettings();
    const merged = mergeWithDefaultSettings(stored as AppSettings | undefined);
    const existing = merged.cloudProviders[result.name];
    // apiKey === null means "keep existing" (edit mode without Replace).
    // Falling back to '' on a brand-new entry is fine — validation in the
    // modal already required a key in add mode.
    const apiKey = result.apiKey ?? existing?.apiKey ?? '';
    merged.cloudProviders[result.name] = { baseUrl: result.baseUrl, apiKey };
    await window.api.saveSettings(merged);

    // Probe the just-saved provider so the user sees an immediate count
    // (or learns the creds are wrong before they leave the panel).
    const count = await discoverModelCount(result.name, { baseUrl: result.baseUrl, apiKey });
    if (count === null) {
      showError(
        `Saved '${result.name}', but discovery failed. Check the API key and base URL.`
      );
    } else if (count === 0) {
      showError(
        `Saved '${result.name}', but discovery returned 0 chat models. Double-check the API key, base URL, and that your account has chat-model access.`
      );
    } else {
      showSuccess(
        `Saved '${result.name}' — discovered ${count} model${count === 1 ? '' : 's'}.`
      );
    }
    triggerRefresh();
  }, [showError, showSuccess, triggerRefresh]);

  const removeProvider = useCallback(async (name: string) => {
    if (!window.api?.getSettings || !window.api?.saveSettings) {
      throw new Error('Settings storage unavailable');
    }
    const stored = await window.api.getSettings();
    const merged = mergeWithDefaultSettings(stored as AppSettings | undefined);
    delete merged.cloudProviders[name];
    await window.api.saveSettings(merged);
    showSuccess(`Removed provider '${name}'.`);
    triggerRefresh();
  }, [showSuccess, triggerRefresh]);

  const query = searchQuery.trim().toLowerCase();
  const filtered = query
    ? rows.filter((p) => `${p.name} ${p.baseUrl}`.toLowerCase().includes(query))
    : rows;

  // Hide the section entirely when a search has no matches and the user is
  // not searching the section name itself.
  if (query && filtered.length === 0 && !'cloud'.includes(query)) {
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
            <span className="category-count">({filtered.length})</span>
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
          ) : filtered.length === 0 ? (
            <div className="backend-row-item" style={{ padding: '4px 12px', opacity: 0.7, fontSize: '0.74rem' }}>
              {query
                ? 'No providers match your search.'
                : 'No providers configured. Click "+ Add" to connect Fireworks, OpenAI, Together, OpenRouter, or any OpenAI-compatible endpoint.'}
            </div>
          ) : (
            filtered.map((p) => {
              // We can't see env vars from the renderer, so a non-zero
              // modelCount with hasApiKey=false implies the server's
              // LEMONADE_<NAME>_API_KEY fallback is satisfying discovery.
              const envKeyLikely = !p.hasApiKey && (p.modelCount ?? 0) > 0;
              const ok = p.hasApiKey || envKeyLikely;
              const dotClass = ok ? 'loaded' : 'update-required';
              const dotTitle = p.hasApiKey
                ? 'API key stored on this client'
                : envKeyLikely
                  ? `Auth working — likely via the LEMONADE_${p.name.toUpperCase()}_API_KEY env var on the server`
                  : `No API key for this client. Click Edit to add one, or set LEMONADE_${p.name.toUpperCase()}_API_KEY on the server.`;

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
                            {p.modelCount === null ? '…' : p.modelCount} model{p.modelCount === 1 ? '' : 's'}
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
              onSave={saveProvider}
              onRemove={modal.mode === 'edit' ? removeProvider : undefined}
              showError={showError}
            />
          </div>
        </div>,
        document.body
      )}
    </>
  );
};

export default CloudProvidersSection;
