import React, { useEffect, useMemo, useRef, useState } from 'react';
import type { ModelInfo } from '../api';
import { capabilityFromModelInfo } from '../modelCapabilities';
import { Icon } from './Icon';
import RouterNodeEditor from './RouterNodeEditor';
import {
  buildRouterPullRequest,
  classifierLabels,
  createEmptyRouterDraft,
  createRouterClassifier,
  createRouterRule,
  parseRouterPayload,
  renameClassifierReference,
  renameClassifierLabelReference,
  routerNodeReferencesClassifier,
  routerDraftFromModelInfo,
  routerDisplayName,
  validateRouterDraft,
  type RouterClassifier,
  type RouterDraft,
  type RouterPullRequest,
} from '../features/router/routerTypes';
import {
  loadRouterRecords,
  routerRecordToDraft,
  routerRecordToModelInfo,
  upsertRouterRecord,
} from '../features/router/routerStore';

function modelName(model: ModelInfo | null | undefined): string {
  if (!model) return '';
  return String((model as any).model_name ?? model.name ?? model.id ?? '').trim();
}

function modelLabel(model: ModelInfo): string {
  return String(model.display_name || modelName(model));
}

function downloadJson(name: string, payload: unknown): void {
  const safeName = name.replace(/[^A-Za-z0-9._-]+/g, '-').replace(/^-+|-+$/g, '') || 'router';
  const url = URL.createObjectURL(new Blob([JSON.stringify(payload, null, 2)], { type: 'application/json' }));
  const link = document.createElement('a');
  link.href = url;
  link.download = `${safeName}.router.json`;
  document.body.appendChild(link);
  link.click();
  link.remove();
  window.setTimeout(() => URL.revokeObjectURL(url), 0);
}

async function copyText(text: string): Promise<void> {
  if (navigator.clipboard?.writeText) {
    await navigator.clipboard.writeText(text);
    return;
  }
  const textarea = document.createElement('textarea');
  textarea.value = text;
  textarea.style.position = 'fixed';
  textarea.style.opacity = '0';
  document.body.appendChild(textarea);
  textarea.select();
  document.execCommand('copy');
  textarea.remove();
}

function moveItem<T>(items: T[], index: number, delta: number): T[] {
  const target = index + delta;
  if (target < 0 || target >= items.length) return items;
  const next = [...items];
  [next[index], next[target]] = [next[target], next[index]];
  return next;
}

function nextSafeId(prefix: string, existing: string[]): string {
  const used = new Set(existing);
  let index = 1;
  while (used.has(`${prefix}-${index}`)) index += 1;
  return `${prefix}-${index}`;
}

function nextConceptName(existing: Record<string, string[]>): string {
  return nextSafeId('concept', Object.keys(existing));
}


interface RouterEditorPanelProps {
  models: ModelInfo[];
  scope: string;
  initialModel?: ModelInfo | null;
  onRegister: (request: RouterPullRequest) => Promise<void>;
  onSaved?: (model: ModelInfo) => void;
  onDeleted?: (modelName: string) => Promise<void> | void;
  onClose: () => void;
}

