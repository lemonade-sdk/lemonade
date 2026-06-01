import React, { useState, useEffect, useRef } from 'react';
import { SemanticRoutingConfig, DEFAULT_SEMANTIC_ROUTING } from './utils/appSettings';

interface SemanticRoutingModalProps {
  config: SemanticRoutingConfig;
  onClose: () => void;
  onSave: (config: SemanticRoutingConfig) => Promise<void>;
  showError: (msg: string) => void;
  showSuccess: (msg: string) => void;
}

const SAMPLE_CONFIG = `# Lemonade Semantic Router Config
version: "1.0"
enabled: true
default_model: "Qwen3.5-9B-NoThinking"

models:
  - name: "local-small"
    id: "Qwen3.5-9B-NoThinking"
    type: local
  - name: "cloud-kimi"
    id: "fireworks.kimi-k2p6"
    type: cloud

signals:
  jailbreak:
    enabled: true
    threshold: 0.7

  pii:
    enabled: true
    threshold: 0.9

  keywords:
    complex_task:
      corpus:
        - "system design"
        - "software architecture"
        - "implement"
        - "refactor"
        - "algorithm"
        - "machine learning"
      threshold: 0.25
      target: "cloud-kimi"
    simple_query:
      corpus:
        - "what is"
        - "define"
        - "list"
        - "how do i"
      threshold: 0.15
      target: "local-small"

  complexity:
    enabled: true
    low: "local-small"
    medium: "local-small"
    high: "cloud-kimi"

settings:
  signal_timeout_ms: 500
  fail_open: true
`;

const CONFIG_TEMPLATES: Array<{ name: string; config: string }> = [
  {
    name: 'Full Example',
    config: SAMPLE_CONFIG,
  },
  {
    name: 'Security Only',
    config: `# Security-focused routing (block jailbreak/PII, no model routing)
version: "1.0"
enabled: true

models: []

signals:
  jailbreak:
    enabled: true
    threshold: 0.7

  pii:
    enabled: true
    threshold: 0.9

  keywords: {}

  complexity:
    enabled: false

settings:
  signal_timeout_ms: 500
  fail_open: true
`,
  },
  {
    name: 'Complexity Based',
    config: `# Route by complexity only (no security checks)
version: "1.0"
enabled: true
default_model: "Qwen3.5-9B-NoThinking"

models:
  - name: "local"
    id: "Qwen3.5-9B-NoThinking"
    type: local
  - name: "cloud"
    id: "fireworks.kimi-k2p6"
    type: cloud

signals:
  jailbreak:
    enabled: false

  pii:
    enabled: false

  keywords: {}

  complexity:
    enabled: true
    low: "local"
    medium: "local"
    high: "cloud"

settings:
  signal_timeout_ms: 500
  fail_open: true
`,
  },
];

