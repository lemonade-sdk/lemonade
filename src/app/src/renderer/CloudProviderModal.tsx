import React, { useEffect, useRef, useState } from 'react';

export interface CloudProviderInitialValues {
  name?: string;
  baseUrl?: string;
  hasApiKey?: boolean; // true when editing an existing provider that already has a key set
}

export interface CloudProviderSaveResult {
  name: string;
  baseUrl: string;
  apiKey: string | null; // null = keep existing (edit mode without "Replace")
}

interface CloudProviderModalProps {
  mode: 'add' | 'edit';
  initialValues?: CloudProviderInitialValues;
  onClose: () => void;
  // Parent owns persistence (local app settings) and discovery (calls
  // /internal/cloud/discover). The modal is a pure form.
  onSave: (result: CloudProviderSaveResult) => Promise<void>;
  onRemove?: (name: string) => Promise<void>;
  showError: (msg: string) => void;
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
  mode, initialValues, onClose, onSave, onRemove, showError
}) => {
  const [name, setName] = useState(initialValues?.name ?? '');
  const [baseUrl, setBaseUrl] = useState(initialValues?.baseUrl ?? '');
  const [apiKey, setApiKey] = useState('');
  // Edit-mode safety: when an existing provider already has a key in local
  // settings, the field is masked behind a "Replace" gate so the user can
  // change base_url without unintentionally clobbering the saved key with
  // an empty value. In every other case the input is editable directly.
  const [replaceKey, setReplaceKey] = useState(mode === 'add' || !initialValues?.hasApiKey);
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
    if (mode !== 'edit' || !initialValues?.name || !onRemove) return;
    const target = initialValues.name;
    if (!window.confirm(`Remove provider "${target}"? This deletes it from this client's settings.`)) {
      return;
    }
    setError(null);
    setIsSaving(true);
    try {
      await onRemove(target);
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

    // null signals "keep existing key" — only sent when editing and the
    // user did not opt into Replace.
    const apiKeyToSave: string | null = (replaceKey && trimmedKey) ? trimmedKey : null;

    setIsSaving(true);
    try {
      await onSave({ name: trimmedName, baseUrl: trimmedBaseUrl, apiKey: apiKeyToSave });
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
          <label className="form-label" title="Short identifier used as the model name prefix">
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
          <label className="form-label" title="API key for the provider. Stored locally on this client (in this app's settings), never on the lemonade server. Sent per-request via the X-Lemonade-Cloud-Key header.">
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
        {mode === 'edit' && initialValues?.name && onRemove && (
          <button
            className="settings-reset-button"
            style={{ marginRight: 'auto', color: '#ef4444' }}
            onClick={handleRemove}
            disabled={isSaving}
            title="Remove this provider from this client's settings"
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
