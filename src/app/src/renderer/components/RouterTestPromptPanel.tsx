import React, { useEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { XIcon } from './Icons';
import { useModels } from '../hooks/useModels';
import { DownloadAbortError, ensureModelReady } from '../utils/backendInstaller';
import { serverFetch } from '../utils/serverConfig';
import { adjustTextareaHeight } from '../utils/textareaUtils';
import {
  DecisionResult,
  RoutingPolicyDoc,
  TraceRequestInputs,
  downloadTraceFile,
  findMatchedRuleIndex,
  formatTraceEntry,
  renderDecisionTree,
} from '../utils/decisionTree';

interface RouterTestPromptPanelProps {
  /** The live policy built from the in-progress Router Builder draft, or
   * null when the draft isn't yet complete enough to build one - see
   * policyUnavailableReason for why. */
  policy: RoutingPolicyDoc | null;
  policyUnavailableReason: string | null;
  /** Models /routing/validate will actually exercise to reach a decision
   * (the llm router model, or classifier models) - ensured downloaded and
   * loaded before each test, same as any other inference pre-flight. */
  requiredModels: string[];
  /** Used to label the policy in trace/tree exports - there's no uploaded
   * filename here, since the policy comes from the live draft. */
  routerName: string;
  showError: (message: string) => void;
  showSuccess: (message: string) => void;
  showWarning: (message: string) => void;
}

/** A decision bundled with the exact request inputs that produced it, so the
 * decision tree and trace export never drift from the request they describe
 * even if the user edits the prompt/flags/metadata again before the response
 * lands. */
interface ValidationResult extends TraceRequestInputs {
  decision: DecisionResult;
  policy: RoutingPolicyDoc;
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

const RouterTestPromptPanel: React.FC<RouterTestPromptPanelProps> = ({
  policy, policyUnavailableReason, requiredModels, routerName, showError, showSuccess, showWarning,
}) => {
  const { modelsData } = useModels();
  const [prompt, setPrompt] = useState('');
  const [imageFilename, setImageFilename] = useState<string | null>(null);
  const [imageThumbnailUrl, setImageThumbnailUrl] = useState<string | null>(null);
  const [hasImages, setHasImages] = useState(false);
  const [hasTools, setHasTools] = useState(false);
  const [metadataText, setMetadataText] = useState('');
  const [metadataError, setMetadataError] = useState<string | null>(null);
  const [isValidating, setIsValidating] = useState(false);
  const [validationError, setValidationError] = useState<string | null>(null);
  const [result, setResult] = useState<ValidationResult | null>(null);
  const [showTreeModal, setShowTreeModal] = useState(false);

  const imageInputRef = useRef<HTMLInputElement>(null);
  const imageThumbnailUrlRef = useRef<string | null>(null);
  imageThumbnailUrlRef.current = imageThumbnailUrl;
  // Bumped on every submit so a response that arrives after being superseded
  // is dropped instead of resurrecting a stale result.
  const latestRequestIdRef = useRef(0);

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

  // `policy` is rebuilt from the Builder tab's draft on every edit (see
  // RouterCollectionPanel's testPolicy memo) - if the user edits a rule while
  // a validation is in flight, that request is now testing a policy that no
  // longer matches what's on screen. Stop waiting on it: bump the request id
  // so its eventual response is dropped as stale, and flip the button back to
  // "Test prompt" immediately instead of leaving it stuck on "Validating…".
  useEffect(() => {
    if (!isValidating) return;
    latestRequestIdRef.current++;
    setIsValidating(false);
  }, [policy]);

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

  const handleViewDecisionTree = () => {
    if (!result) return;
    if (!result.decision.default_used && findMatchedRuleIndex(result.policy, result.decision) === -1) {
      showWarning(
        `Matched rule "${result.decision.matched_rule}" was not found in the loaded policy — the decision tree can't be shown.`
      );
      return;
    }
    setShowTreeModal(true);
  };

  const handleValidate = async () => {
    if (!policy) return;
    const requestId = ++latestRequestIdRef.current;
    const submittedPrompt = prompt;
    const submittedPolicy = policy;
    const submittedHasImages = hasImages;
    const submittedHasTools = hasTools;
    setIsValidating(true);
    setValidationError(null);
    try {
      // Ensure every model this test will actually exercise (the llm router,
      // or classifier models) is downloaded and loaded first - otherwise the
      // classifier just fails open to the default model instead of testing
      // the real decision. Reuses the same download pipeline (and Download
      // Manager popup) as creating a router policy.
      for (const modelName of requiredModels) {
        if (requestId !== latestRequestIdRef.current) return;
        await ensureModelReady(modelName, modelsData);
      }
      if (requestId !== latestRequestIdRef.current) return;

      const { metadata } = parseMetadataText(metadataText);
      const response = await serverFetch('/routing/validate', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          policy: submittedPolicy,
          prompt: submittedPrompt,
          has_images: submittedHasImages,
          has_tools: submittedHasTools,
          ...(Object.keys(metadata).length > 0 ? { metadata } : {}),
        }),
      });
      const data = await response.json();
      if (requestId !== latestRequestIdRef.current) return;
      if (!response.ok) {
        setValidationError(data?.error ?? `HTTP ${response.status}`);
        setResult(null);
        return;
      }
      const newDecision = data.decision as DecisionResult;
      // The submitted policy may only have routing.router (no routing.rules),
      // so decision.matched_rule (a synthesized __route_N id) can't be
      // resolved against it. normalized_policy is what the server actually
      // evaluated — prefer it, falling back for an older server that
      // predates this field.
      const evaluatedPolicy = (data.normalized_policy as RoutingPolicyDoc | undefined) ?? submittedPolicy;
      setResult({
        decision: newDecision,
        policy: evaluatedPolicy,
        prompt: submittedPrompt,
        hasImages: submittedHasImages,
        hasTools: submittedHasTools,
        metadata,
        policyFilename: routerName,
      });
      showSuccess(
        newDecision.default_used
          ? `Routing policy validated — routed to "${newDecision.route_to}" via the default fallback.`
          : `Routing policy validated — routed to "${newDecision.route_to}" via rule "${newDecision.matched_rule}".`
      );
    } catch (error) {
      if (requestId !== latestRequestIdRef.current) return;
      if (error instanceof DownloadAbortError) return; // user paused/cancelled the download - not an error
      setValidationError(error instanceof Error ? error.message : 'Unknown error');
      setResult(null);
    } finally {
      setIsValidating(false);
    }
  };

  return (
    <div className="router-test-prompt-panel">
      <div className="form-section">
        <label className="form-label">Prompt</label>
        <textarea
          className="form-input router-test-prompt-textarea"
          placeholder="Enter a prompt to test against the router..."
          value={prompt}
          onChange={(e) => {
            setPrompt(e.target.value);
            adjustTextareaHeight(e.target);
          }}
        />
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
        <div className="router-test-prompt-file-row">
          <button className="settings-reset-button" onClick={() => imageInputRef.current?.click()}>
            Select image
          </button>
          {imageFilename && (
            <span className="router-test-prompt-file-name" title={imageFilename}>
              {imageThumbnailUrl && (
                <img className="router-test-prompt-thumbnail" src={imageThumbnailUrl} alt="" />
              )}
              {imageFilename}
              <button
                className="router-test-prompt-clear-btn"
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
          className="form-input router-test-prompt-textarea"
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
        {metadataError && <div className="router-test-prompt-error">{metadataError}</div>}
      </div>

      <div className="form-section">
        {policyUnavailableReason && (
          <div className="router-test-prompt-unavailable">{policyUnavailableReason}</div>
        )}
        <button
          className="settings-save-button"
          disabled={!policy || isValidating || !!metadataError}
          onClick={handleValidate}
        >
          {isValidating ? 'Validating…' : 'Test prompt'}
        </button>
      </div>

      {validationError && (
        <div className="router-test-prompt-error">{validationError}</div>
      )}

      {result && (
        <div className="router-test-prompt-results">
          <div className="router-test-prompt-summary">
            {result.decision.default_used
              ? <>Routed to <b>{result.decision.route_to}</b> (default fallback — no rule matched)</>
              : <>Routed to <b>{result.decision.route_to}</b> via rule "{result.decision.matched_rule}"</>}
          </div>

          <div className="form-section">
            <button className="settings-reset-button" onClick={handleViewDecisionTree}>
              View decision tree
            </button>
          </div>

          <div className="form-section">
            <label className="form-label">Trace</label>
            <pre className="trace-log">
              {result.decision.trace.length === 0
                ? '(no trace entries)'
                : result.decision.trace.map((entry, i) => formatTraceEntry(entry, i)).join('\n')}
            </pre>
          </div>

          <div className="form-section">
            <button
              className="settings-reset-button"
              onClick={() => downloadTraceFile(result.decision, result)}
            >
              Download trace (.txt)
            </button>
          </div>
        </div>
      )}

      {showTreeModal && result && createPortal(
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
                {renderDecisionTree(result.policy, result.decision, result)}
              </div>
            </div>
          </div>
        </div>,
        document.body,
      )}
    </div>
  );
};

export default RouterTestPromptPanel;
