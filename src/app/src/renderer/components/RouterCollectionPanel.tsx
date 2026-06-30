import React, { useEffect, useMemo, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { useModels } from '../hooks/useModels';
import { getModelDisplayName } from '../utils/modelDisplayName';
import { serverFetch } from '../utils/serverConfig';
import {
  RouterClassifier,
  RouterCollectionDraft,
  RouterRule,
  RouterKeywordEntry,
  RouterRegexEntry,
  buildRouterCollectionPullRequest,
  getRouterCandidateOptions,
  routingToRouterCollectionDraft,
} from '../utils/customCollections';
import { isCollectionRecipe } from '../utils/recipeNames';

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

const slugify = (s: string) =>
  s.trim().toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '') || 'clf';

const classifierChipLines = (clf: RouterClassifier): string[] => {
  const lines: string[] = [`${clf.type} · ${displayNameShort(clf.model) || 'no model'}`];
  if (clf.type === 'semantic_similarity') {
    const concepts = Object.keys(clf.referencePhrases ?? {});
    if (concepts.length) lines.push(`concepts: ${concepts.join(', ')}`);
  } else if (clf.type === 'classifier') {
    if (clf.labels?.length) lines.push(`labels: ${clf.labels.slice(0, 3).join(', ')}`);
    if (clf.onError === 'match_true') lines.push('fail-closed');
  } else if (clf.type === 'llm') {
    if (clf.prompt) lines.push(clf.prompt.slice(0, 40) + (clf.prompt.length > 40 ? '…' : ''));
  }
  return lines;
};

const ruleChipLines = (rule: RouterRule): string[] => {
  const lines: string[] = [`→ ${displayNameShort(rule.routeTo) || 'no target'}`];
  const kwAny = (rule.matchKeywordsAny ?? []).flatMap(e => e.keywords);
  if (kwAny.length) lines.push(`kw-any: ${kwAny.slice(0, 3).join(', ')}${kwAny.length > 3 ? '…' : ''}`);
  const kwAll = (rule.matchKeywordsAll ?? []).flatMap(e => e.keywords);
  if (kwAll.length) lines.push(`kw-all: ${kwAll.slice(0, 2).join(', ')}${kwAll.length > 2 ? '…' : ''}`);
  if (rule.matchClassifier?.classifierId) lines.push(`clf: ${rule.matchClassifier.classifierId}`);
  if (rule.matchMinChars) lines.push(`≥${rule.matchMinChars} chars`);
  if (rule.matchMaxChars) lines.push(`≤${rule.matchMaxChars} chars`);
  return lines;
};

function displayNameShort(id: string): string {
  if (!id) return '';
  const parts = id.split(/[/:-]/);
  return parts[parts.length - 1] ?? id;
}


