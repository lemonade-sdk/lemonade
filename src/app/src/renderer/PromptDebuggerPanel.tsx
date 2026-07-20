import React, { useEffect, useRef, useState } from 'react';
import { XIcon } from './components/Icons';
import { serverFetch } from './utils/serverConfig';
import { adjustTextareaHeight } from './utils/textareaUtils';
import {
  DecisionResult,
  RoutingPolicyDoc,
  downloadTraceFile,
  renderDecisionTree,
} from './utils/decisionTree';

interface PromptDebuggerPanelProps {
  showError: (message: string) => void;
  showSuccess: (message: string) => void;
  showWarning: (message: string) => void;
}

/**
 * Parses a `key: value` per-line textarea into a metadata object.
 * Splits on the first colon only, so values may contain colons; blank lines
 * are skipped; a line with no colon or an empty key is a format error.
 */
function parseMetadataText(text: string): { metadata: Record<string, string>; error: string | null } {
  const metadata: Record<string, string> = {};
  const lines = text.split('\n');
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i].trim();
    if (line === '') continue;
    const colonIndex = line.indexOf(':');
    if (colonIndex === -1) {
      return { metadata: {}, error: `Metadata line ${i + 1} is missing a ':' (expected "key: value")` };
    }
    const key = line.slice(0, colonIndex).trim();
    const value = line.slice(colonIndex + 1).trim();
    if (key === '') {
      return { metadata: {}, error: `Metadata line ${i + 1} has an empty key` };
    }
    metadata[key] = value;
  }
  return { metadata, error: null };
}