export const RouterEditorPanel: React.FC<RouterEditorPanelProps> = ({
  models,
  scope,
  initialModel,
  onRegister,
  onSaved,
  onDeleted,
  onClose,
}) => {
  const [draft, setDraft] = useState<RouterDraft>(() => createEmptyRouterDraft());
  const [savedRecords, setSavedRecords] = useState(() => loadRouterRecords(scope));
  const [candidateSearch, setCandidateSearch] = useState('');
  const [tab, setTab] = useState<'builder' | 'json'>('builder');
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [notice, setNotice] = useState<string | null>(null);
  const importRef = useRef<HTMLInputElement>(null);

  const refreshSaved = () => setSavedRecords(loadRouterRecords(scope));

  useEffect(() => {
    setSavedRecords(loadRouterRecords(scope));
  }, [scope]);

  useEffect(() => {
    if (!initialModel || String((initialModel as any).recipe || '').toLowerCase() !== 'collection.router') return;
    try {
      setDraft(routerDraftFromModelInfo(initialModel));
      setError(null);
      setNotice(null);
    } catch (initialError) {
      setError(initialError instanceof Error ? initialError.message : 'Could not open this router.');
    }
  }, [initialModel]);

  const candidateModels = useMemo(() => models
    .filter(model => String((model as any).recipe || '').toLowerCase() !== 'collection.router')
    .filter(model => {
      const capability = capabilityFromModelInfo(model);
      return capability === 'chat' || capability === 'omni' || capability === 'unknown';
    })
    .sort((a, b) => modelLabel(a).localeCompare(modelLabel(b))), [models]);

  const filteredCandidateModels = useMemo(() => {
    const query = candidateSearch.trim().toLowerCase();
    if (!query) return candidateModels;
    return candidateModels.filter(model => `${modelLabel(model)} ${modelName(model)} ${(model.labels || []).join(' ')}`.toLowerCase().includes(query));
  }, [candidateModels, candidateSearch]);

  const embeddingModels = useMemo(() => {
    const explicit = models.filter(model => {
      const labels = (model.labels || []).map(label => label.toLowerCase());
      return capabilityFromModelInfo(model) === 'embedding' || labels.includes('embedding') || labels.includes('embeddings');
    });
    return (explicit.length ? explicit : models)
      .filter(model => String((model as any).recipe || '').toLowerCase() !== 'collection.router')
      .sort((a, b) => modelLabel(a).localeCompare(modelLabel(b)));
  }, [models]);

  const validationErrors = useMemo(() => validateRouterDraft(draft), [draft]);
  const request = useMemo(() => {
    try { return buildRouterPullRequest(draft); } catch { return null; }
  }, [draft]);
  const jsonPreview = useMemo(() => request ? JSON.stringify(request, null, 2) : '', [request]);

  const setPatch = (patch: Partial<RouterDraft>) => {
    setDraft(current => ({ ...current, ...patch }));
    setError(null);
    setNotice(null);
  };

  const resetDraft = () => {
    setDraft(createEmptyRouterDraft());
    setError(null);
    setNotice(null);
    setTab('builder');
  };

  const loadSaved = (modelNameValue: string) => {
    if (!modelNameValue) {
      resetDraft();
      return;
    }
    const record = savedRecords.find(item => item.model_name === modelNameValue);
    if (!record) return;
    try {
      setDraft(routerRecordToDraft(record));
      setError(null);
      setNotice(`Loaded ${record.display_name}.`);
    } catch (loadError) {
      setError(loadError instanceof Error ? loadError.message : 'Could not load saved router.');
    }
  };

  const toggleCandidate = (name: string) => {
    setDraft(current => {
      const exists = current.candidates.includes(name);
      const candidates = exists ? current.candidates.filter(item => item !== name) : [...current.candidates, name];
      const defaultModel = candidates.includes(current.defaultModel) ? current.defaultModel : (candidates[0] || '');
      const rules = current.rules.map(rule => ({
        ...rule,
        routeTo: candidates.includes(rule.routeTo) ? rule.routeTo : defaultModel,
      }));
      return { ...current, candidates, defaultModel, rules };
    });
    setError(null);
    setNotice(null);
  };

  const addClassifier = (type: RouterClassifier['type']) => {
    const classifier = createRouterClassifier(draft.classifiers.length, type);
    classifier.id = nextSafeId('classifier', draft.classifiers.map(item => item.id));
    setPatch({ classifiers: [...draft.classifiers, classifier] });
  };

  const updateClassifier = (index: number, patch: Partial<RouterClassifier>) => {
    setDraft(current => {
      const previous = current.classifiers[index];
      const nextClassifier = { ...previous, ...patch };
      const classifiers = current.classifiers.map((item, itemIndex) => itemIndex === index ? nextClassifier : item);
      const rules = previous.id !== nextClassifier.id
        ? current.rules.map(rule => ({ ...rule, condition: renameClassifierReference(rule.condition, previous.id, nextClassifier.id) }))
        : current.rules;
      return { ...current, classifiers, rules };
    });
    setError(null);
    setNotice(null);
  };

  const removeClassifier = (index: number) => {
    const classifier = draft.classifiers[index];
    if (classifier && draft.rules.some(rule => routerNodeReferencesClassifier(rule.condition, classifier.id))) {
      setError(`Classifier "${classifier.id}" is still used by a rule. Change those conditions before removing it.`);
      return;
    }
    setPatch({ classifiers: draft.classifiers.filter((_, itemIndex) => itemIndex !== index) });
  };

  const renameSemanticConcept = (classifierIndex: number, previousName: string, nextName: string): void => {
    setDraft(current => {
      const classifier = current.classifiers[classifierIndex];
      if (!classifier || classifier.type !== 'semantic_similarity') return current;
      if (nextName !== previousName && Object.prototype.hasOwnProperty.call(classifier.referencePhrases, nextName)) return current;
      const entries = Object.entries(classifier.referencePhrases).map(([name, phrases]) =>
        name === previousName ? [nextName, phrases] as const : [name, phrases] as const,
      );
      const nextClassifier = {
        ...classifier,
        referencePhrases: Object.fromEntries(entries),
        defaultLabel: classifier.defaultLabel === previousName ? nextName : classifier.defaultLabel,
      };
      return {
        ...current,
        classifiers: current.classifiers.map((item, index) => index === classifierIndex ? nextClassifier : item),
        rules: current.rules.map(rule => ({
          ...rule,
          condition: renameClassifierLabelReference(rule.condition, classifier.id, previousName, nextName),
        })),
      };
    });
    setError(null);
    setNotice(null);
  };

  const addRule = () => {
    const rule = createRouterRule(draft.rules.length, draft.defaultModel);
    rule.id = nextSafeId('rule', draft.rules.map(item => item.id));
    setPatch({ rules: [...draft.rules, rule] });
  };

  const importFile = async (file: File | undefined) => {
    if (!file) return;
    try {
      const parsed = JSON.parse(await file.text());
      setDraft(parseRouterPayload(parsed));
      setError(null);
      setNotice(`Imported ${file.name}. Save & register to persist it.`);
      setTab('builder');
    } catch (importError) {
      setError(importError instanceof Error ? importError.message : 'Could not import router JSON.');
    } finally {
      if (importRef.current) importRef.current.value = '';
    }
  };

  const save = async () => {
    setError(null);
    setNotice(null);
    let nextRequest: RouterPullRequest;
    try {
      nextRequest = buildRouterPullRequest(draft);
    } catch (buildError) {
      setError(buildError instanceof Error ? buildError.message : 'Router validation failed.');
      return;
    }
    setSaving(true);
    try {
      await onRegister(nextRequest);
      const record = upsertRouterRecord(scope, { ...draft, modelName: nextRequest.model_name });
      refreshSaved();
      setDraft(routerRecordToDraft(record));
      setNotice(`Registered ${record.model_name}.`);
      onSaved?.(routerRecordToModelInfo(record));
    } catch (saveError) {
      setError(saveError instanceof Error ? saveError.message : 'Could not register router.');
    } finally {
      setSaving(false);
    }
  };

  const deleteCurrent = async () => {
    const modelNameValue = draft.modelName;
    if (!modelNameValue) return;
    if (!window.confirm(`Delete ${modelNameValue}?`)) return;
    try {
      await onDeleted?.(modelNameValue);
      refreshSaved();
      resetDraft();
      setNotice(`Deleted ${modelNameValue}.`);
    } catch (deleteError) {
      setError(deleteError instanceof Error ? deleteError.message : 'Could not delete router.');
    }
  };

  return (
    <section className="router-editor" aria-label="Router editor">
      <header className="router-editor__header">
        <div className="router-editor__heading">
          <span className="router-editor__icon"><Icon name="router" size={20} /></span>
          <div>
            <div className="router-editor__eyebrow">Models · collection.router</div>
            <h2>Router</h2>
          </div>
        </div>
        <button type="button" className="router-editor__close" onClick={onClose} aria-label="Close router editor" title="Close"><Icon name="x" size={18} /></button>
      </header>

      <div className="router-editor__toolbar">
        <button type="button" className="btn btn--ghost btn--tiny" onClick={resetDraft}><Icon name="plus" size={13} /> New</button>
        <label className="router-editor__saved-select">
          <span className="sr-only">Saved routers</span>
          <select value={draft.modelName || ''} onChange={event => loadSaved(event.target.value)}>
            <option value="">Unsaved router</option>
            {savedRecords.map(record => <option key={record.model_name} value={record.model_name}>{record.display_name}</option>)}
          </select>
        </label>
        <span className="router-editor__toolbar-spacer" />
        <button type="button" className="btn btn--ghost btn--tiny" onClick={() => importRef.current?.click()}><Icon name="download" size={13} /> Import</button>
        <button type="button" className="btn btn--ghost btn--tiny" disabled={!request} onClick={() => request && downloadJson(routerDisplayName(request.model_name), request)}><Icon name="file" size={13} /> Export</button>
        <input ref={importRef} className="hidden-file-input" type="file" accept="application/json,.json" onChange={event => { void importFile(event.target.files?.[0]); }} />
      </div>

      <div className="router-editor__tabs" role="tablist" aria-label="Router editor view">
        <button type="button" className={tab === 'builder' ? 'is-active' : ''} role="tab" aria-selected={tab === 'builder'} onClick={() => setTab('builder')}>Builder</button>
        <button type="button" className={tab === 'json' ? 'is-active' : ''} role="tab" aria-selected={tab === 'json'} onClick={() => setTab('json')}>JSON preview</button>
      </div>

      <div className="router-editor__body">
        <div className="router-editor__backend-note">
          <Icon name="info" size={15} />
          <span>This editor only exposes server-supported v1 paths. NL Router and LLM classifiers remain hidden until backend issue #2405 lands.</span>
        </div>

        {tab === 'json' ? (
          <section className="router-editor__json-panel">
            <div className="router-editor__section-head">
              <div><h3>Registration payload</h3><p>Exact body sent to <code>/api/v1/pull</code>.</p></div>
              <button type="button" className="btn btn--ghost btn--tiny" disabled={!jsonPreview} onClick={() => { void copyText(jsonPreview).then(() => setNotice('JSON copied.')); }}><Icon name="copy" size={13} /> Copy</button>
            </div>
            {jsonPreview ? <pre>{jsonPreview}</pre> : <div className="router-editor__empty">Fix validation errors to generate the payload.</div>}
          </section>
        ) : (
          <>
            <section className="router-editor__section">
              <div className="router-editor__section-head">
                <div><h3>Identity</h3><p>Saved as a user-scoped virtual model.</p></div>
              </div>
              <div className="router-editor__form-grid">
                <label><span>Router name</span><input value={draft.name} placeholder="Fast-or-smart" onChange={event => setPatch({ name: event.target.value })} /></label>
                <label><span>Model ID</span><input value={draft.modelName || (draft.name ? `user.${draft.name.replace(/[^A-Za-z0-9._-]+/g, '-')}` : '')} readOnly /></label>
              </div>
            </section>

            <section className="router-editor__section">
              <div className="router-editor__section-head">
                <div><h3>Candidate models</h3><p>Rules may route only to selected candidates.</p></div>
                <span className="router-editor__count">{draft.candidates.length} selected</span>
              </div>
              <div className="router-editor__candidate-search"><Icon name="search" size={14} /><input value={candidateSearch} placeholder="Search registered models" onChange={event => setCandidateSearch(event.target.value)} /></div>
              <div className="router-editor__candidate-list">
                {filteredCandidateModels.map(model => {
                  const name = modelName(model);
                  const checked = draft.candidates.includes(name);
                  return (
                    <label key={name} className={`router-editor__candidate ${checked ? 'is-selected' : ''}`}>
                      <input type="checkbox" checked={checked} onChange={() => toggleCandidate(name)} />
                      <span className="router-editor__candidate-main"><strong>{modelLabel(model)}</strong><small>{name}</small></span>
                      <span className="router-editor__candidate-recipe">{String((model as any).recipe || 'model')}</span>
                    </label>
                  );
                })}
                {filteredCandidateModels.length === 0 && <div className="router-editor__empty">No compatible models match this search.</div>}
              </div>
              <label className="router-editor__default-model">
                <span>Default model <small>Used when no rule matches or evaluation fails.</small></span>
                <select value={draft.defaultModel} onChange={event => setPatch({ defaultModel: event.target.value })}>
                  <option value="">Select default</option>
                  {draft.candidates.map(candidate => <option key={candidate} value={candidate}>{candidate}</option>)}
                </select>
              </label>
            </section>

            <section className="router-editor__section">
              <div className="router-editor__section-head">
                <div><h3>Classifiers</h3><p>Optional model-backed signals evaluated once per request.</p></div>
                <div className="router-editor__section-actions">
                  <button type="button" className="btn btn--ghost btn--tiny" onClick={() => addClassifier('classifier')}><Icon name="plus" size={13} /> Classifier</button>
                  <button type="button" className="btn btn--ghost btn--tiny" onClick={() => addClassifier('semantic_similarity')}><Icon name="plus" size={13} /> Semantic</button>
                </div>
              </div>
              {draft.classifiers.length === 0 ? <div className="router-editor__empty">No classifiers. Deterministic rules need none.</div> : (
                <div className="router-editor__classifier-list">
                  {draft.classifiers.map((classifier, index) => {
                    const labels = classifierLabels(classifier);
                    return (
                      <article className="router-editor__classifier" key={`${classifier.id}-${index}`}>
                        <div className="router-editor__card-head">
                          <strong>{classifier.type === 'semantic_similarity' ? 'Semantic similarity' : 'Text classifier'}</strong>
                          <button type="button" className="router-editor__icon-btn" onClick={() => removeClassifier(index)} title="Remove classifier"><Icon name="trash" size={14} /></button>
                        </div>
                        <div className="router-editor__form-grid router-editor__form-grid--classifier">
                          <label><span>ID</span><input value={classifier.id} onChange={event => updateClassifier(index, { id: event.target.value })} /></label>
                          <label><span>Type</span><select value={classifier.type} onChange={event => updateClassifier(index, { ...createRouterClassifier(index, event.target.value as RouterClassifier['type']), id: classifier.id })}><option value="classifier">classifier</option><option value="semantic_similarity">semantic_similarity</option></select></label>
                          <label className="router-editor__wide"><span>Model</span><select value={classifier.model} onChange={event => updateClassifier(index, { model: event.target.value })}><option value="">Select model</option>{(classifier.type === 'semantic_similarity' ? embeddingModels : models).filter(model => String((model as any).recipe || '').toLowerCase() !== 'collection.router').map(model => <option key={modelName(model)} value={modelName(model)}>{modelLabel(model)} · {modelName(model)}</option>)}</select></label>
                          {classifier.type === 'classifier' ? (
                            <label className="router-editor__wide"><span>Output labels <small>Comma-separated</small></span><input value={classifier.labels.join(', ')} onChange={event => updateClassifier(index, { labels: event.target.value.split(',').map(label => label.trimStart()), defaultLabel: undefined })} /></label>
                          ) : (
                            <div className="router-editor__wide router-editor__concepts">
                              <div className="router-editor__mini-head"><span>Concepts and reference phrases</span><button type="button" className="btn btn--ghost btn--tiny" onClick={() => updateClassifier(index, { referencePhrases: { ...classifier.referencePhrases, [nextConceptName(classifier.referencePhrases)]: ['example phrase'] } })}><Icon name="plus" size={12} /> Concept</button></div>
                              {Object.entries(classifier.referencePhrases).map(([concept, phrases], conceptIndex) => (
                                <div className="router-editor__concept" key={`${concept}-${conceptIndex}`}>
                                  <input value={concept} aria-label="Concept name" onChange={event => renameSemanticConcept(index, concept, event.target.value)} />
                                  <input value={phrases.join(', ')} aria-label="Reference phrases" placeholder="Comma-separated phrases" onChange={event => updateClassifier(index, { referencePhrases: { ...classifier.referencePhrases, [concept]: event.target.value.split(',').map(phrase => phrase.trimStart()) } })} />
                                  <button type="button" className="router-editor__icon-btn" title="Remove concept" onClick={() => { const next = { ...classifier.referencePhrases }; delete next[concept]; updateClassifier(index, { referencePhrases: next, defaultLabel: undefined }); }}><Icon name="trash" size={13} /></button>
                                </div>
                              ))}
                            </div>
                          )}
                          <label><span>Default label</span><select value={classifier.defaultLabel || ''} onChange={event => updateClassifier(index, { defaultLabel: event.target.value || undefined })}><option value="">None</option>{labels.map(label => <option key={label} value={label}>{label}</option>)}</select></label>
                          <label><span>On error</span><select value={classifier.onError} onChange={event => updateClassifier(index, { onError: event.target.value as RouterClassifier['onError'] })}><option value="match_false">Do not match</option><option value="match_true">Match rule</option></select></label>
                        </div>
                      </article>
                    );
                  })}
                </div>
              )}
            </section>

            <section className="router-editor__section">
              <div className="router-editor__section-head">
                <div><h3>Ordered rules</h3><p>First matching rule wins. The default model handles the remainder.</p></div>
                <button type="button" className="btn btn--ghost btn--tiny" onClick={addRule}><Icon name="plus" size={13} /> Rule</button>
              </div>
              <div className="router-editor__rule-list">
                {draft.rules.map((rule, index) => (
                  <article className="router-editor__rule" key={`${rule.id}-${index}`}>
                    <div className="router-editor__card-head">
                      <span className="router-editor__rule-order">{index + 1}</span>
                      <strong>{rule.id || `Rule ${index + 1}`}</strong>
                      <div className="router-editor__rule-actions">
                        <button type="button" disabled={index === 0} onClick={() => setPatch({ rules: moveItem(draft.rules, index, -1) })} title="Move rule up"><Icon name="chevron-up" size={14} /></button>
                        <button type="button" disabled={index === draft.rules.length - 1} onClick={() => setPatch({ rules: moveItem(draft.rules, index, 1) })} title="Move rule down"><Icon name="chevron-down" size={14} /></button>
                        <button type="button" onClick={() => setPatch({ rules: draft.rules.filter((_, itemIndex) => itemIndex !== index) })} title="Remove rule"><Icon name="trash" size={14} /></button>
                      </div>
                    </div>
                    <div className="router-editor__rule-meta">
                      <label><span>Rule ID</span><input value={rule.id} onChange={event => setPatch({ rules: draft.rules.map((item, itemIndex) => itemIndex === index ? { ...item, id: event.target.value } : item) })} /></label>
                      <label><span>Route to</span><select value={rule.routeTo} onChange={event => setPatch({ rules: draft.rules.map((item, itemIndex) => itemIndex === index ? { ...item, routeTo: event.target.value } : item) })}><option value="">Select candidate</option>{draft.candidates.map(candidate => <option key={candidate} value={candidate}>{candidate}</option>)}</select></label>
                    </div>
                    <RouterNodeEditor node={rule.condition} classifiers={draft.classifiers} onChange={condition => setPatch({ rules: draft.rules.map((item, itemIndex) => itemIndex === index ? { ...item, condition } : item) })} />
                    <details className="router-editor__outputs">
                      <summary>Optional decision outputs</summary>
                      <textarea value={rule.outputsText || ''} placeholder={'{\n  "tier": "fast"\n}'} spellCheck={false} onChange={event => setPatch({ rules: draft.rules.map((item, itemIndex) => itemIndex === index ? { ...item, outputsText: event.target.value } : item) })} />
                    </details>
                  </article>
                ))}
                {draft.rules.length === 0 && <div className="router-editor__empty">At least one rule is required.</div>}
              </div>
            </section>
          </>
        )}

        {validationErrors.length > 0 && (
          <section className="router-editor__validation" aria-live="polite">
            <strong><Icon name="alert" size={14} /> {validationErrors.length} validation {validationErrors.length === 1 ? 'issue' : 'issues'}</strong>
            <ul>{validationErrors.slice(0, 8).map((message, index) => <li key={`${message}-${index}`}>{message}</li>)}</ul>
          </section>
        )}
        {error && <div className="router-editor__message router-editor__message--error"><Icon name="alert" size={14} /> {error}</div>}
        {notice && <div className="router-editor__message router-editor__message--success"><Icon name="check" size={14} /> {notice}</div>}
      </div>

      <footer className="router-editor__footer">
        {draft.modelName && <button type="button" className="btn btn--danger btn--tiny" onClick={() => { void deleteCurrent(); }}><Icon name="trash" size={13} /> Delete</button>}
        <span />
        <button type="button" className="btn btn--ghost" onClick={onClose}>Close</button>
        <button type="button" className="btn btn--primary" disabled={saving || validationErrors.length > 0} onClick={() => { void save(); }}>{saving ? 'Registering…' : 'Save & register'}</button>
      </footer>
    </section>
  );
};

export default RouterEditorPanel;