const RouterCollectionPanel: React.FC<RouterCollectionPanelProps> = ({
  mode,
  collectionId,
  onClose,
  onSave,
  onExport,
}) => {
  const { modelsData } = useModels();
  const [draft, setDraft] = useState<RouterCollectionDraft>(() => emptyDraft());
  const [error, setError] = useState<string | null>(null);
  const [previewJson, setPreviewJson] = useState<string | null>(null);
  const [detailPanel, setDetailPanel] = useState<
    { kind: 'classifier'; id: string } | { kind: 'rule'; id: string } | null
  >(null);
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
    if (mode !== 'edit' || !collectionId) {
      setDraft(emptyDraft());
      return;
    }
    serverFetch(`/v1/models/${encodeURIComponent(collectionId)}`)
      .then((r) => r.json())
      .then((raw: unknown) => {
        if (!raw || typeof raw !== 'object' || Array.isArray(raw)) {
          setError('Could not load router policy. Refresh and try again.');
          return;
        }
        const rec = raw as Record<string, unknown>;
        if (!rec.routing || typeof rec.routing !== 'object') {
          setError('This model has no routing policy. Refresh and try again.');
          return;
        }
        setDraft(routingToRouterCollectionDraft(
          collectionId,
          rec.routing as Record<string, unknown>,
          Array.isArray(rec.components) ? rec.components as string[] : [],
        ));
      })
      .catch(() => setError('Failed to load router policy from server.'));
  }, [mode, collectionId]);

  const closeDetail = () => setDetailPanel(null);

  const nextRuleId = () => {
    const existing = new Set((draft.rules ?? []).map((r) => r.id));
    let n = ruleSeqRef.current + 1;
    while (existing.has(`rule-${n}`)) n++;
    ruleSeqRef.current = n;
    return `rule-${n}`;
  };

  const nextClassifierId = () => {
    const existing = new Set((draft.classifiers ?? []).map((c) => c.id));
    let n = clfSeqRef.current + 1;
    while (existing.has(`clf-${n}`)) n++;
    clfSeqRef.current = n;
    return `clf-${n}`;
  };

  const patch = (p: Partial<RouterCollectionDraft>) => {
    setDraft((prev) => ({ ...prev, ...p }));
    setError(null);
    setPreviewJson(null);
  };

  const toggleCandidate = (id: string) => {
    setDraft((prev) => {
      const next = prev.candidates.includes(id)
        ? prev.candidates.filter((c) => c !== id)
        : [...prev.candidates, id];
      const defaultModel = next.includes(prev.defaultModel) ? prev.defaultModel : '';
      const rules = (prev.rules ?? []).filter((r) => next.includes(r.routeTo));
      return { ...prev, candidates: next, defaultModel, rules };
    });
    setError(null);
  };

  const addClassifier = () => {
    const id = nextClassifierId();
    setDraft((prev) => ({
      ...prev,
      classifiers: [
        ...(prev.classifiers ?? []),
        { id, type: 'classifier', model: '', labels: [], defaultLabel: '', onError: 'match_false' },
      ],
    }));
    setDetailPanel({ kind: 'classifier', id });
  };

  const removeClassifier = (id: string) => {
    setDraft((prev) => ({
      ...prev,
      classifiers: (prev.classifiers ?? []).filter((c) => c.id !== id),
      rules: (prev.rules ?? []).map((r) =>
        r.matchClassifier?.classifierId === id ? { ...r, matchClassifier: undefined } : r,
      ),
    }));
  };

  const patchClassifier = (id: string, p: Partial<RouterClassifier>) => {
    setDraft((prev) => ({
      ...prev,
      classifiers: (prev.classifiers ?? []).map((c) => (c.id === id ? { ...c, ...p } : c)),
    }));
    setError(null);
  };

  const addRule = () => {
    if (draft.candidates.length === 0) {
      setError('Select at least one candidate model before adding rules.');
      return;
    }
    const id = nextRuleId();
    setDraft((prev) => ({
      ...prev,
      rules: [
        ...(prev.rules ?? []),
        { id, routeTo: prev.candidates[0] ?? '' },
      ],
    }));
    setDetailPanel({ kind: 'rule', id });
  };

  const removeRule = (id: string) => {
    setDraft((prev) => ({ ...prev, rules: (prev.rules ?? []).filter((r) => r.id !== id) }));
  };

  const patchRule = (id: string, p: Partial<RouterRule>) => {
    setDraft((prev) => ({
      ...prev,
      rules: (prev.rules ?? []).map((r) => (r.id === id ? { ...r, ...p } : r)),
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
        if (c.type === 'llm') {
          if (!c.prompt?.trim()) {
            setError(`Classifier "${c.id}": enter a routing prompt.`); return null;
          }
        }
        if (c.type === 'semantic_similarity') {
          const concepts = Object.keys(c.referencePhrases ?? {});
          if (concepts.length === 0) {
            setError(`Classifier "${c.id}": add at least one concept.`); return null;
          }
          for (const k of concepts) {
            if (!(c.referencePhrases![k]?.length)) {
              setError(`Classifier "${c.id}" concept "${k}": add at least one phrase.`); return null;
            }
          }
        }
      }
      if (!draft.rules?.length) { setError('Add at least one rule.'); return null; }
      const classifierIds = new Set((draft.classifiers ?? []).map((c) => c.id));
      for (const r of draft.rules) {
        if (!r.routeTo) { setError(`Rule "${r.id}": select a target model.`); return null; }
        if (!draft.candidates.includes(r.routeTo)) {
          setError(`Rule "${r.id}": target model must be a candidate.`); return null;
        }
        const hasKw = (r.matchKeywordsAny ?? []).some(e => e.keywords?.length > 0);
        const hasKwAll = (r.matchKeywordsAll ?? []).some(e => e.keywords?.length > 0);
        const hasRegex = (r.matchRegex ?? []).some(e => e.pattern?.trim());
        const hasMin = r.matchMinChars !== undefined;
        const hasMax = r.matchMaxChars !== undefined;
        const hasTools = r.matchHasTools !== undefined;
        const hasImages = r.matchHasImages !== undefined;
        const hasClf = !!r.matchClassifier?.classifierId;
        if (!hasKw && !hasKwAll && !hasRegex && !hasMin && !hasMax && !hasTools && !hasImages && !hasClf) {
          setError(`Rule "${r.id}": add at least one condition.`); return null;
        }
        if (hasClf && !classifierIds.has(r.matchClassifier!.classifierId)) {
          setError(`Rule "${r.id}": references unknown classifier "${r.matchClassifier!.classifierId}".`);
          return null;
        }
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

  const displayName = (id: string) => {
    const info = modelsData[id];
    return info?.model_name ?? getModelDisplayName(id);
  };

  const displayNameWithStatus = (id: string) => {
    const info = modelsData[id];
    const name = info?.model_name ?? getModelDisplayName(id);
    return `${name} (${info?.downloaded === true ? 'downloaded' : 'registered - will download'})`;
  };

  const classifiers = draft.classifiers ?? [];
  const rules = draft.rules ?? [];

  const activeConditionCount = (r: RouterRule) =>
    [
      (r.matchKeywordsAny ?? []).some(e => e.keywords?.length > 0),
      (r.matchKeywordsAll ?? []).some(e => e.keywords?.length > 0),
      (r.matchRegex ?? []).some(e => e.pattern?.trim()),
      r.matchMinChars !== undefined,
      r.matchMaxChars !== undefined,
      r.matchHasTools !== undefined,
      r.matchHasImages !== undefined,
      !!r.matchClassifier?.classifierId,
    ].filter(Boolean).length;


  const renderClassifierDetail = (clf: RouterClassifier) => {
    if (!clf) return null;
    const parsedLabels = (clf.labels ?? []).filter(Boolean);
    return (
      <div className="custom-collection-content" style={{ padding: '16px 24px', display: 'flex', flexDirection: 'column', gap: 12 }}>
        <div className="router-classifier-field">
          <label className="form-label" style={{ fontSize: '11px' }}>ID *</label>
          <input type="text" className="form-input" style={{ fontSize: '12px' }}
            value={clf.id}
            onChange={(e) => patchClassifier(clf.id, { id: slugify(e.target.value) })}
            placeholder="e.g. pii" />
        </div>

        <div className="router-classifier-field">
          <label className="form-label" style={{ fontSize: '11px' }}>Type *</label>
          <select className="form-input form-select" style={{ fontSize: '12px' }}
            value={clf.type}
            onChange={(e) => patchClassifier(clf.id, {
              type: e.target.value as RouterClassifier['type'],
              labels: [], defaultLabel: '', referencePhrases: {}, prompt: undefined,
            })}>
            <option value="classifier">Classifier</option>
            <option value="semantic_similarity">Semantic Similarity</option>
            <option value="llm">LLM</option>
          </select>
        </div>

        <div className="router-classifier-field">
          <label className="form-label" style={{ fontSize: '11px' }}>Model *</label>
          <select className="form-input form-select" style={{ fontSize: '12px' }}
            value={clf.model}
            onChange={(e) => patchClassifier(clf.id, { model: e.target.value })}>
            <option value="">Select a model…</option>
            {(clf.type === 'semantic_similarity' ? embeddingOptions : candidateOptions).map(({ id }) => (
              <option key={id} value={id}>{displayNameWithStatus(id)}</option>
            ))}
          </select>
          {clf.type === 'semantic_similarity' && embeddingOptions.length === 0 && (
            <span className="settings-description" style={{ display: 'block', fontSize: '0.68rem', marginTop: 3 }}>
              No embedding models found. Pull one first (e.g. nomic-embed-text-v1.5-GGUF).
            </span>
          )}
        </div>

        {clf.type === 'llm' && (
          <div className="router-classifier-field">
            <label className="form-label" style={{ fontSize: '11px' }}>Prompt *</label>
            <textarea className="form-input" rows={4}
              style={{ fontSize: '12px', resize: 'vertical', fontFamily: 'monospace' }}
              defaultValue={clf.prompt ?? ''}
              onBlur={(e) => patchClassifier(clf.id, { prompt: e.target.value || undefined })}
              placeholder={'Classify this request.\nReply with ONLY one of: SAFE, RISKY'} />
          </div>
        )}

        {clf.type === 'classifier' && (
          <>
            <div className="router-classifier-field">
              <label className="form-label" style={{ fontSize: '11px' }}>
                Labels
                <span className="settings-description" style={{ marginLeft: 4 }}>(one per line)</span>
              </label>
              <textarea className="form-input" rows={3}
                style={{ fontSize: '12px', resize: 'vertical', fontFamily: 'monospace' }}
                defaultValue={(clf.labels ?? []).join('\n')}
                onBlur={(e) => {
                  const labels = e.target.value.split('\n').map((s) => s.trim()).filter(Boolean);
                  const defaultLabel = labels.includes(clf.defaultLabel ?? '') ? clf.defaultLabel : '';
                  patchClassifier(clf.id, { labels, defaultLabel });
                }}
                placeholder={'PII\nNO_PII'} />
            </div>

            {parsedLabels.length > 0 && (
              <div className="router-classifier-field">
                <label className="form-label" style={{ fontSize: '11px' }}>
                  Default Label
                  <span className="settings-description" style={{ marginLeft: 4 }}>- used when rule omits label</span>
                </label>
                <select className="form-input form-select" style={{ fontSize: '12px' }}
                  value={clf.defaultLabel ?? ''}
                  onChange={(e) => patchClassifier(clf.id, { defaultLabel: e.target.value })}>
                  <option value="">None</option>
                  {parsedLabels.map((l) => <option key={l} value={l}>{l}</option>)}
                </select>
              </div>
            )}

            <div className="router-classifier-field">
              <label className="form-label" style={{ fontSize: '11px' }}>On Error</label>
              <select className="form-input form-select" style={{ fontSize: '12px' }}
                value={clf.onError ?? 'match_false'}
                onChange={(e) => patchClassifier(clf.id, { onError: e.target.value as RouterClassifier['onError'] })}>
                <option value="match_false">match_false - fail-open (default)</option>
                <option value="match_true">match_true - fail-closed (safer for security)</option>
              </select>
            </div>
          </>
        )}

        {clf.type === 'semantic_similarity' && (
          <div className="router-classifier-field">
            <div className="router-classifier-section-header" style={{ marginBottom: 4 }}>
              <label className="form-label" style={{ fontSize: '11px', margin: 0 }}>
                Concepts *
                <span className="settings-description" style={{ marginLeft: 4 }}>
                  - each concept becomes an output label scored by cosine similarity
                </span>
              </label>
              <button type="button" className="settings-reset-button"
                style={{ fontSize: '11px', padding: '1px 8px' }}
                onClick={() => {
                  const existing = clf.referencePhrases ?? {};
                  const newKey = `concept-${Object.keys(existing).length + 1}`;
                  patchClassifier(clf.id, { referencePhrases: { ...existing, [newKey]: [] } });
                }}>
                + Add Concept
              </button>
            </div>
            {Object.entries(clf.referencePhrases ?? {}).map(([concept, phrases]) => (
              <div key={concept} className="router-classifier-card" style={{ marginBottom: 4 }}>
                <div className="router-classifier-header">
                  <input type="text" className="form-input"
                    style={{ fontSize: '12px', fontFamily: 'monospace', flex: 1 }}
                    defaultValue={concept}
                    onBlur={(e) => {
                      const newName = e.target.value.trim();
                      if (!newName || newName === concept) return;
                      const existing = { ...clf.referencePhrases };
                      const phrs = existing[concept];
                      delete existing[concept];
                      existing[newName] = phrs;
                      patchClassifier(clf.id, { referencePhrases: existing });
                    }}
                    placeholder="concept name (= output label)" />
                  <button type="button" className="settings-reset-button"
                    style={{ fontSize: '11px', padding: '1px 8px', marginLeft: 6 }}
                    onClick={() => {
                      const existing = { ...clf.referencePhrases };
                      delete existing[concept];
                      patchClassifier(clf.id, { referencePhrases: existing });
                    }}>
                    Remove
                  </button>
                </div>
                <div className="router-classifier-field">
                  <label className="form-label" style={{ fontSize: '10px' }}>
                    Phrases <span className="settings-description" style={{ marginLeft: 4 }}>(one per line)</span>
                  </label>
                  <textarea className="form-input" rows={3}
                    style={{ fontSize: '12px', resize: 'vertical', fontFamily: 'monospace' }}
                    defaultValue={phrases.join('\n')}
                    onBlur={(e) => {
                      const updated = e.target.value.split('\n').map((s) => s.trim()).filter(Boolean);
                      patchClassifier(clf.id, { referencePhrases: { ...clf.referencePhrases, [concept]: updated } });
                    }}
                    placeholder={'how do I return my order\ntrack my package\nrefund policy'} />
                </div>
              </div>
            ))}
            {Object.keys(clf.referencePhrases ?? {}).length === 0 && (
              <div className="collection-role-empty" style={{ marginTop: 4 }}>
                No concepts yet. Click "+ Add Concept" to create one.
              </div>
            )}
          </div>
        )}
      </div>
    );
  };

  const renderRuleDetail = (rule: RouterRule) => {
    if (!rule) return null;
    const count = activeConditionCount(rule);
    const multi = count >= 2;
    return (
      <div className="custom-collection-content" style={{ padding: '16px 24px', display: 'flex', flexDirection: 'column', gap: 12 }}>
        <div className="router-rule-field">
          <label className="form-label" style={{ fontSize: '11px' }}>Rule ID</label>
          <input type="text" className="form-input router-rule-id-input"
            value={rule.id}
            onChange={(e) => {
              const newId = slugify(e.target.value) || rule.id;
              setDraft((prev) => ({
                ...prev,
                rules: (prev.rules ?? []).map((r) => r.id === rule.id ? { ...r, id: newId } : r),
              }));
              // keep detailPanel in sync with new id
              setDetailPanel({ kind: 'rule', id: newId });
            }}
            placeholder={rule.id}
            title="Rule ID - used in routing decisions and audit trace" />
        </div>

        <div className="router-rule-field">
          <label className="form-label" style={{ fontSize: '11px' }}>Route to *</label>
          <select className="form-input form-select" style={{ fontSize: '12px' }}
            value={rule.routeTo}
            onChange={(e) => patchRule(rule.id, { routeTo: e.target.value })}>
            <option value="">Select candidate…</option>
            {draft.candidates.map((id) => (
              <option key={id} value={id}>{displayName(id)}</option>
            ))}
          </select>
        </div>

        <div className="router-rule-field">
          <label className="form-label" style={{ fontSize: '11px' }}>
            Outputs
            <span className="settings-description" style={{ marginLeft: 4 }}>- optional JSON passed to the decision</span>
          </label>
          <input type="text" className="form-input"
            style={{ fontSize: '12px', fontFamily: 'monospace' }}
            defaultValue={rule.outputs ? JSON.stringify(rule.outputs) : ''}
            onBlur={(e) => {
              const raw = e.target.value.trim();
              if (!raw) { patchRule(rule.id, { outputs: undefined }); return; }
              try { patchRule(rule.id, { outputs: JSON.parse(raw) }); } catch { /* keep last valid */ }
            }}
            placeholder='{"verdict":"warn"}' />
        </div>

        <div className="router-rule-conditions-label">
          <span className="form-label" style={{ fontSize: '11px' }}>Conditions</span>
        </div>

        {/* Operator */}
        <div className="router-rule-field">
          <label className="form-label" style={{ fontSize: '11px', opacity: multi ? 1 : 0.45 }}>Match when</label>
          <div className="router-rule-operator">
            {(['any', 'all'] as const).map((op) => (
              <label key={op} className="router-mode-option" style={{ fontSize: '12px', opacity: multi ? 1 : 0.45, cursor: multi ? 'pointer' : 'not-allowed' }}>
                <input type="radio" name={`op-detail-${rule.id}`} value={op}
                  checked={(rule.operator ?? 'any') === op}
                  disabled={!multi}
                  onChange={() => patchRule(rule.id, { operator: op })} />
                <span>{op === 'any' ? 'Any condition matches (OR)' : 'All conditions match (AND)'}</span>
              </label>
            ))}
          </div>
          {!multi && (
            <span className="settings-description" style={{ display: 'block', fontSize: '0.66rem', marginTop: 3 }}>
              Set 2 or more conditions to combine them.
            </span>
          )}
        </div>

        {/* Keywords - any */}
        {(() => {
          const entries: RouterKeywordEntry[] = rule.matchKeywordsAny ?? [];
          return (
            <div className="router-rule-field">
              <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 4 }}>
                <label className="form-label" style={{ fontSize: '11px', margin: 0 }}>Keywords - any</label>
                <button type="button" className="settings-reset-button" style={{ fontSize: '11px', padding: '1px 8px' }}
                  disabled={entries.length >= 2}
                  title={entries.length >= 2 ? '2 entries cover all cases - one to match, one to exclude' : 'Add a keywords-any condition'}
                  onClick={() => patchRule(rule.id, { matchKeywordsAny: [...entries, { keywords: [] }] })}>+</button>
              </div>
              {entries.map((entry, i) => (
                <div key={i} style={{ display: 'flex', gap: 6, marginBottom: 4 }}>
                  <select className="form-input form-select router-condition-select"
                    value={entry.not ? 'no' : 'yes'}
                    onChange={(e) => { const u = [...entries]; u[i] = { ...entry, not: e.target.value === 'no' ? true : undefined }; patchRule(rule.id, { matchKeywordsAny: u }); }}>
                    <option value="yes">YES</option>
                    <option value="no">NO</option>
                  </select>
                  <input type="text" className="form-input"
                    style={{ fontSize: '12px', flex: 1, borderColor: entry.not ? '#6b0101' : undefined }}
                    defaultValue={entry.keywords.join(', ')}
                    onBlur={(e) => { const u = [...entries]; u[i] = { ...entry, keywords: e.target.value.split(',').map(s => s.trim()).filter(Boolean) }; patchRule(rule.id, { matchKeywordsAny: u }); }}
                    placeholder="e.g. function, stack trace" />
                  <button type="button" className="settings-reset-button" style={{ fontSize: '11px', padding: '1px 8px' }}
                    onClick={() => patchRule(rule.id, { matchKeywordsAny: entries.filter((_, j) => j !== i) })}>×</button>
                </div>
              ))}
              {entries.length === 0 && <div className="settings-description" style={{ fontSize: '0.68rem' }}>Click + to add.</div>}
            </div>
          );
        })()}

        {/* Keywords - all */}
        {(() => {
          const entries: RouterKeywordEntry[] = rule.matchKeywordsAll ?? [];
          return (
            <div className="router-rule-field">
              <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 4 }}>
                <label className="form-label" style={{ fontSize: '11px', margin: 0 }}>Keywords - all</label>
                <button type="button" className="settings-reset-button" style={{ fontSize: '11px', padding: '1px 8px' }}
                  disabled={entries.length >= 2}
                  title={entries.length >= 2 ? '2 entries cover all cases - one to match, one to exclude' : 'Add a keywords-all condition'}
                  onClick={() => patchRule(rule.id, { matchKeywordsAll: [...entries, { keywords: [] }] })}>+</button>
              </div>
              {entries.map((entry, i) => (
                <div key={i} style={{ display: 'flex', gap: 6, marginBottom: 4 }}>
                  <select className="form-input form-select router-condition-select"
                    value={entry.not ? 'no' : 'yes'}
                    onChange={(e) => { const u = [...entries]; u[i] = { ...entry, not: e.target.value === 'no' ? true : undefined }; patchRule(rule.id, { matchKeywordsAll: u }); }}>
                    <option value="yes">YES</option>
                    <option value="no">NO</option>
                  </select>
                  <input type="text" className="form-input"
                    style={{ fontSize: '12px', flex: 1, borderColor: entry.not ? '#6b0101' : undefined }}
                    defaultValue={entry.keywords.join(', ')}
                    onBlur={(e) => { const u = [...entries]; u[i] = { ...entry, keywords: e.target.value.split(',').map(s => s.trim()).filter(Boolean) }; patchRule(rule.id, { matchKeywordsAll: u }); }}
                    placeholder="e.g. urgent, escalate" />
                  <button type="button" className="settings-reset-button" style={{ fontSize: '11px', padding: '1px 8px' }}
                    onClick={() => patchRule(rule.id, { matchKeywordsAll: entries.filter((_, j) => j !== i) })}>×</button>
                </div>
              ))}
              {entries.length === 0 && <div className="settings-description" style={{ fontSize: '0.68rem' }}>Click + to add.</div>}
            </div>
          );
        })()}

        {/* Regex */}
        {(() => {
          const entries: RouterRegexEntry[] = rule.matchRegex ?? [];
          return (
            <div className="router-rule-field">
              <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 4 }}>
                <label className="form-label" style={{ fontSize: '11px', margin: 0 }}>Regex</label>
                <button type="button" className="settings-reset-button" style={{ fontSize: '11px', padding: '1px 8px' }}
                  disabled={entries.length >= 2}
                  title={entries.length >= 2 ? '2 entries cover all cases - one to match, one to exclude' : 'Add a regex condition'}
                  onClick={() => patchRule(rule.id, { matchRegex: [...entries, { pattern: '' }] })}>+</button>
              </div>
              {entries.map((entry, i) => (
                <div key={i} style={{ display: 'flex', gap: 6, marginBottom: 4 }}>
                  <select className="form-input form-select router-condition-select"
                    value={entry.not ? 'no' : 'yes'}
                    onChange={(e) => { const u = [...entries]; u[i] = { ...entry, not: e.target.value === 'no' ? true : undefined }; patchRule(rule.id, { matchRegex: u }); }}>
                    <option value="yes">YES</option>
                    <option value="no">NO</option>
                  </select>
                  <input type="text" className="form-input"
                    style={{ fontSize: '12px', fontFamily: 'monospace', flex: 1, borderColor: entry.not ? '#6b0101' : undefined }}
                    value={entry.pattern}
                    onChange={(e) => { const u = [...entries]; u[i] = { ...entry, pattern: e.target.value }; patchRule(rule.id, { matchRegex: u }); }}
                    placeholder="e.g. ```[a-z]*" />
                  <button type="button" className="settings-reset-button" style={{ fontSize: '11px', padding: '1px 8px' }}
                    onClick={() => patchRule(rule.id, { matchRegex: entries.filter((_, j) => j !== i) })}>×</button>
                </div>
              ))}
              {entries.length === 0 && <div className="settings-description" style={{ fontSize: '0.68rem' }}>Click + to add.</div>}
            </div>
          );
        })()}

        {/* Min / Max chars */}
        <div className="router-rule-field" style={{ display: 'flex', gap: 12 }}>
          <div style={{ flex: 1 }}>
            <label className="form-label" style={{ fontSize: '11px' }}>Min chars</label>
            <div style={{ display: 'flex', gap: 6 }}>
              <select className="form-input form-select router-condition-select"
                value={rule.matchMinCharsNot === undefined ? '' : rule.matchMinCharsNot ? 'no' : 'yes'}
                onChange={(e) => patchRule(rule.id, { matchMinCharsNot: e.target.value === '' ? undefined : e.target.value === 'no' })}>
                <option value=""></option><option value="yes">YES</option><option value="no">NO</option>
              </select>
              <input type="number" className="form-input"
                style={{ fontSize: '12px', flex: 1, borderColor: rule.matchMinCharsNot === true ? '#6b0101' : undefined }}
                min={0} value={rule.matchMinChars ?? ''}
                onChange={(e) => { const n = e.target.value ? parseInt(e.target.value) : undefined; patchRule(rule.id, { matchMinChars: n, matchMinCharsNot: n !== undefined && rule.matchMinCharsNot === undefined ? false : rule.matchMinCharsNot }); }}
                placeholder="none" />
            </div>
          </div>
          <div style={{ flex: 1 }}>
            <label className="form-label" style={{ fontSize: '11px' }}>Max chars</label>
            <div style={{ display: 'flex', gap: 6 }}>
              <select className="form-input form-select router-condition-select"
                value={rule.matchMaxCharsNot === undefined ? '' : rule.matchMaxCharsNot ? 'no' : 'yes'}
                onChange={(e) => patchRule(rule.id, { matchMaxCharsNot: e.target.value === '' ? undefined : e.target.value === 'no' })}>
                <option value=""></option><option value="yes">YES</option><option value="no">NO</option>
              </select>
              <input type="number" className="form-input"
                style={{ fontSize: '12px', flex: 1, borderColor: rule.matchMaxCharsNot === true ? '#6b0101' : undefined }}
                min={0} value={rule.matchMaxChars ?? ''}
                onChange={(e) => { const n = e.target.value ? parseInt(e.target.value) : undefined; patchRule(rule.id, { matchMaxChars: n, matchMaxCharsNot: n !== undefined && rule.matchMaxCharsNot === undefined ? false : rule.matchMaxCharsNot }); }}
                placeholder="none" />
            </div>
          </div>
        </div>

        {/* Has tools / Has images */}
        <div className="router-rule-field" style={{ display: 'flex', gap: 16, flexWrap: 'wrap' }}>
          {(['matchHasTools', 'matchHasImages'] as const).map((field) => {
            const notField = (field + 'Not') as 'matchHasToolsNot' | 'matchHasImagesNot';
            const label = field === 'matchHasTools' ? 'Request has tools' : 'Request has images';
            return (
              <div key={field} style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                <select className="form-input form-select router-condition-select"
                  value={rule[field] === undefined ? '' : rule[notField] ? 'no' : 'yes'}
                  onChange={(e) => {
                    const v = e.target.value;
                    patchRule(rule.id, { [field]: v === '' ? undefined : true, [notField]: v === 'no' ? true : v === '' ? undefined : false } as Partial<RouterRule>);
                  }}>
                  <option value=""></option><option value="yes">YES</option><option value="no">NO</option>
                </select>
                <span className="form-label" style={{ fontSize: '12px', margin: 0, color: rule[notField] ? '#6b0101' : undefined }}>{label}</span>
              </div>
            );
          })}
        </div>

        {/* Classifier condition */}
        {classifiers.length > 0 && (
          <div className="router-rule-field">
            <label className="form-label" style={{ fontSize: '11px' }}>Classifier condition</label>
            <div style={{ display: 'flex', gap: 8, alignItems: 'flex-end', flexWrap: 'wrap' }}>
              <div style={{ flex: 2, minWidth: 100 }}>
                <select className="form-input form-select" style={{ fontSize: '12px' }}
                  value={rule.matchClassifier?.classifierId ?? ''}
                  onChange={(e) => patchRule(rule.id, {
                    matchClassifier: e.target.value ? { classifierId: e.target.value, minScore: 0.5 } : undefined,
                  })}>
                  <option value="">None</option>
                  {classifiers.map((c) => <option key={c.id} value={c.id}>{c.id}</option>)}
                </select>
              </div>
              {rule.matchClassifier?.classifierId && (() => {
                const activeCLF = classifiers.find((c) => c.id === rule.matchClassifier!.classifierId);
                const clfLabels = activeCLF?.type === 'semantic_similarity'
                  ? Object.keys(activeCLF.referencePhrases ?? {}).filter(Boolean)
                  : activeCLF?.labels?.filter(Boolean) ?? [];
                return (
                  <>
                    {clfLabels.length > 0 && (
                      <div style={{ flex: 1, minWidth: 80 }}>
                        <label className="form-label" style={{ fontSize: '10px' }}>Label <span className="settings-description" style={{ marginLeft: 3 }}>(overrides default)</span></label>
                        <select className="form-input form-select" style={{ fontSize: '12px' }}
                          value={rule.matchClassifier!.label ?? ''}
                          onChange={(e) => patchRule(rule.id, { matchClassifier: { ...rule.matchClassifier!, label: e.target.value || undefined } })}>
                          <option value="">default</option>
                          {clfLabels.map((l) => <option key={l} value={l}>{l}</option>)}
                        </select>
                      </div>
                    )}
                    <div style={{ flex: 1, minWidth: 60 }}>
                      <label className="form-label" style={{ fontSize: '10px' }}>Min score</label>
                      <input type="number" className="form-input" style={{ fontSize: '12px' }}
                        min={0} max={1} step={0.05} value={rule.matchClassifier!.minScore ?? 0.5}
                        onChange={(e) => patchRule(rule.id, { matchClassifier: { ...rule.matchClassifier!, minScore: parseFloat(e.target.value) } })} />
                    </div>
                    <div style={{ flex: 1, minWidth: 60 }}>
                      <label className="form-label" style={{ fontSize: '10px' }}>Max score</label>
                      <input type="number" className="form-input" style={{ fontSize: '12px' }}
                        min={0} max={1} step={0.05} value={rule.matchClassifier!.maxScore ?? ''}
                        onChange={(e) => patchRule(rule.id, { matchClassifier: { ...rule.matchClassifier!, maxScore: e.target.value ? parseFloat(e.target.value) : undefined } })}
                        placeholder="none" />
                    </div>
                    <div style={{ alignSelf: 'flex-end', paddingBottom: 4 }}>
                      <label className="router-not-toggle" style={{ fontSize: '12px' }}>
                        <input type="checkbox" checked={rule.matchClassifier!.not === true}
                          onChange={(e) => patchRule(rule.id, { matchClassifier: { ...rule.matchClassifier!, not: e.target.checked || undefined } })} />
                        <span>NOT</span>
                      </label>
                    </div>
                  </>
                );
              })()}
            </div>
          </div>
        )}
      </div>
    );
  };

  const expandIcon = (
    <svg width="11" height="11" viewBox="0 0 12 12" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round">
      <path d="M7 1h4v4M5 7L11 1M1 5v6h6"/>
    </svg>
  );


  return (
    <>
      {/* ── Header ─────────────────────────────────────────────────────────── */}
      <div className="settings-header">
        <h3>{mode === 'edit' ? 'Edit Hybrid Router' : 'New Hybrid Router'}</h3>
        <button type="button" className="settings-close-button" onClick={onClose} title="Close">
          <svg width="14" height="14" viewBox="0 0 14 14">
            <path d="M 1,1 L 13,13 M 13,1 L 1,13" stroke="currentColor" strokeWidth="2" strokeLinecap="round" />
          </svg>
        </button>
      </div>

      {/* ── Scrollable content ─────────────────────────────────────────────── */}
      <div className="settings-content custom-collection-content">

        {/* Name */}
        <div className="form-section">
          <label className="form-label">Router Name *</label>
          <input type="text" className="form-input"
            value={draft.name}
            onChange={(e) => patch({ name: e.target.value.replace(/^user\./, '') })}
            placeholder="MyHybridRouter" />
        </div>

        {/* Candidates */}
        <div className="form-section">
          <label className="form-label">
            Candidate Models *
            <span className="settings-description" style={{ marginLeft: 6 }}>- the LLMs that can answer requests</span>
          </label>
          {candidateOptions.length === 0 ? (
            <div className="collection-role-empty">No compatible models found. Pull or register LLM models first.</div>
          ) : (
            <div className="router-candidate-list">
              {candidateOptions.map(({ id, info }) => (
                <label key={id} className="router-candidate-row">
                  <input type="checkbox" checked={draft.candidates.includes(id)} onChange={() => toggleCandidate(id)} />
                  <span className="router-candidate-name">{info.model_name ?? getModelDisplayName(id)}</span>
                  {info.downloaded === true
                    ? <span className="router-candidate-badge downloaded">local</span>
                    : <span className="router-candidate-badge">will download</span>}
                </label>
              ))}
            </div>
          )}
        </div>

        {/* Default model */}
        <div className="form-section">
          <label className="form-label">
            Default Model *
            <span className="settings-description" style={{ marginLeft: 6 }}>- fallback when no rule matches</span>
          </label>
          <select className="form-input form-select" value={draft.defaultModel}
            onChange={(e) => patch({ defaultModel: e.target.value })}
            disabled={draft.candidates.length === 0}>
            <option value="">{draft.candidates.length === 0 ? 'Select candidates first' : 'Select default model…'}</option>
            {draft.candidates.map((id) => <option key={id} value={id}>{displayName(id)}</option>)}
          </select>
        </div>

        {/* Routing mode */}
        <div className="form-section">
          <label className="form-label">Routing Mode</label>
          <div className="router-mode-options">
            <label className="router-mode-option">
              <input type="radio" name="routingMode" value="llm" checked={draft.routingMode === 'llm'} onChange={() => patch({ routingMode: 'llm' })} />
              <span><strong>NL Router</strong><span className="settings-description" style={{ display: 'block' }}>A small LLM reads your prompt and picks the best candidate.</span></span>
            </label>
            <label className="router-mode-option">
              <input type="radio" name="routingMode" value="rules" checked={draft.routingMode === 'rules'} onChange={() => patch({ routingMode: 'rules' })} />
              <span><strong>Rules</strong><span className="settings-description" style={{ display: 'block' }}>Keyword, length, and classifier conditions.</span></span>
            </label>
          </div>
        </div>

        {/* NL Router fields */}
        {draft.routingMode === 'llm' && (
          <>
            <div className="form-section">
              <label className="form-label">
                Router LLM *
                <span className="settings-description" style={{ marginLeft: 6 }}>- small model that reads your prompt</span>
              </label>
              <select className="form-input form-select" value={draft.routerModel ?? ''} onChange={(e) => patch({ routerModel: e.target.value })}>
                <option value="">Select a router LLM…</option>
                {candidateOptions.map(({ id }) => (
                  <option key={id} value={id}>{displayNameWithStatus(id)}{draft.candidates.includes(id) ? ' (also a candidate)' : ''}</option>
                ))}
              </select>
            </div>
            <div className="form-section">
              <label className="form-label">Routing Prompt *</label>
              <textarea className="form-input" rows={5} value={draft.routerPrompt ?? ''}
                onChange={(e) => patch({ routerPrompt: e.target.value })}
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

        {/* Rules mode - chip grids */}
        {draft.routingMode === 'rules' && (
          <>
            {/* Classifiers chip grid */}
            <div className="form-section">
              <div className="router-classifier-section-header">
                <label className="form-label" style={{ margin: 0 }}>
                  Classifiers
                  <span className="settings-description" style={{ marginLeft: 6 }}>- optional, for semantic or model-based matching</span>
                </label>
                <button type="button" className="settings-reset-button" style={{ fontSize: '11px', padding: '2px 10px' }} onClick={addClassifier}>
                  + Add Classifier
                </button>
              </div>
              <div className="router-chip-grid">
                {classifiers.length === 0 && (
                  <div className="collection-role-empty" style={{ width: '100%', textAlign: 'center' }}>
                    No classifiers. Rules can still use keyword and length conditions without any.
                  </div>
                )}
                {classifiers.map((clf) => {
                  const lines = classifierChipLines(clf);
                  return (
                    <div key={clf.id} className="router-chip">
                      <button type="button" className="router-chip-expand" title="Remove classifier"
                        style={{ right: 24 }}
                        onClick={() => removeClassifier(clf.id)}>
                        <svg width="10" height="10" viewBox="0 0 12 12" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round">
                          <path d="M1 1 L11 11 M11 1 L1 11"/>
                        </svg>
                      </button>
                      <button type="button" className="router-chip-expand" title="Edit classifier"
                        onClick={() => setDetailPanel({ kind: 'classifier', id: clf.id })}>
                        {expandIcon}
                      </button>
                      <div className="router-chip-id" style={{ paddingRight: 38 }}>{clf.id}</div>
                      {lines.map((line, i) => <div key={i} className="router-chip-line">{line}</div>)}
                    </div>
                  );
                })}
              </div>
            </div>

            {/* Rules chip grid */}
            <div className="form-section">
              <div className="router-classifier-section-header">
                <label className="form-label" style={{ margin: 0 }}>Rules *</label>
                <button type="button" className="settings-reset-button" style={{ fontSize: '11px', padding: '2px 10px' }}
                  onClick={addRule} title="Add a rule">
                  + Add Rule
                </button>
              </div>
              <div className="router-chip-grid">
                {rules.length === 0 && (
                  <div className="collection-role-empty" style={{ width: '100%', textAlign: 'center' }}>
                    No rules yet. Add a rule to route specific queries to a candidate.
                  </div>
                )}
                {rules.map((rule) => {
                  const lines = ruleChipLines(rule);
                  return (
                    <div key={rule.id} className="router-chip">
                      <button type="button" className="router-chip-expand" title="Remove rule"
                        style={{ right: 24 }}
                        onClick={() => removeRule(rule.id)}>
                        <svg width="10" height="10" viewBox="0 0 12 12" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round">
                          <path d="M1 1 L11 11 M11 1 L1 11"/>
                        </svg>
                      </button>
                      <button type="button" className="router-chip-expand" title="Edit rule"
                        onClick={() => setDetailPanel({ kind: 'rule', id: rule.id })}>
                        {expandIcon}
                      </button>
                      <div className="router-chip-id" style={{ paddingRight: 38 }}>{rule.id}</div>
                      {lines.map((line, i) => <div key={i} className="router-chip-line">{line}</div>)}
                    </div>
                  );
                })}
              </div>
            </div>
          </>
        )}

      </div>
      {/* ── End scrollable content ─────────────────────────────────────────── */}

      {/* ── JSON preview ───────────────────────────────────────────────────── */}
      {previewJson !== null && (
        <div className="router-json-preview">
          <div className="router-json-preview-header">
            <span className="router-json-preview-title">Generated JSON</span>
            <button type="button" className="router-json-preview-btn" title="Copy to clipboard"
              onClick={() => navigator.clipboard.writeText(previewJson)}>
              <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                <rect x="9" y="9" width="13" height="13" rx="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/>
              </svg>
            </button>
            <button type="button" className="router-json-preview-btn" title="Download as JSON"
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

      {/* ── Footer ─────────────────────────────────────────────────────────── */}
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

      {/* ── Detail panel portal ─────────────────────────────────────────────── */}
      {detailPanel && createPortal(
        <div className="router-detail-overlay"
          onMouseDown={(e) => { if (e.target === e.currentTarget) closeDetail(); }}>
          <div className="router-detail-panel" onMouseDown={(e) => e.stopPropagation()}>
            <div className="settings-header">
              <h3>
                {detailPanel.kind === 'classifier'
                  ? `Classifier: ${detailPanel.id}`
                  : `Rule: ${detailPanel.id}`}
              </h3>
              <button type="button" className="settings-close-button" onClick={closeDetail} title="Done">
                <svg width="14" height="14" viewBox="0 0 14 14">
                  <path d="M 1,1 L 13,13 M 13,1 L 1,13" stroke="currentColor" strokeWidth="2" strokeLinecap="round" />
                </svg>
              </button>
            </div>
            <div className="settings-content" style={{ padding: 0 }}>
              {detailPanel.kind === 'classifier'
                ? (() => { const clf = classifiers.find(c => c.id === detailPanel.id); return clf ? renderClassifierDetail(clf) : null; })()
                : (() => { const rule = rules.find(r => r.id === detailPanel.id); return rule ? renderRuleDetail(rule) : null; })()}
            </div>
            <div className="settings-footer custom-collection-footer">
              <button type="button" className="settings-reset-button" style={{ marginRight: 'auto' }}
                onClick={() => {
                  if (detailPanel.kind === 'classifier') removeClassifier(detailPanel.id);
                  else removeRule(detailPanel.id);
                  closeDetail();
                }}>
                Remove
              </button>
              <button type="button" className="settings-save-button" onClick={closeDetail}>Done</button>
            </div>
          </div>
        </div>,
        document.body
      )}
    </>
  );
};

export default RouterCollectionPanel;