const PromptDebuggerPanel: React.FC<PromptDebuggerPanelProps> = ({ showError }) => {
  const [prompt, setPrompt] = useState('');
  const [policyJson, setPolicyJson] = useState<RoutingPolicyDoc | null>(null);
  const [policyFilename, setPolicyFilename] = useState<string | null>(null);
  const [imageFilename, setImageFilename] = useState<string | null>(null);
  const [imageThumbnailUrl, setImageThumbnailUrl] = useState<string | null>(null);
  const [hasImages, setHasImages] = useState(false);
  const [hasTools, setHasTools] = useState(false);
  const [metadataText, setMetadataText] = useState('');
  const [metadataError, setMetadataError] = useState<string | null>(null);
  const [isValidating, setIsValidating] = useState(false);
  const [validationError, setValidationError] = useState<string | null>(null);
  const [decision, setDecision] = useState<DecisionResult | null>(null);
  const [showTreeModal, setShowTreeModal] = useState(false);

  const policyInputRef = useRef<HTMLInputElement>(null);
  const imageInputRef = useRef<HTMLInputElement>(null);
  const imageThumbnailUrlRef = useRef<string | null>(null);
  imageThumbnailUrlRef.current = imageThumbnailUrl;

  useEffect(() => {
    // Revoke the local preview object URL on unmount so it doesn't leak.
    return () => {
      if (imageThumbnailUrlRef.current) {
        URL.revokeObjectURL(imageThumbnailUrlRef.current);
      }
    };
  }, []);

  useEffect(() => {
    setMetadataError(parseMetadataText(metadataText).error);
  }, [metadataText]);

  useEffect(() => {
    if (!showTreeModal) return;
    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') setShowTreeModal(false);
    };
    document.addEventListener('keydown', handleKeyDown);
    return () => {
      document.removeEventListener('keydown', handleKeyDown);
    };
  }, [showTreeModal]);

  const handlePolicyFileChange = (e: React.ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0];
    e.target.value = '';
    if (!file) return;
    const reader = new FileReader();
    reader.onload = (ev) => {
      try {
        const json = JSON.parse(ev.target?.result as string);
        setPolicyJson(json);
        setPolicyFilename(file.name);
        setValidationError(null);
        setDecision(null);
      } catch {
        showError(`"${file.name}" is not valid JSON.`);
      }
    };
    reader.readAsText(file);
  };

  const clearPolicy = () => {
    setPolicyJson(null);
    setPolicyFilename(null);
    setDecision(null);
    setValidationError(null);
  };

  const handleImageFileChange = (e: React.ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0];
    e.target.value = '';
    if (!file) return;
    if (imageThumbnailUrl) URL.revokeObjectURL(imageThumbnailUrl);
    setImageFilename(file.name);
    setImageThumbnailUrl(URL.createObjectURL(file));
    setHasImages(true);
  };

  const clearImage = () => {
    if (imageThumbnailUrl) URL.revokeObjectURL(imageThumbnailUrl);
    setImageFilename(null);
    setImageThumbnailUrl(null);
    setHasImages(false);
  };

  const handleValidate = async () => {
    if (!policyJson) return;
    setIsValidating(true);
    setValidationError(null);
    try {
      const { metadata } = parseMetadataText(metadataText);
      const response = await serverFetch('/routing/validate', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          policy: policyJson,
          prompt,
          has_images: hasImages,
          has_tools: hasTools,
          ...(Object.keys(metadata).length > 0 ? { metadata } : {}),
        }),
      });
      const data = await response.json();
      if (!response.ok) {
        setValidationError(data?.error ?? `HTTP ${response.status}`);
        setDecision(null);
        return;
      }
      setDecision(data.decision as DecisionResult);
    } catch (error) {
      setValidationError(error instanceof Error ? error.message : 'Unknown error');
      setDecision(null);
    } finally {
      setIsValidating(false);
    }
  };

  return (
    <div className="prompt-debugger-panel">
      <div className="form-section">
        <label className="form-label">Prompt</label>
        <textarea
          className="form-input prompt-debugger-textarea"
          placeholder="Enter a prompt to test against the routing policy..."
          value={prompt}
          onChange={(e) => {
            setPrompt(e.target.value);
            adjustTextareaHeight(e.target);
          }}
        />
      </div>

      <div className="form-section">
        <label className="form-label">Routing Policy</label>
        <input
          ref={policyInputRef}
          type="file"
          accept=".json"
          style={{ display: 'none' }}
          onChange={handlePolicyFileChange}
        />
        <div className="prompt-debugger-file-row">
          <button className="settings-reset-button" onClick={() => policyInputRef.current?.click()}>
            Select routing policy JSON
          </button>
          {policyFilename && (
            <span className="prompt-debugger-file-name" title={policyFilename}>
              {policyFilename}
              <button
                className="prompt-debugger-clear-btn"
                onClick={clearPolicy}
                title="Clear selected policy"
                aria-label="Clear selected policy"
              >
                <XIcon size={11} strokeWidth={2} />
              </button>
            </span>
          )}
        </div>
      </div>

      <div className="form-section">
        <label className="form-label">Image (optional)</label>
        <input
          ref={imageInputRef}
          type="file"
          accept="image/*"
          style={{ display: 'none' }}
          onChange={handleImageFileChange}
        />
        <div className="prompt-debugger-file-row">
          <button className="settings-reset-button" onClick={() => imageInputRef.current?.click()}>
            Select image
          </button>
          {imageFilename && (
            <span className="prompt-debugger-file-name" title={imageFilename}>
              {imageThumbnailUrl && (
                <img className="prompt-debugger-thumbnail" src={imageThumbnailUrl} alt="" />
              )}
              {imageFilename}
              <button
                className="prompt-debugger-clear-btn"
                onClick={clearImage}
                title="Clear selected image"
                aria-label="Clear selected image"
              >
                <XIcon size={11} strokeWidth={2} />
              </button>
            </span>
          )}
        </div>
        <span className="settings-description">
          Only whether an image is attached is sent to the server — the image itself stays local.
        </span>
      </div>

      <div className="form-section">
        <label className="settings-checkbox-label">
          <input
            type="checkbox"
            className="settings-checkbox"
            checked={hasTools}
            onChange={(e) => setHasTools(e.target.checked)}
          />
          <div className="settings-checkbox-content">
            <span className="settings-label-text">Tools attached</span>
            <span className="settings-description">
              Simulates a request that included a non-empty tools[] array.
            </span>
          </div>
        </label>
      </div>

      <div className="form-section">
        <label className="form-label">Metadata (optional)</label>
        <textarea
          className="form-input prompt-debugger-textarea"
          placeholder={'key: value\none-per-line'}
          value={metadataText}
          onChange={(e) => {
            setMetadataText(e.target.value);
            adjustTextareaHeight(e.target);
          }}
        />
        <span className="settings-description">
          One key:value pair per line. Sent to the policy as the metadata map.
        </span>
        {metadataError && <div className="prompt-debugger-error">{metadataError}</div>}
      </div>

      <div className="form-section">
        <button
          className="settings-save-button"
          disabled={!policyJson || isValidating || !!metadataError}
          onClick={handleValidate}
        >
          {isValidating ? 'Validating…' : 'Validate routing policy'}
        </button>
      </div>

      {validationError && (
        <div className="prompt-debugger-error">{validationError}</div>
      )}

      {decision && (
        <div className="prompt-debugger-results">
          <div className="prompt-debugger-summary">
            {decision.default_used
              ? <>Routed to <b>{decision.route_to}</b> (default fallback — no rule matched)</>
              : <>Routed to <b>{decision.route_to}</b> via rule "{decision.matched_rule}"</>}
          </div>

          <div className="form-section">
            <button className="settings-reset-button" onClick={() => setShowTreeModal(true)}>
              View decision tree
            </button>
          </div>

          <div className="form-section">
            <label className="form-label">Trace</label>
            <pre className="trace-log">
              {decision.trace.length === 0
                ? '(no trace entries)'
                : decision.trace
                    .map((entry, i) => {
                      const scorePart = entry.score !== undefined ? ` score=${entry.score.toFixed(2)}` : '';
                      return `${i + 1}. ${entry.condition}:${scorePart} result=${entry.result}`;
                    })
                    .join('\n')}
            </pre>
          </div>

          <div className="form-section">
            <button
              className="settings-reset-button"
              onClick={() => downloadTraceFile(decision, prompt)}
            >
              Download trace (.txt)
            </button>
          </div>
        </div>
      )}

      {showTreeModal && decision && policyJson && (
        <div className="settings-overlay" onMouseDown={() => setShowTreeModal(false)}>
          <div className="settings-modal decision-tree-modal" onMouseDown={(e) => e.stopPropagation()}>
            <div className="settings-header">
              <h3>Decision Tree</h3>
              <button className="settings-close-button" onClick={() => setShowTreeModal(false)} title="Close">
                <svg width="14" height="14" viewBox="0 0 14 14">
                  <path d="M 1,1 L 13,13 M 13,1 L 1,13" stroke="currentColor" strokeWidth="2" strokeLinecap="round" />
                </svg>
              </button>
            </div>
            <div className="settings-content">
              <div className="decision-tree-scroll">
                {renderDecisionTree(policyJson, decision)}
              </div>
            </div>
          </div>
        </div>
      )}
    </div>
  );
};

export default PromptDebuggerPanel;
