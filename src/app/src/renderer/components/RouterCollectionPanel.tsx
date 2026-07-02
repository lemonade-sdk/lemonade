import React, { useEffect, useMemo, useRef, useState } from 'react';
import { useModels } from '../hooks/useModels';
import { getModelDisplayName } from '../utils/modelDisplayName';
import { serverFetch } from '../utils/serverConfig';
import {
  RouterClassifier,
  RouterCollectionDraft,
  RouterRule,
  buildRouterCollectionPullRequest,
  getRouterCandidateOptions,
  routingToRouterCollectionDraft,
} from '../utils/customCollections';
import { isCollectionRecipe } from '../utils/recipeNames';
import { isLeaf, isOperatorNode, validateRuleNode } from '../utils/routerTree';
import RouterPipelineCanvas from './RouterPipelineCanvas';
import { ModelCheckboxList, ModelSelect, type ModelOption } from './ModelSearchPicker';

interface RouterCollectionPanelProps {
  mode: 'create' | 'edit';
  collectionId?: string;
  onClose: () => void;
  onSave: (collection: RouterCollectionDraft) => void | Promise<void>;
  onExport: (collection: RouterCollectionDraft) => void;
}

const DEFAULT_ROUTER_NAME = 'MyHybridRouter';
const DEFAULT_ROUTER_PROMPT =
  'You route user requests to the best model. Reply with ONLY the exact model name, nothing else.';

const emptyDraft = (): RouterCollectionDraft => ({
  name: DEFAULT_ROUTER_NAME,
  candidates: [],
  defaultModel: '',
  routingMode: 'llm',
  routerModel: '',
  routerPrompt: DEFAULT_ROUTER_PROMPT,
  classifiers: [],
  rules: [],
});

// Remove all references to a classifier id from a conditionTree
function stripClassifierFromTree(
  node: RouterRule['conditionTree'],
  classifierId: string,
): RouterRule['conditionTree'] {
  if (!node) return null;
  if (isLeaf(node)) {
    return node.signalType === 'classifier' && node.classifierId === classifierId ? null : node;
  }
  if (isOperatorNode(node)) {
    const cleaned = node.conditions
      .map(c => stripClassifierFromTree(c, classifierId))
      .filter((c): c is NonNullable<typeof c> => c !== null);
    if (cleaned.length === 0) return null;
    if (node.operator !== 'NOT' && cleaned.length === 1) return cleaned[0];
    return { ...node, conditions: cleaned };
  }
  return node;
}

