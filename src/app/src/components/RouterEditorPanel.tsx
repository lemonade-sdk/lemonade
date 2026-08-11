import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import api, { type CloudProviderRow, type ModelInfo } from '../api';
import { capabilityFromModelInfo } from '../modelCapabilities';
import { Icon } from './Icon';
import RouterNodeEditor from './RouterNodeEditor';
import {
  WorkspaceActionButton,
  WorkspaceActionGroup,
  WorkspaceDetailPanel,
  WorkspaceMetadataChip,
} from './WorkspacePanels';
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
  routerDraftHasLlmProgress,
  routerDraftHasRulesProgress,
  routerDisplayName,
  switchRouterDraftMode,
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
import {
  describeRouterModelConnection,
  providerEndpointNeedsInsecureOptIn,
  validateProviderEndpoint,
} from '../features/router/routerConnections';

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
  initialModel?: ModelInfo | null;
  onRegister: (request: RouterPullRequest) => Promise<void>;
  onSaved?: (model: ModelInfo) => void;
  onDeleted?: (modelName: string) => Promise<void> | void;
  onClose: () => void;
}

export const RouterEditorPanel: React.FC<RouterEditorPanelProps> = ({
  models,
  initialModel,
  onRegister,
  onSaved,
  onDeleted,
  onClose,
}) => {
  const [draft, setDraft] = useState<RouterDraft>(() => createEmptyRouterDraft());
  const [savedRecords, setSavedRecords] = useState(() => loadRouterRecords());
  const [candidateSearch, setCandidateSearch] = useState('');
  const [tab, setTab] = useState<'builder' | 'json'>('builder');
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [notice, setNotice] = useState<string | null>(null);
  const [cloudProviders, setCloudProviders] = useState<CloudProviderRow[]>([]);
  const [connectionsError, setConnectionsError] = useState<string | null>(null);
  const [editingProvider, setEditingProvider] = useState<string | null>(null);
  const [editingConnectionModel, setEditingConnectionModel] = useState<string | null>(null);
  const [providerEndpointDraft, setProviderEndpointDraft] = useState('');
  const [providerAllowInsecureDraft, setProviderAllowInsecureDraft] = useState(false);
  const [savingProvider, setSavingProvider] = useState(false);
  const importRef = useRef<HTMLInputElement>(null);

  const refreshSaved = () => setSavedRecords(loadRouterRecords());
  const refreshCloudProviders = useCallback(async () => {
    try {
      setCloudProviders(await api.cloudProviders());
      setConnectionsError(null);
    } catch (providerError) {
      setConnectionsError(providerError instanceof Error ? providerError.message : 'Could not load external providers.');
    }
  }, []);

  useEffect(() => {
    setSavedRecords(loadRouterRecords());
    void refreshCloudProviders();
  }, [refreshCloudProviders]);

  useEffect(() => {
    if (!initialModel || String((initialModel as any).recipe || '').toLowerCase() !== 'collection.router') return;

    const applyModel = (sourceModel: ModelInfo) => {
      try {
        setDraft(routerDraftFromModelInfo(sourceModel));
        setError(null);
        setNotice(null);
      } catch (initialError) {
        setError(initialError instanceof Error ? initialError.message : 'Could not open this router.');
      }
    };

    const modelNameValue = modelName(initialModel);
    if (!modelNameValue) {
      applyModel(initialModel);
      return;
    }

    // The /models list can contain only summary fields. Always prefer the
    // authoritative detail record when editing an existing server router, just
    // like the current main RouterCollectionPanel. A locally persisted request
    // is used only as a fallback if the detail request itself fails.
    let cancelled = false;
    void api.modelDetail(modelNameValue)
      .then(detailedModel => {
        if (cancelled) return;
        applyModel(detailedModel);
      })
      .catch(detailError => {
        if (cancelled) return;
        if ((initialModel as any).routing) {
          applyModel(initialModel);
          return;
        }
        setError(detailError instanceof Error ? detailError.message : 'Could not load this router policy.');
      });
    return () => { cancelled = true; };
  }, [initialModel]);

  const candidateModels = useMemo(() => models
    .filter(model => String((model as any).recipe || '').toLowerCase() !== 'collection.router')
    .filter(model => {
      const labels = (model.labels || []).map(label => label.toLowerCase());
      if (labels.includes('classification') || labels.includes('classifier')) return false;
      const capability = capabilityFromModelInfo(model);
      return capability === 'chat' || capability === 'omni' || capability === 'unknown';
    })
    .sort((a, b) => modelLabel(a).localeCompare(modelLabel(b))), [models]);

  const filteredCandidateModels = useMemo(() => {
    const query = candidateSearch.trim().toLowerCase();
    if (!query) return candidateModels;
    return candidateModels.filter(model => `${modelLabel(model)} ${modelName(model)} ${(model.labels || []).join(' ')}`.toLowerCase().includes(query));
  }, [candidateModels, candidateSearch]);

  const embeddingModels = useMemo(() => models
    .filter(model => String((model as any).recipe || '').toLowerCase() !== 'collection.router')
    .filter(model => (model.labels || []).some(label => {
      const normalized = label.toLowerCase();
      return normalized === 'embedding' || normalized === 'embeddings';
    }))
    .sort((a, b) => modelLabel(a).localeCompare(modelLabel(b))), [models]);

  const classifierModels = useMemo(() => models
    .filter(model => String((model as any).recipe || '').toLowerCase() !== 'collection.router')
    .filter(model => (model.labels || []).some(label => label.toLowerCase() === 'classification'))
    .sort((a, b) => modelLabel(a).localeCompare(modelLabel(b))), [models]);

  const connectedModelRoles = useMemo(() => {
    const roles = new Map<string, string[]>();
    const add = (name: string, role: string) => {
      const normalized = name.trim();
      if (!normalized) return;
      const current = roles.get(normalized) || [];
      if (!current.includes(role)) current.push(role);
      roles.set(normalized, current);
    };
    draft.candidates.forEach(candidate => add(candidate, 'Routing target'));
    if (draft.mode === 'llm') add(draft.llmRouter.model, 'Natural-language router');
    if (draft.mode === 'rules') draft.classifiers.forEach(classifier => add(classifier.model, `Classifier · ${classifier.id || classifier.type}`));
    return roles;
  }, [draft.candidates, draft.mode, draft.llmRouter.model, draft.classifiers]);

  const selectedConnections = useMemo(() => [...connectedModelRoles.keys()].map(candidate =>
    describeRouterModelConnection(candidate, models, cloudProviders)
  ), [connectedModelRoles, models, cloudProviders]);

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

  const setRoutingMode = (mode: RouterDraft['mode']) => {
    if (mode === draft.mode) return;

    if (draft.mode === 'rules') {
      if (routerDraftHasRulesProgress(draft) && !window.confirm(
        'Switch to Natural-language router?\n\nSwitching modes will clear your existing rules and classifiers. This cannot be undone.',
      )) return;
      setDraft(current => switchRouterDraftMode(current, 'llm'));
    } else {
      if (routerDraftHasLlmProgress(draft) && !window.confirm(
        'Switch to ordered rules?\n\nSwitching modes will clear your Natural-language routing model and instruction. This cannot be undone.',
      )) return;
      setDraft(current => switchRouterDraftMode(current, 'rules'));
    }
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

  const startEditingProvider = (provider: string, endpoint: string, modelNameValue: string, allowInsecureHttp: boolean) => {
    setEditingProvider(provider);
    setEditingConnectionModel(modelNameValue);
    setProviderEndpointDraft(endpoint);
    setProviderAllowInsecureDraft(allowInsecureHttp);
    setConnectionsError(null);
  };

  const saveProviderEndpoint = async () => {
    if (!editingProvider) return;
    const endpointError = validateProviderEndpoint(providerEndpointDraft, providerAllowInsecureDraft);
    if (endpointError) {
      setConnectionsError(endpointError);
      return;
    }
    setSavingProvider(true);
    setConnectionsError(null);
    try {
      await api.installCloudProvider(editingProvider, providerEndpointDraft.trim(), undefined, providerAllowInsecureDraft);
      await refreshCloudProviders();
      setEditingProvider(null);
      setEditingConnectionModel(null);
      setProviderEndpointDraft('');
      setProviderAllowInsecureDraft(false);
      setNotice(`Updated ${editingProvider} endpoint.`);
    } catch (providerError) {
      setConnectionsError(providerError instanceof Error ? providerError.message : 'Could not update provider endpoint.');
    } finally {
      setSavingProvider(false);
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
      const record = upsertRouterRecord({ ...draft, modelName: nextRequest.model_name });
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
    <WorkspaceDetailPanel
      className="router-editor"
      ariaLabel="Router editor"
      leading={<Icon name="router" size={20} aria-hidden="true" />}
      title={<h2 className="workspace-detail-panel__title">Router</h2>}
      metadata={<WorkspaceMetadataChip emphasis="high" tone="accent">collection.router</WorkspaceMetadataChip>}
      description={<p>Build and register a virtual model that routes requests across compatible candidates.</p>}
      actions={(
        <WorkspaceActionGroup label="Router editor actions">
          <WorkspaceActionButton appearance="primary" icon="check" disabled={saving || validationErrors.length > 0} onClick={() => { void save(); }}>
            {saving ? 'Registering…' : 'Save & register'}
          </WorkspaceActionButton>
          {draft.modelName && (
            <WorkspaceActionButton appearance="danger" icon="trash" onClick={() => { void deleteCurrent(); }}>Delete</WorkspaceActionButton>
          )}
          <span className="workspace-action-group__spacer" />
          <WorkspaceActionButton appearance="secondary" icon="x" onClick={onClose}>Close</WorkspaceActionButton>
        </WorkspaceActionGroup>
      )}
      onClose={onClose}
      closeLabel="Close router editor"
    >

      <div className="router-editor__toolbar">
        <WorkspaceActionButton size="small" icon="compose" onClick={resetDraft}>New</WorkspaceActionButton>
        <label className="router-editor__saved-select">
          <span className="sr-only">Saved routers</span>
          <select className="select" value={draft.modelName || ''} onChange={event => loadSaved(event.target.value)}>
            <option value="">Unsaved router</option>
            {savedRecords.map(record => <option key={record.model_name} value={record.model_name}>{record.display_name}</option>)}
          </select>
        </label>
        <span className="router-editor__toolbar-spacer" />
        <WorkspaceActionButton size="small" icon="file-up" onClick={() => importRef.current?.click()}>Import</WorkspaceActionButton>
        <WorkspaceActionButton size="small" icon="file" disabled={!request} onClick={() => request && downloadJson(routerDisplayName(request.model_name), request)}>Export</WorkspaceActionButton>
        <input ref={importRef} className="hidden-file-input" type="file" accept="application/json,.json" onChange={event => { void importFile(event.target.files?.[0]); }} />
      </div>

      <div className="router-editor__tabs" role="tablist" aria-label="Router editor view">
        <button type="button" className={tab === 'builder' ? 'is-active' : ''} role="tab" aria-selected={tab === 'builder'} onClick={() => setTab('builder')}>Builder</button>
        <button type="button" className={tab === 'json' ? 'is-active' : ''} role="tab" aria-selected={tab === 'json'} onClick={() => setTab('json')}>JSON preview</button>
      </div>

      <div className="router-editor__body">
        <div className="router-editor__backend-note">
          <Icon name="info" size={15} />
          <span>Router v1 supports ordered rules, model-backed signals, and natural-language routing. The default model remains the fail-open fallback.</span>
        </div>

        {tab === 'json' ? (
          <section className="router-editor__json-panel">
            <div className="router-editor__section-head">
              <div><h3>Registration payload</h3><p>Exact body sent to <code>/api/v1/pull</code>.</p></div>
              <WorkspaceActionButton size="small" icon="copy" disabled={!jsonPreview} onClick={() => { void copyText(jsonPreview).then(() => setNotice('JSON copied.')); }}>Copy</WorkspaceActionButton>
            </div>
            {jsonPreview ? <pre>{jsonPreview}</pre> : <div className="router-editor__empty">Fix validation errors to generate the payload.</div>}
          </section>
        ) : (
          <>
            <section className="router-editor__section">
              <div className="router-editor__section-head">
                <div><h3>Identity</h3><p>Saved as a virtual model.</p></div>
              </div>
              <div className="router-editor__form-grid">
                <label><span>Router name</span><input className="input" value={draft.name} placeholder="Fast-or-smart" onChange={event => setPatch({ name: event.target.value })} /></label>
                <label><span>Model ID</span><input className="input" value={draft.modelName || (draft.name ? `user.${draft.name.replace(/[^A-Za-z0-9._-]+/g, '-')}` : '')} readOnly /></label>
              </div>
            </section>

            <section className="router-editor__section">
              <div className="router-editor__section-head">
                <div><h3>Candidate models</h3><p>The router may select only from these registered models.</p></div>
                <span className="router-editor__count">{draft.candidates.length} selected</span>
              </div>
              <div className="router-editor__candidate-search"><Icon name="search" size={14} /><input value={candidateSearch} placeholder="Search registered models" onChange={event => setCandidateSearch(event.target.value)} /></div>
              <div className="router-editor__candidate-list">
                {filteredCandidateModels.map(model => {
                  const name = modelName(model);
                  const checked = draft.candidates.includes(name);
                  const connection = describeRouterModelConnection(name, models, cloudProviders);
                  return (
                    <label key={name} className={`router-editor__candidate ${checked ? 'is-selected' : ''}`}>
                      <input type="checkbox" checked={checked} onChange={() => toggleCandidate(name)} />
                      <span className="router-editor__candidate-main">
                        <strong>{modelLabel(model)}</strong>
                        <small>{name}</small>
                        {connection.kind === 'external' && connection.endpoint && <small title={connection.endpoint}>{connection.endpoint}</small>}
                      </span>
                      <span className={`router-editor__source-badge router-editor__source-badge--${connection.kind}`}>
                        {connection.kind === 'external' ? `External · ${connection.provider || 'provider'}` : 'Internal'}
                      </span>
                    </label>
                  );
                })}
                {filteredCandidateModels.length === 0 && <div className="router-editor__empty">No compatible models match this search.</div>}
              </div>
              <label className="router-editor__default-model">
                <span>Default model <small>Used when no rule matches or evaluation fails.</small></span>
                <select className="select" value={draft.defaultModel} onChange={event => setPatch({ defaultModel: event.target.value })}>
                  <option value="">Select default</option>
                  {draft.candidates.map(candidate => <option key={candidate} value={candidate}>{candidate}</option>)}
                </select>
              </label>

              <div className="router-editor__connections" aria-label="Connected model topology">
                <div className="router-editor__mini-head">
                  <span>Connected model topology</span>
                  <small>External endpoints are provider-wide and affect every model from that provider.</small>
                </div>
                {selectedConnections.length === 0 ? (
                  <div className="router-editor__empty">Select candidate models to review their connections.</div>
                ) : selectedConnections.map(connection => (
                  <div className="router-editor__connection" key={connection.modelName}>
                    <div className="router-editor__connection-main">
                      <div>
                        <strong>{connection.displayName}</strong>
                        {connection.modelName === draft.defaultModel && <span className="router-editor__default-badge">Default</span>}
                      </div>
                      <small>{connection.modelName}</small>
                      <small>{(connectedModelRoles.get(connection.modelName) || []).join(' · ')}</small>
                    </div>
                    <div className="router-editor__connection-source">
                      <span className={`router-editor__source-badge router-editor__source-badge--${connection.kind}`}>
                        {connection.kind === 'external' ? 'External' : connection.kind === 'internal' ? 'Internal' : 'Unresolved'}
                      </span>
                      <small>{connection.kind === 'external' ? (connection.provider || 'Unknown provider') : (connection.backend || connection.recipe || 'Local model')}</small>
                    </div>
                    {connection.kind === 'external' ? (
                      <div className="router-editor__connection-endpoint">
                        {editingProvider === connection.provider && editingConnectionModel === connection.modelName ? (
                          <div className="router-editor__endpoint-editor">
                            <input
                              className="input"
                              value={providerEndpointDraft}
                              aria-label={`${connection.provider} endpoint`}
                              placeholder="https://api.example.com/v1"
                              onChange={event => setProviderEndpointDraft(event.target.value)}
                            />
                            {providerEndpointNeedsInsecureOptIn(providerEndpointDraft) && (
                              <label className="router-editor__insecure-opt-in">
                                <input type="checkbox" checked={providerAllowInsecureDraft} onChange={event => setProviderAllowInsecureDraft(event.target.checked)} />
                                <span>Allow insecure HTTP</span>
                              </label>
                            )}
                            <WorkspaceActionButton size="small" appearance="primary" disabled={savingProvider} onClick={() => { void saveProviderEndpoint(); }}>
                              {savingProvider ? 'Saving…' : 'Save'}
                            </WorkspaceActionButton>
                            <WorkspaceActionButton size="small" onClick={() => { setEditingProvider(null); setEditingConnectionModel(null); setProviderAllowInsecureDraft(false); setConnectionsError(null); }}>Cancel</WorkspaceActionButton>
                          </div>
                        ) : (
                          <>
                            <span title={connection.endpoint || 'Endpoint unavailable'}>{connection.endpoint || 'Endpoint not configured'}</span>
                            <small>{connection.authConfigured ? 'Authentication configured' : 'Authentication required'}</small>
                            {connection.provider && (
                              <WorkspaceActionButton size="small" icon="edit" onClick={() => startEditingProvider(connection.provider, connection.endpoint, connection.modelName, connection.allowInsecureHttp)}>
                                Edit endpoint
                              </WorkspaceActionButton>
                            )}
                          </>
                        )}
                      </div>
                    ) : (
                      <div className="router-editor__connection-endpoint"><span>Managed by Lemonade</span><small>Local registered model</small></div>
                    )}
                  </div>
                ))}
                {connectionsError && selectedConnections.some(connection => connection.kind === 'external') && <div className="router-editor__message router-editor__message--error"><Icon name="alert" size={14} /> {connectionsError}</div>}
              </div>
            </section>

            <section className="router-editor__section">
              <div className="router-editor__section-head">
                <div><h3>Routing strategy</h3><p>Choose how the router selects a candidate. Switching modes clears incompatible configuration after confirmation.</p></div>
              </div>
              <div className="router-editor__strategy" role="radiogroup" aria-label="Routing strategy">
                <button
                  type="button"
                  className={`router-editor__strategy-option ${draft.mode === 'rules' ? 'is-active' : ''}`}
                  role="radio"
                  aria-checked={draft.mode === 'rules'}
                  onClick={() => setRoutingMode('rules')}
                >
                  <Icon name="layers" size={18} />
                  <span><strong>Ordered rules</strong><small>Deterministic conditions and optional model-backed signals. First match wins.</small></span>
                </button>
                <button
                  type="button"
                  className={`router-editor__strategy-option ${draft.mode === 'llm' ? 'is-active' : ''}`}
                  role="radio"
                  aria-checked={draft.mode === 'llm'}
                  onClick={() => setRoutingMode('llm')}
                >
                  <Icon name="brain-circuit" size={18} />
                  <span><strong>Natural-language router</strong><small>A routing model reads your instruction and chooses one candidate directly.</small></span>
                </button>
              </div>
            </section>

            {draft.mode === 'llm' ? (
              <section className="router-editor__section">
                <div className="router-editor__section-head">
                  <div><h3>Natural-language router</h3><p>The selected model must return exactly one candidate. Invalid output falls back to the default model.</p></div>
                  <WorkspaceMetadataChip emphasis="high" tone="accent">routing.router</WorkspaceMetadataChip>
                </div>
                <div className="router-editor__form-grid">
                  <label className="router-editor__wide">
                    <span>Routing model <small>Usually a small, fast chat model.</small></span>
                    <select
                      className="select"
                      value={draft.llmRouter.model}
                      onChange={event => setPatch({ llmRouter: { ...draft.llmRouter, model: event.target.value } })}
                    >
                      <option value="">Select routing model</option>
                      {candidateModels.map(model => <option key={modelName(model)} value={modelName(model)}>{modelLabel(model)} · {modelName(model)}</option>)}
                    </select>
                  </label>
                  <label className="router-editor__wide">
                    <span>Routing instruction <small>Describe clearly when each candidate should be selected.</small></span>
                    <textarea
                      className="textarea router-editor__prompt"
                      value={draft.llmRouter.prompt}
                      placeholder="Use the fast model for everyday questions. Use the larger model for difficult reasoning, coding, or long context."
                      spellCheck={false}
                      onChange={event => setPatch({ llmRouter: { ...draft.llmRouter, prompt: event.target.value } })}
                    />
                  </label>
                </div>
              </section>
            ) : (
              <>
            <section className="router-editor__section">
              <div className="router-editor__section-head">
                <div><h3>Classifiers</h3><p>Optional model-backed signals evaluated once per request.</p></div>
                <div className="router-editor__section-actions">
                  <WorkspaceActionButton size="small" icon="plus" onClick={() => addClassifier('classifier')}>Classifier</WorkspaceActionButton>
                  <WorkspaceActionButton size="small" icon="plus" onClick={() => addClassifier('semantic_similarity')}>Semantic</WorkspaceActionButton>
                  <WorkspaceActionButton size="small" icon="plus" onClick={() => addClassifier('llm')}>LLM signal</WorkspaceActionButton>
                </div>
              </div>
              {draft.classifiers.length === 0 ? <div className="router-editor__empty">No classifiers. Deterministic rules need none.</div> : (
                <div className="router-editor__classifier-list">
                  {draft.classifiers.map((classifier, index) => {
                    const labels = classifierLabels(classifier);
                    return (
                      <article className="router-editor__classifier" key={`${classifier.id}-${index}`}>
                        <div className="router-editor__card-head">
                          <strong>{classifier.type === 'semantic_similarity' ? 'Semantic similarity' : classifier.type === 'llm' ? 'LLM classifier' : 'Text classifier'}</strong>
                          <WorkspaceActionButton appearance="danger" size="small" icon="trash" iconOnly onClick={() => removeClassifier(index)} aria-label="Remove classifier" title="Remove classifier" />
                        </div>
                        <div className="router-editor__form-grid router-editor__form-grid--classifier">
                          <label><span>ID</span><input className="input" value={classifier.id} onChange={event => updateClassifier(index, { id: event.target.value })} /></label>
                          <label><span>Type</span><select className="select" value={classifier.type} onChange={event => updateClassifier(index, { ...createRouterClassifier(index, event.target.value as RouterClassifier['type']), id: classifier.id })}><option value="classifier">classifier</option><option value="semantic_similarity">semantic_similarity</option><option value="llm">llm</option></select></label>
                          <label className="router-editor__wide"><span>Model</span><select className="select" value={classifier.model} onChange={event => updateClassifier(index, { model: event.target.value })}><option value="">Select model</option>{(classifier.type === 'semantic_similarity' ? embeddingModels : classifier.type === 'llm' ? candidateModels : classifierModels).map(model => <option key={modelName(model)} value={modelName(model)}>{modelLabel(model)} · {modelName(model)}</option>)}</select></label>
                          {classifier.type === 'semantic_similarity' ? (
                            <div className="router-editor__wide router-editor__concepts">
                              <div className="router-editor__mini-head"><span>Concepts and reference phrases</span><WorkspaceActionButton size="small" icon="plus" onClick={() => updateClassifier(index, { referencePhrases: { ...classifier.referencePhrases, [nextConceptName(classifier.referencePhrases)]: ['example phrase'] } })}>Concept</WorkspaceActionButton></div>
                              {Object.entries(classifier.referencePhrases).map(([concept, phrases], conceptIndex) => (
                                <div className="router-editor__concept" key={`${concept}-${conceptIndex}`}>
                                  <input className="input" value={concept} aria-label="Concept name" onChange={event => renameSemanticConcept(index, concept, event.target.value)} />
                                  <input className="input" value={phrases.join(', ')} aria-label="Reference phrases" placeholder="Comma-separated phrases" onChange={event => updateClassifier(index, { referencePhrases: { ...classifier.referencePhrases, [concept]: event.target.value.split(',').map(phrase => phrase.trimStart()) } })} />
                                  <WorkspaceActionButton appearance="danger" size="small" icon="trash" iconOnly title="Remove concept" aria-label="Remove concept" onClick={() => { const next = { ...classifier.referencePhrases }; delete next[concept]; updateClassifier(index, { referencePhrases: next, defaultLabel: undefined }); }} />
                                </div>
                              ))}
                            </div>
                          ) : (
                            <>
                              {classifier.type === 'llm' && (
                                <label className="router-editor__wide">
                                  <span>Classification prompt <small>Explain when each label applies.</small></span>
                                  <textarea className="textarea router-editor__prompt" value={classifier.prompt} placeholder="Choose SAFE for routine requests and RISKY for requests that could cause external side effects." spellCheck={false} onChange={event => updateClassifier(index, { prompt: event.target.value })} />
                                </label>
                              )}
                              <label className="router-editor__wide"><span>Output labels <small>Comma-separated</small></span><input className="input" value={classifier.labels.join(', ')} onChange={event => updateClassifier(index, { labels: event.target.value.split(',').map(label => label.trimStart()), defaultLabel: undefined })} /></label>
                            </>
                          )}
                          <label><span>Default label</span><select className="select" value={classifier.defaultLabel || ''} onChange={event => updateClassifier(index, { defaultLabel: event.target.value || undefined })}><option value="">None</option>{labels.map(label => <option key={label} value={label}>{label}</option>)}</select></label>
                          <label><span>On error</span><select className="select" value={classifier.onError} onChange={event => updateClassifier(index, { onError: event.target.value as RouterClassifier['onError'] })}><option value="match_false">Do not match</option><option value="match_true">Match rule</option></select></label>
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
                <WorkspaceActionButton size="small" icon="plus" onClick={addRule}>Rule</WorkspaceActionButton>
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
                      <label><span>Rule ID</span><input className="input" value={rule.id} onChange={event => setPatch({ rules: draft.rules.map((item, itemIndex) => itemIndex === index ? { ...item, id: event.target.value } : item) })} /></label>
                      <label><span>Route to</span><select className="select" value={rule.routeTo} onChange={event => setPatch({ rules: draft.rules.map((item, itemIndex) => itemIndex === index ? { ...item, routeTo: event.target.value } : item) })}><option value="">Select candidate</option>{draft.candidates.map(candidate => <option key={candidate} value={candidate}>{candidate}</option>)}</select></label>
                    </div>
                    <RouterNodeEditor node={rule.condition} classifiers={draft.classifiers} onChange={condition => setPatch({ rules: draft.rules.map((item, itemIndex) => itemIndex === index ? { ...item, condition } : item) })} />
                    <details className="router-editor__outputs">
                      <summary>Optional decision outputs</summary>
                      <textarea className="textarea" value={rule.outputsText || ''} placeholder={'{\n  "tier": "fast"\n}'} spellCheck={false} onChange={event => setPatch({ rules: draft.rules.map((item, itemIndex) => itemIndex === index ? { ...item, outputsText: event.target.value } : item) })} />
                    </details>
                  </article>
                ))}
                {draft.rules.length === 0 && <div className="router-editor__empty">At least one rule is required.</div>}
              </div>
            </section>
              </>
            )}
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

    </WorkspaceDetailPanel>
  );
};

export default RouterEditorPanel;
