import React, { useCallback, useEffect, useState } from 'react';
import { createPortal } from 'react-dom';
import {
  AppSettings,
  SemanticRoutingConfig,
  DEFAULT_SEMANTIC_ROUTING,
  mergeWithDefaultSettings,
} from './utils/appSettings';
import SemanticRoutingModal from './SemanticRoutingModal';

interface SemanticRoutingSectionProps {
  searchQuery: string;
  showError: (msg: string) => void;
  showSuccess: (msg: string) => void;
}

interface RoutingStatus {
  enabled: boolean;
  signalsEnabled: {
    jailbreak: boolean;
    pii: boolean;
    keywords: number;
    complexity: boolean;
  };
  modelCount: number;
  serviceHealthy: boolean | null;
}

const loadRoutingConfigFromSettings = async (): Promise<SemanticRoutingConfig> => {
  if (!window.api?.getSettings) return DEFAULT_SEMANTIC_ROUTING;
  const stored = await window.api.getSettings();
  const merged = mergeWithDefaultSettings(stored as AppSettings | undefined);
  return merged.semanticRouting ?? DEFAULT_SEMANTIC_ROUTING;
};

const parseYamlConfig = (yaml: string): RoutingStatus | null => {
  try {
    const lines = yaml.split('\n');
    const status: RoutingStatus = {
      enabled: false,
      signalsEnabled: { jailbreak: false, pii: false, keywords: 0, complexity: false },
      modelCount: 0,
      serviceHealthy: null,
    };

    let inModels = false;
    let inSignals = false;
    let inKeywords = false;

    for (const line of lines) {
      const trimmed = line.trim();
      if (trimmed.startsWith('enabled:')) {
        status.enabled = trimmed.includes('true');
      }
      if (trimmed === 'models:') {
        inModels = true;
        inSignals = false;
        inKeywords = false;
      } else if (trimmed === 'signals:') {
        inModels = false;
        inSignals = true;
        inKeywords = false;
      } else if (trimmed === 'keywords:') {
        inKeywords = true;
      } else if (inModels && trimmed.startsWith('- name:')) {
        status.modelCount++;
      } else if (inSignals && !inKeywords) {
        if (trimmed.startsWith('jailbreak:')) {
          status.signalsEnabled.jailbreak = true;
        } else if (trimmed.startsWith('pii:')) {
          status.signalsEnabled.pii = true;
        } else if (trimmed.startsWith('complexity:')) {
          status.signalsEnabled.complexity = true;
        }
      } else if (inKeywords && trimmed.match(/^\w+:/) && !trimmed.startsWith('corpus:') && !trimmed.startsWith('threshold:') && !trimmed.startsWith('target:')) {
        status.signalsEnabled.keywords++;
      }
    }

    return status;
  } catch {
    return null;
  }
};