const SemanticRoutingModal: React.FC<SemanticRoutingModalProps> = ({
  config, onClose, onSave, showError, showSuccess
}) => {
  const [enabled, setEnabled] = useState(config.enabled);
  const [configYaml, setConfigYaml] = useState(config.configYaml || SAMPLE_CONFIG);
  const [servicePort, setServicePort] = useState(config.servicePort);
  const [isSaving, setIsSaving] = useState(false);
  const [isValidating, setIsValidating] = useState(false);
  const [validationResult, setValidationResult] = useState<{ valid: boolean; errors: string[] } | null>(null);
  const [error, setError] = useState<string | null>(null);
  const textareaRef = useRef<HTMLTextAreaElement>(null);

  useEffect(() => {
    if (textareaRef.current) {
      textareaRef.current.focus();
    }
  }, []);

  const handleTemplateSelect = (template: { name: string; config: string }) => {
    setConfigYaml(template.config);
    setValidationResult(null);
  };

  const handleValidate = async () => {
    setIsValidating(true);
    setValidationResult(null);
    setError(null);

    try {
      const response = await fetch(`http://127.0.0.1:${servicePort}/config/validate`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ config_yaml: configYaml }),
      });

      if (!response.ok) {
        throw new Error(`Validation service returned ${response.status}`);
      }

      const result = await response.json();
      setValidationResult({ valid: result.valid, errors: result.errors || [] });

      if (result.valid) {
        showSuccess('Configuration is valid.');
      }
    } catch (e: any) {
      const basicErrors = validateYamlLocally(configYaml);
      if (basicErrors.length > 0) {
        setValidationResult({ valid: false, errors: basicErrors });
      } else {
        setError(`Validation service unavailable. Basic syntax check passed but full validation requires the routing service to be running on port ${servicePort}.`);
        setValidationResult({ valid: true, errors: [] });
      }
    } finally {
      setIsValidating(false);
    }
  };

  const validateYamlLocally = (yaml: string): string[] => {
    const errors: string[] = [];
    const lines = yaml.split('\n');

    let hasVersion = false;
    let hasModels = false;
    let hasSignals = false;

    for (const line of lines) {
      const trimmed = line.trim();
      if (trimmed.startsWith('version:')) hasVersion = true;
      if (trimmed === 'models:') hasModels = true;
      if (trimmed === 'signals:') hasSignals = true;
    }

    if (!hasVersion) errors.push("Missing 'version' field");
    if (!hasModels) errors.push("Missing 'models' section");
    if (!hasSignals) errors.push("Missing 'signals' section");

    return errors;
  };

  const handleSave = async () => {
    setError(null);

    if (!configYaml.trim()) {
      setError('Configuration cannot be empty.');
      return;
    }

    const basicErrors = validateYamlLocally(configYaml);
    if (basicErrors.length > 0) {
      setError(`Invalid configuration: ${basicErrors.join(', ')}`);
      return;
    }

    setIsSaving(true);
    try {
      // Push config to the Python semantic router service
      try {
        const response = await fetch(`http://127.0.0.1:${servicePort}/config`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ config_yaml: configYaml }),
        });
        if (!response.ok) {
          const errBody = await response.json().catch(() => ({}));
          const errMsg = errBody.detail || `Service returned ${response.status}`;
          throw new Error(errMsg);
        }
        showSuccess('Configuration pushed to semantic router service.');
      } catch (serviceErr: any) {
        // Non-fatal: save locally even if service is down
        console.warn('Failed to push config to service:', serviceErr);
        showError(`Config saved locally, but failed to push to service: ${serviceErr.message}. Restart the service to apply.`);
      }

      const newConfig: SemanticRoutingConfig = {
        enabled,
        configYaml,
        servicePort,
        lastValidated: new Date().toISOString(),
      };
      await onSave(newConfig);
      onClose();
    } catch (e: any) {
      const msg = e?.message || String(e);
      setError(msg);
      showError(`Failed to save configuration: ${msg}`);
    } finally {
      setIsSaving(false);
    }
  };

  const handleReset = () => {
    setEnabled(DEFAULT_SEMANTIC_ROUTING.enabled);
    setConfigYaml(SAMPLE_CONFIG);
    setServicePort(DEFAULT_SEMANTIC_ROUTING.servicePort);
    setValidationResult(null);
    setError(null);
  };

  return (
    <>
      <div className="settings-header">
        <h3>Semantic Routing Configuration</h3>
        <button className="settings-close-button" onClick={onClose} title="Close">
          <svg width="14" height="14" viewBox="0 0 14 14">
            <path d="M 1,1 L 13,13 M 13,1 L 1,13" stroke="currentColor" strokeWidth="2" strokeLinecap="round"/>
          </svg>
        </button>
      </div>

      <div className="settings-content" style={{ maxHeight: '60vh', overflowY: 'auto' }}>
        <span className="settings-description" style={{ display: 'block', marginBottom: '12px' }}>
          Configure intelligent prompt routing between local and cloud models based on security signals (jailbreak, PII detection), keyword matching, and complexity scoring.
        </span>

        <div className="form-section">
          <label className="form-label">
            <input
              type="checkbox"
              checked={enabled}
              onChange={(e) => setEnabled(e.target.checked)}
              style={{ marginRight: '8px' }}
            />
            Enable semantic routing
          </label>
        </div>

        <div className="form-section">
          <label className="form-label">Service Port</label>
          <input
            type="number"
            className="form-input"
            value={servicePort}
            onChange={(e) => setServicePort(parseInt(e.target.value) || 8765)}
            min={1024}
            max={65535}
            style={{ width: '100px' }}
          />
          <span className="settings-description" style={{ display: 'block', marginTop: '4px', fontSize: '0.7rem' }}>
            Port where the Python semantic router service runs (default: 8765)
          </span>
        </div>

        <div className="form-section">
          <label className="form-label">Templates</label>
          <div style={{ display: 'flex', flexWrap: 'wrap', gap: '6px' }}>
            {CONFIG_TEMPLATES.map((template) => (
              <button
                key={template.name}
                type="button"
                className="settings-reset-button"
                style={{ fontSize: '12px', padding: '4px 10px' }}
                onClick={() => handleTemplateSelect(template)}
              >
                {template.name}
              </button>
            ))}
          </div>
        </div>

        <div className="form-section">
          <label className="form-label">Routing Configuration (YAML)</label>
          <textarea
            ref={textareaRef}
            className="form-input"
            value={configYaml}
            onChange={(e) => {
              setConfigYaml(e.target.value);
              setValidationResult(null);
            }}
            style={{
              fontFamily: 'monospace',
              fontSize: '12px',
              minHeight: '300px',
              resize: 'vertical',
              whiteSpace: 'pre',
              overflowX: 'auto',
            }}
            spellCheck={false}
          />
        </div>

        {validationResult && (
          <div
            className={`form-section`}
            style={{
              padding: '8px 12px',
              borderRadius: '4px',
              backgroundColor: validationResult.valid ? 'rgba(34, 197, 94, 0.1)' : 'rgba(239, 68, 68, 0.1)',
              border: `1px solid ${validationResult.valid ? 'rgba(34, 197, 94, 0.3)' : 'rgba(239, 68, 68, 0.3)'}`,
            }}
          >
            {validationResult.valid ? (
              <span style={{ color: '#22c55e' }}>Configuration is valid</span>
            ) : (
              <div>
                <span style={{ color: '#ef4444' }}>Validation errors:</span>
                <ul style={{ margin: '4px 0 0 0', paddingLeft: '20px' }}>
                  {validationResult.errors.map((err, i) => (
                    <li key={i} style={{ color: '#ef4444', fontSize: '0.8rem' }}>{err}</li>
                  ))}
                </ul>
              </div>
            )}
          </div>
        )}

        {error && <div className="form-error">{error}</div>}
      </div>

      <div className="settings-footer">
        <button
          className="settings-reset-button"
          style={{ marginRight: 'auto' }}
          onClick={handleReset}
          disabled={isSaving}
        >
          Reset to Default
        </button>
        <button
          className="settings-reset-button"
          onClick={handleValidate}
          disabled={isValidating || isSaving}
        >
          {isValidating ? 'Validating…' : 'Validate'}
        </button>
        <button className="settings-reset-button" onClick={onClose} disabled={isSaving}>
          Cancel
        </button>
        <button className="settings-save-button" onClick={handleSave} disabled={isSaving}>
          {isSaving ? 'Saving…' : 'Save'}
        </button>
      </div>
    </>
  );
};

export default SemanticRoutingModal;