const RouterCollectionPanel: React.FC<RouterCollectionPanelProps> = ({
  mode, collectionId, onClose, onSave, onExport,
}) => {
  const { modelsData } = useModels();
  const [draft, setDraft] = useState<RouterCollectionDraft>(() => emptyDraft());
  const [error, setError] = useState<string | null>(null);
  const [previewJson, setPreviewJson] = useState<string | null>(null);
  const ruleSeqRef = useRef(0);
  const clfSeqRef = useRef(0);

  const candidateOptions = useMemo(() => getRouterCandidateOptions(modelsData), [modelsData]);

  const embeddingOptions = useMemo(() =>
    Object.entries(modelsData)
      .filter(([, info]) => (info?.labels ?? []).includes('embeddings') && !isCollectionRecipe(info?.recipe))
      .map(([id, info]) => ({ id, info }))
      .sort((a, b) => {
        const dl = Number(b.info.downloaded === true) - Number(a.info.downloaded === true);
        return dl !== 0 ? dl : (a.info.model_name ?? a.id).localeCompare(b.info.model_name ?? b.id);
      }),
  [modelsData]);

  useEffect(() => {
    setError(null);
    setPreviewJson(null);
    if (mode !== 'edit' || !collectionId) { setDraft(emptyDraft()); return; }
    serverFetch(`/v1/models/${encodeURIComponent(collectionId)}`)
      .then(r => r.json())
      .then((raw: unknown) => {
        if (!raw || typeof raw !== 'object' || Array.isArray(raw)) {
          setError('Could not load router policy. Refresh and try again.'); return;
        }
        const rec = raw as Record<string, unknown>;
        if (!rec.routing || typeof rec.routing !== 'object') {
          setError('This model has no routing policy. Refresh and try again.'); return;
        }
        setDraft(routingToRouterCollectionDraft(
          collectionId,
          rec.routing as Record<string, unknown>,
          Array.isArray(rec.components) ? rec.components as string[] : [],
        ));
      })
      .catch(() => setError('Failed to load router policy from server.'));
  }, [mode, collectionId]);

  const nextRuleId = () => {
    const existing = new Set((draft.rules ?? []).map(r => r.id));
    let n = ruleSeqRef.current + 1;
    while (existing.has(`rule-${n}`)) n++;
    ruleSeqRef.current = n;
    return `rule-${n}`;
  };

  const nextClassifierId = () => {
    const existing = new Set((draft.classifiers ?? []).map(c => c.id));
    let n = clfSeqRef.current + 1;
    while (existing.has(`clf-${n}`)) n++;
    clfSeqRef.current = n;
    return `clf-${n}`;
  };

  const patch = (p: Partial<RouterCollectionDraft>) => {
    setDraft(prev => ({ ...prev, ...p }));
    setError(null);
    setPreviewJson(null);
  };

  const toggleCandidate = (id: string) => {
    setDraft(prev => {
      const next = prev.candidates.includes(id)
        ? prev.candidates.filter(c => c !== id)
        : [...prev.candidates, id];
      const defaultModel = next.includes(prev.defaultModel) ? prev.defaultModel : '';
      const rules = (prev.rules ?? []).filter(r => next.includes(r.routeTo));
      return { ...prev, candidates: next, defaultModel, rules };
    });
    setError(null);
  };

  const addClassifier = () => {
    const id = nextClassifierId();
    setDraft(prev => ({
      ...prev,
      classifiers: [...(prev.classifiers ?? []),
        { id, type: 'classifier', model: '', labels: [], defaultLabel: '', onError: 'match_false' }],
    }));
  };

  const removeClassifier = (id: string) => {
    setDraft(prev => ({
      ...prev,
      classifiers: (prev.classifiers ?? []).filter(c => c.id !== id),
      rules: (prev.rules ?? []).map(r => ({
        ...r,
        conditionTree: stripClassifierFromTree(r.conditionTree, id),
      })),
    }));
  };

  const patchClassifier = (id: string, p: Partial<RouterClassifier>) => {
    setDraft(prev => ({
      ...prev,
      classifiers: (prev.classifiers ?? []).map(c => c.id === id ? { ...c, ...p } : c),
    }));
    setError(null);
  };

  const addRule = () => {
    if (draft.candidates.length === 0) {
      setError('Select at least one candidate model before adding rules.');
      return;
    }
    const id = nextRuleId();
    setDraft(prev => ({
      ...prev,
      rules: [...(prev.rules ?? []), { id, routeTo: prev.candidates[0] ?? '', conditionTree: null }],
    }));
  };

  const removeRule = (id: string) => {
    setDraft(prev => ({ ...prev, rules: (prev.rules ?? []).filter(r => r.id !== id) }));
  };

  const patchRule = (id: string, p: Partial<RouterRule>) => {
    setDraft(prev => ({
      ...prev,
      rules: (prev.rules ?? []).map(r => r.id === id ? { ...r, ...p } : r),
    }));
    setError(null);
  };

  const validate = (): RouterCollectionDraft | null => {
    const name = draft.name.trim();
    if (!name) { setError('Router name is required.'); return null; }
    if (draft.candidates.length === 0) { setError('Select at least one candidate model.'); return null; }
    if (!draft.defaultModel) { setError('Select a default model from the candidates.'); return null; }
    if (!draft.candidates.includes(draft.defaultModel)) {
      setError('Default model must be one of the selected candidates.'); return null;
    }
    if (draft.routingMode === 'llm') {
      if (!draft.routerModel) { setError('Select a router LLM.'); return null; }
      if (!draft.routerPrompt?.trim()) { setError('Enter a routing prompt.'); return null; }
    } else {
      for (const c of draft.classifiers ?? []) {
        if (!c.id.trim()) { setError('Each classifier needs an id.'); return null; }
        if (!c.model) { setError(`Classifier "${c.id}": select a model.`); return null; }
        if (c.type === 'llm' && !c.prompt?.trim()) {
          setError(`Classifier "${c.id}": enter a routing prompt.`); return null;
        }
        if (c.type === 'semantic_similarity') {
          const concepts = Object.keys(c.referencePhrases ?? {});
          if (!concepts.length) { setError(`Classifier "${c.id}": add at least one concept.`); return null; }
          for (const k of concepts) {
            if (!(c.referencePhrases![k]?.length)) {
              setError(`Classifier "${c.id}" concept "${k}": add at least one phrase.`); return null;
            }
          }
        }
      }
      if (!draft.rules?.length) { setError('Add at least one rule.'); return null; }
      const classifierIds = new Set((draft.classifiers ?? []).map(c => c.id));
      for (const r of draft.rules) {
        if (!r.routeTo) { setError(`Rule "${r.id}": select a target model.`); return null; }
        if (!draft.candidates.includes(r.routeTo)) {
          setError(`Rule "${r.id}": target model must be a candidate.`); return null;
        }
        if (!r.conditionTree) { setError(`Rule "${r.id}": add at least one condition.`); return null; }
        const treeErrors = validateRuleNode(r.conditionTree, classifierIds);
        if (treeErrors.length) { setError(`Rule "${r.id}": ${treeErrors[0]}`); return null; }
      }
    }
    return { ...draft, name };
  };

  const handleSave = async () => {
    const d = validate();
    if (!d) return;
    void onSave(d);
  };

  const handleExport = () => {
    const d = validate();
    if (!d) return;
    onExport(d);
  };

  const handlePreview = () => {
    if (previewJson !== null) { setPreviewJson(null); return; }
    const d = validate();
    if (!d) return;
    try {
      setPreviewJson(JSON.stringify(buildRouterCollectionPullRequest(d), null, 2));
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Failed to build JSON.');
    }
  };

  const displayName = (id: string) => modelsData[id]?.model_name ?? getModelDisplayName(id);
  const displayNameWithStatus = (id: string) => {
    const info = modelsData[id];
    return `${info?.model_name ?? getModelDisplayName(id)} (${info?.downloaded === true ? 'downloaded' : 'registered - will download'})`;
  };

  return (
    <>
      <div className="settings-header">
        <h3>{mode === 'edit' ? 'Edit Hybrid Router' : 'New Hybrid Router'}</h3>
        <button type="button" className="settings-close-button" onClick={onClose} title="Close">
          <svg width="14" height="14" viewBox="0 0 14 14">
            <path d="M 1,1 L 13,13 M 13,1 L 1,13" stroke="currentColor" strokeWidth="2" strokeLinecap="round" />
          </svg>
        </button>
      </div>

      <div className="settings-content custom-collection-content">

        <div className="form-section">
          <label className="form-label">Router Name *</label>
          <input type="text" className="form-input"
            value={draft.name}
            onChange={e => patch({ name: e.target.value.replace(/^user\./, '') })}
            placeholder="MyHybridRouter" />
        </div>

        <div className="form-section">
          <label className="form-label">
            Candidate Models *
            <span className="settings-description" style={{ marginLeft: 6 }}>- the LLMs that can answer requests</span>
          </label>
          {candidateOptions.length === 0 ? (
            <div className="collection-role-empty">No compatible models found. Pull or register LLM models first.</div>
          ) : (
            <ModelCheckboxList
              options={candidateOptions.map(({ id, info }) => ({
                id,
                label: info.model_name ?? getModelDisplayName(id),
                sublabel: info.downloaded === true ? 'local' : 'will download',
                downloaded: info.downloaded === true,
              }) satisfies ModelOption)}
              selected={draft.candidates}
              onToggle={toggleCandidate}
            />
          )}
        </div>

        <div className="form-section">
          <label className="form-label">
            Default Model *
            <span className="settings-description" style={{ marginLeft: 6 }}>- fallback when no rule matches</span>
          </label>
          <ModelSelect
            options={draft.candidates.map(id => ({ id, label: displayName(id) }))}
            value={draft.defaultModel}
            onChange={id => patch({ defaultModel: id })}
            placeholder={draft.candidates.length === 0 ? 'Select candidates first' : 'Select default model…'}
            disabled={draft.candidates.length === 0}
          />
        </div>

        <div className="form-section">
          <label className="form-label">Routing Mode</label>
          <div className="router-mode-options">
            <label className="router-mode-option">
              <input type="radio" name="routingMode" value="llm" checked={draft.routingMode === 'llm'} onChange={() => patch({ routingMode: 'llm' })} />
              <span><strong>NL Router</strong><span className="settings-description" style={{ display: 'block' }}>A small LLM reads your prompt and picks the best candidate.</span></span>
            </label>
            <label className="router-mode-option">
              <input type="radio" name="routingMode" value="rules" checked={draft.routingMode === 'rules'} onChange={() => patch({ routingMode: 'rules' })} />
              <span><strong>Rules</strong><span className="settings-description" style={{ display: 'block' }}>Visual boolean expression builder with drag-and-drop conditions.</span></span>
            </label>
          </div>
        </div>

        {draft.routingMode === 'llm' && (
          <>
            <div className="form-section">
              <label className="form-label">
                Router LLM *
                <span className="settings-description" style={{ marginLeft: 6 }}>- small model that reads your prompt</span>
              </label>
              <ModelSelect
                options={candidateOptions.map(({ id }) => ({ id, label: displayNameWithStatus(id) }))}
                value={draft.routerModel ?? ''}
                onChange={id => patch({ routerModel: id })}
                placeholder="Select a router LLM…"
                annotate={id => draft.candidates.includes(id) ? '(also a candidate)' : null}
              />
            </div>
            <div className="form-section">
              <label className="form-label">Routing Prompt *</label>
              <textarea className="form-input" rows={5} value={draft.routerPrompt ?? ''}
                onChange={e => patch({ routerPrompt: e.target.value })}
                placeholder={DEFAULT_ROUTER_PROMPT} spellCheck={false}
                style={{ fontFamily: 'monospace', fontSize: '12px', resize: 'vertical' }} />
              <span className="settings-description" style={{ display: 'block', marginTop: 4 }}>
                Tell the router LLM which model to pick and when. End with: &ldquo;Reply with ONLY the exact model name.&rdquo;
              </span>
              {draft.candidates.length > 0 && (
                <span className="settings-description" style={{ display: 'block', marginTop: 4 }}>
                  Candidates: {draft.candidates.join(', ')}
                </span>
              )}
            </div>
          </>
        )}

        {draft.routingMode === 'rules' && (
          <RouterPipelineCanvas
            draft={draft}
            candidateOptions={candidateOptions}
            embeddingOptions={embeddingOptions}
            displayName={displayName}
            displayNameWithStatus={displayNameWithStatus}
            onPatchClassifier={patchClassifier}
            onAddClassifier={addClassifier}
            onRemoveClassifier={removeClassifier}
            onPatchRule={patchRule}
            onAddRule={addRule}
            onRemoveRule={removeRule}
            highlightedClassifierId={null}
            previewJson={previewJson}
            onPreviewJson={handlePreview}
          />
        )}

      </div>

      {previewJson !== null && (
        <div className="router-json-preview">
          <div className="router-json-preview-header">
            <span className="router-json-preview-title">Generated JSON</span>
            <button type="button" className="router-json-preview-btn" title="Copy"
              onClick={() => navigator.clipboard.writeText(previewJson)}>
              <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                <rect x="9" y="9" width="13" height="13" rx="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/>
              </svg>
            </button>
            <button type="button" className="router-json-preview-btn" title="Download"
              onClick={() => {
                const blob = new Blob([previewJson], { type: 'application/json' });
                const url = URL.createObjectURL(blob);
                const a = document.createElement('a');
                a.href = url; a.download = `${draft.name.trim() || 'router'}.json`; a.click();
                URL.revokeObjectURL(url);
              }}>
              <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/>
              </svg>
            </button>
            <button type="button" className="router-json-preview-btn" title="Close" onClick={() => setPreviewJson(null)}>
              <svg width="12" height="12" viewBox="0 0 12 12" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round">
                <path d="M1 1 L11 11 M11 1 L1 11"/>
              </svg>
            </button>
          </div>
          <pre className="router-json-preview-body">{previewJson}</pre>
        </div>
      )}

      {error && <div className="router-panel-error">{error}</div>}

      <div className="settings-footer custom-collection-footer">
        <button type="button" className="settings-reset-button" onClick={handlePreview}>
          {previewJson !== null ? 'Hide JSON' : 'Preview JSON'}
        </button>
        <button type="button" className="settings-reset-button" onClick={handleExport}>Export</button>
        <button type="button" className="settings-reset-button" onClick={onClose}>Cancel</button>
        <button type="button" className="settings-save-button" onClick={handleSave}>
          {mode === 'edit' ? 'Save' : 'Create'}
        </button>
      </div>
    </>
  );
};

export default RouterCollectionPanel;
