import React, { useEffect, useRef, useState } from 'react';
import { serverConfig, getServerBaseUrl } from './utils/serverConfig';

export interface CloudProviderInitialValues {
  name?: string;
  baseUrl?: string;
  hasApiKey?: boolean; // true when editing an existing provider that already has a key set
}

interface CloudProviderModalProps {
  mode: 'add' | 'edit';
  initialValues?: CloudProviderInitialValues;
  onClose: () => void;
  onSaved: () => void;
  showError: (msg: string) => void;
  showSuccess: (msg: string) => void;
}

// Common base URLs offered as quick-fill suggestions. Users can still paste
// any custom URL (vLLM/LM Studio/private gateways).
const BASE_URL_PRESETS: Array<{ name: string; baseUrl: string; label: string }> = [
  { name: 'fireworks',  baseUrl: 'https://api.fireworks.ai/inference/v1', label: 'Fireworks AI' },
  { name: 'openai',     baseUrl: 'https://api.openai.com/v1',              label: 'OpenAI' },
  { name: 'together',   baseUrl: 'https://api.together.xyz/v1',            label: 'Together AI' },
  { name: 'openrouter', baseUrl: 'https://openrouter.ai/api/v1',           label: 'OpenRouter' },
];

const CloudProviderModal: React.FC<CloudProviderModalProps> = ({
  mode, initialValues, onClose, onSaved, showError, showSuccess
}) => {
  const [name, setName] = useState(initialValues?.name ?? '');
  const [baseUrl, setBaseUrl] = useState(initialValues?.baseUrl ?? '');
  const [apiKey, setApiKey] = useState('');
  // When editing, the existing key is masked and not pre-filled. The user
  // toggles "Replace key" to type a new one. Leaving it blank keeps the
  // current key intact.
  const [replaceKey, setReplaceKey] = useState(mode === 'add');
  const [revealKey, setRevealKey] = useState(false);
  const [isSaving, setIsSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const nameInputRef = useRef<HTMLInputElement>(null);

  useEffect(() => {
    if (mode === 'add' && nameInputRef.current) {
      nameInputRef.current.focus();
    }
  }, [mode]);

  const handlePresetClick = (preset: { name: string; baseUrl: string }) => {
    if (mode !== 'add') return;
    if (!name.trim()) setName(preset.name);
    setBaseUrl(preset.baseUrl);
  };

  const handleRemove = async () => {
    if (mode !== 'edit' || !initialValues?.name) return;
    const target = initialValues.name;
    if (!window.confirm(`Remove provider "${target}"? This deletes its entry from config.json.`)) {
      return;
    }
    setError(null);
    setIsSaving(true);
    try {
      // null on a provider key is the deletion sentinel handled by
      // RuntimeConfig::apply_changes — it erases the entry on disk.
      const url = `${getServerBaseUrl()}/internal/set`;
      const response = await serverConfig.fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          cloud_offload: { providers: { [target]: null } },
        }),
      });
      if (!response.ok) {
        let msg = `HTTP ${response.status}`;
        try {
          const body = await response.json();
          if (body?.error) msg = typeof body.error === 'string' ? body.error : JSON.stringify(body.error);
        } catch { /* ignore */ }
        throw new Error(msg);
      }
      showSuccess(`Removed provider '${target}'.`);
      onSaved();
      onClose();
    } catch (e: any) {
      const msg = e?.message || String(e);
      setError(msg);
      showError(`Failed to remove provider: ${msg}`);
    } finally {
      setIsSaving(false);
    }
  };

  const handleSave = async () => {
    setError(null);
    const trimmedName = name.trim().toLowerCase();
    const trimmedBaseUrl = baseUrl.trim();
    const trimmedKey = apiKey.trim();

    if (!trimmedName) { setError('Provider name is required.'); return; }
    if (!/^[a-z0-9_-]+$/.test(trimmedName)) {
      setError('Provider name may only contain lowercase letters, digits, hyphens, and underscores.');
      return;
    }
    if (!trimmedBaseUrl) { setError('Base URL is required.'); return; }
    if (!/^https?:\/\//.test(trimmedBaseUrl)) {
      setError('Base URL must start with http:// or https://');
      return;
    }
    if (mode === 'add' && !trimmedKey) {
      setError('API key is required.');
      return;
    }

    // Build the nested patch. RuntimeConfig::set merges into existing config
    // — we only send the keys we want to change. When editing without
    // "Replace key" enabled, omit api_key entirely so the server keeps its
    // current value.
    const providerPatch: Record<string, string> = { base_url: trimmedBaseUrl };
    if (replaceKey && trimmedKey) {
      providerPatch.api_key = trimmedKey;
    }

    const patch = {
      cloud_offload: {
        enabled: true, // adding a provider implies the user wants cloud on
        providers: { [trimmedName]: providerPatch },
      },
    };

    setIsSaving(true);
    try {
      // POST goes to /internal/set; /internal/config is read-only (GET).
      const url = `${getServerBaseUrl()}/internal/set`;
      const response = await serverConfig.fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(patch),
      });
      if (!response.ok) {
        let msg = `HTTP ${response.status}`;
        try {
          const body = await response.json();
          if (body?.error) msg = typeof body.error === 'string' ? body.error : JSON.stringify(body.error);
        } catch { /* ignore */ }
        throw new Error(msg);
      }

      // Trigger a model cache rebuild + count discovered models for this
      // provider. The /v1/models route rebuilds the cache lazily.
      let discoveredCount: number | null = null;
      try {
        const modelsResponse = await serverConfig.fetch('/models?show_all=true');
        if (modelsResponse.ok) {
          const modelsBody = await modelsResponse.json();
          const list = Array.isArray(modelsBody) ? modelsBody : modelsBody?.data || [];
          const prefix = `${trimmedName}/`;
          discoveredCount = list.filter((m: any) => typeof m?.id === 'string' && m.id.startsWith(prefix)).length;
        }
      } catch { /* discovery is best-effort confirmation */ }

      if (discoveredCount === null) {
        showSuccess(`Saved provider '${trimmedName}'.`);
      } else if (discoveredCount === 0) {
        showError(`Saved '${trimmedName}', but discovery returned 0 models. Double-check the API key and base URL.`);
      } else {
        showSuccess(`Saved '${trimmedName}' — discovered ${discoveredCount} model${discoveredCount === 1 ? '' : 's'}.`);
      }
      onSaved();
      onClose();
    } catch (e: any) {
      const msg = e?.message || String(e);
      setError(msg);
      showError(`Failed to save provider: ${msg}`);
    } finally {
      setIsSaving(false);
    }
  };

  return (
    <>
      <div className="settings-header">
        <h3>{mode === 'add' ? 'Add Cloud Provider' : `Edit ${initialValues?.name ?? 'Provider'}`}</h3>
        <button className="settings-close-button" onClick={onClose} title="Close">
          <svg width="14" height="14" viewBox="0 0 14 14">
            <path d="M 1,1 L 13,13 M 13,1 L 1,13" stroke="currentColor" strokeWidth="2" strokeLinecap="round"/>
          </svg>
        </button>
      </div>

      <div className="settings-content">
        {mode === 'add' && (
          <div className="form-section">
            <label className="form-label">Quick-fill</label>
            <div style={{ display: 'flex', flexWrap: 'wrap', gap: '6px' }}>
              {BASE_URL_PRESETS.map((preset) => (
                <button
                  key={preset.name}
                  type="button"
                  className="settings-reset-button"
                  style={{ fontSize: '12px', padding: '4px 10px' }}
                  onClick={() => handlePresetClick(preset)}
                  title={preset.baseUrl}
                >
                  {preset.label}
                </button>
              ))}
            </div>
          </div>
        )}

        <div className="form-section">
          <label className="form-label" title="Short identifier used as the model name prefix and in the LEMONADE_<NAME>_API_KEY env var">
            Provider name
          </label>
          <input
            ref={nameInputRef}
            type="text"
            className="form-input"
            placeholder="fireworks"
            value={name}
            disabled={mode === 'edit'}
            onChange={(e) => setName(e.target.value)}
          />
        </div>

        <div className="form-section">
          <label className="form-label" title="OpenAI-compatible base URL ending in /v1 (or equivalent)">
            Base URL
          </label>
          <input
            type="text"
            className="form-input"
            placeholder="https://api.fireworks.ai/inference/v1"
            value={baseUrl}
            onChange={(e) => setBaseUrl(e.target.value)}
          />
        </div>

        <div className="form-section">
          <label className="form-label" title="API key for the provider. Stored on the server in config.json. Env var LEMONADE_<NAME>_API_KEY also supported and takes precedence.">
            API key {mode === 'edit' && initialValues?.hasApiKey && !replaceKey && '(currently set)'}
          </label>
          {mode === 'edit' && initialValues?.hasApiKey && !replaceKey ? (
            <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
              <input
                type="text"
                className="form-input"
                value="••••••••••••••••"
                disabled
                style={{ flex: 1 }}
              />
              <button
                type="button"
                className="settings-reset-button"
                onClick={() => setReplaceKey(true)}
              >
                Replace
              </button>
            </div>
          ) : (
            <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
              <input
                type={revealKey ? 'text' : 'password'}
                className="form-input"
                placeholder={mode === 'edit' ? 'Enter new API key (or cancel to keep existing)' : 'fw_…'}
                value={apiKey}
                onChange={(e) => setApiKey(e.target.value)}
                style={{ flex: 1 }}
                autoComplete="off"
              />
              <button
                type="button"
                className="settings-reset-button"
                onClick={() => setRevealKey((v) => !v)}
              >
                {revealKey ? 'Hide' : 'Show'}
              </button>
            </div>
          )}
        </div>

        {error && <div className="form-error">{error}</div>}
      </div>

      <div className="settings-footer">
        {mode === 'edit' && initialValues?.name && (
          <button
            className="settings-reset-button"
            style={{ marginRight: 'auto', color: '#ef4444' }}
            onClick={handleRemove}
            disabled={isSaving}
            title="Remove this provider from config.json"
          >
            Remove
          </button>
        )}
        <button className="settings-reset-button" onClick={onClose} disabled={isSaving}>
          Cancel
        </button>
        <button className="btn btn-primary" onClick={handleSave} disabled={isSaving}>
          {isSaving ? 'Saving…' : mode === 'add' ? 'Add provider' : 'Save changes'}
        </button>
      </div>
    </>
  );
};

export default CloudProviderModal;