const SemanticRoutingSection: React.FC<SemanticRoutingSectionProps> = ({
  searchQuery, showError, showSuccess
}) => {
  const [config, setConfig] = useState<SemanticRoutingConfig>(DEFAULT_SEMANTIC_ROUTING);
  const [status, setStatus] = useState<RoutingStatus | null>(null);
  const [isLoading, setIsLoading] = useState(true);
  const [showModal, setShowModal] = useState(false);
  const [refreshNonce, setRefreshNonce] = useState(0);
  const triggerRefresh = useCallback(() => setRefreshNonce((n) => n + 1), []);

  useEffect(() => {
    let cancelled = false;
    const load = async () => {
      setIsLoading(true);
      try {
        const cfg = await loadRoutingConfigFromSettings();
        if (cancelled) return;
        setConfig(cfg);
        const parsed = parseYamlConfig(cfg.configYaml);
        setStatus(parsed);
        setIsLoading(false);

        // Push config to service on load (if enabled and has config)
        if (cfg.enabled && cfg.configYaml.trim()) {
          try {
            await fetch(`http://127.0.0.1:${cfg.servicePort}/config`, {
              method: 'POST',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify({ config_yaml: cfg.configYaml }),
            });
          } catch {
            // Service may not be running yet, ignore
          }
        }
      } catch (e) {
        console.error('Failed to load semantic routing config:', e);
        if (!cancelled) setIsLoading(false);
      }
    };
    load();
    return () => { cancelled = true; };
  }, [refreshNonce]);

  useEffect(() => {
    if (!window.api?.onSettingsUpdated) return;
    const unsubscribe = window.api.onSettingsUpdated(() => triggerRefresh());
    return () => unsubscribe?.();
  }, [triggerRefresh]);

  const saveConfig = useCallback(async (newConfig: SemanticRoutingConfig) => {
    if (!window.api?.getSettings || !window.api?.saveSettings) {
      throw new Error('Settings storage unavailable');
    }
    const stored = await window.api.getSettings();
    const merged = mergeWithDefaultSettings(stored as AppSettings | undefined);
    merged.semanticRouting = newConfig;
    await window.api.saveSettings(merged);
    showSuccess('Semantic routing configuration saved.');
    triggerRefresh();
  }, [showSuccess, triggerRefresh]);

  const toggleEnabled = useCallback(async () => {
    const newConfig = { ...config, enabled: !config.enabled };
    await saveConfig(newConfig);
  }, [config, saveConfig]);

  const query = searchQuery.trim().toLowerCase();
  if (query && !('routing semantic'.includes(query))) {
    return null;
  }

  const enabledSignalsCount = status
    ? (status.signalsEnabled.jailbreak ? 1 : 0) +
      (status.signalsEnabled.pii ? 1 : 0) +
      status.signalsEnabled.keywords +
      (status.signalsEnabled.complexity ? 1 : 0)
    : 0;

  return (
    <>
      <div className="model-category">
        <div className="model-category-header static" style={{ justifyContent: 'space-between' }}>
          <span>
            <span className="category-label">Routing</span>
            <span className="category-count">
              ({config.enabled ? `${enabledSignalsCount} signals` : 'disabled'})
            </span>
          </span>
          <button
            className="settings-reset-button"
            style={{ fontSize: '0.65rem', padding: '1px 8px', whiteSpace: 'nowrap' }}
            title="Configure semantic routing"
            onClick={(e) => { e.stopPropagation(); setShowModal(true); }}
          >
            Configure
          </button>
        </div>

        <div className="model-list">
          {isLoading ? (
            <div className="backend-row-item" style={{ padding: '4px 12px', opacity: 0.7, fontSize: '0.74rem' }}>
              Loading…
            </div>
          ) : (
            <div
              className="model-item backend-row-item"
              style={{ padding: '2px 12px' }}
            >
              <div className="model-item-content">
                <div className="model-info-left backend-row-main">
                  <div className="backend-row-head">
                    <span className="model-name backend-name">
                      <span
                        className={`model-status-indicator ${config.enabled ? 'loaded' : 'not-downloaded'}`}
                        title={config.enabled ? 'Routing enabled' : 'Routing disabled'}
                      >●</span>
                      Semantic Router
                    </span>
                  </div>
                  <div className="backend-row-detail">
                    <div className="backend-inline-meta">
                      {status ? (
                        <>
                          <span className="backend-version">
                            {status.modelCount} model{status.modelCount === 1 ? '' : 's'}
                          </span>
                          <span className="backend-meta-separator">•</span>
                          <span className="backend-size">
                            {enabledSignalsCount} signal{enabledSignalsCount === 1 ? '' : 's'}
                            {status.signalsEnabled.jailbreak && ' (jailbreak'}
                            {status.signalsEnabled.pii && (status.signalsEnabled.jailbreak ? ', pii' : ' (pii')}
                            {status.signalsEnabled.complexity && (status.signalsEnabled.jailbreak || status.signalsEnabled.pii ? ', complexity' : ' (complexity')}
                            {status.signalsEnabled.keywords > 0 && (status.signalsEnabled.jailbreak || status.signalsEnabled.pii || status.signalsEnabled.complexity ? `, ${status.signalsEnabled.keywords} keywords` : ` (${status.signalsEnabled.keywords} keywords`)}
                            {(status.signalsEnabled.jailbreak || status.signalsEnabled.pii || status.signalsEnabled.complexity || status.signalsEnabled.keywords > 0) && ')'}
                          </span>
                        </>
                      ) : (
                        <span className="backend-version" style={{ opacity: 0.7 }}>
                          No config loaded
                        </span>
                      )}
                    </div>
                    <div className="model-actions">
                      <button
                        className="settings-reset-button"
                        style={{ fontSize: '0.65rem', padding: '1px 8px' }}
                        onClick={toggleEnabled}
                      >
                        {config.enabled ? 'Disable' : 'Enable'}
                      </button>
                    </div>
                  </div>
                </div>
              </div>
            </div>
          )}
        </div>
      </div>

      {showModal && createPortal(
        <div
          className="settings-overlay"
          onMouseDown={(e: React.MouseEvent<HTMLDivElement>) => {
            if (e.target === e.currentTarget) setShowModal(false);
          }}
        >
          <div className="settings-modal" style={{ maxWidth: '700px', width: '90vw' }} onMouseDown={(e: React.MouseEvent) => e.stopPropagation()}>
            <SemanticRoutingModal
              config={config}
              onClose={() => setShowModal(false)}
              onSave={saveConfig}
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

export default SemanticRoutingSection;
