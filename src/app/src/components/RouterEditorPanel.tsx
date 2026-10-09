import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import api, { type CloudProviderRow, type ModelInfo } from '../api';
import { useI18n } from '../i18n';
import { capabilityFromModelInfo, isRouterRecipe } from '../modelCapabilities';
import { Icon } from './Icon';
import Modal from './inspect/Modal';
import RouterModelPicker from './RouterModelPicker';
import RouterRuleGraph from './RouterRuleGraph';
import { RouterSelect } from './RouterNodeEditor';
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
  normalizeRouterModelName,
  parseRouterPayload,
  renameClassifierReference,
  renameClassifierLabelReference,
  routerNodeReferencesClassifier,
  routerDraftFromModelInfo,
  routerDraftFingerprint,
  routerDraftHasLlmProgress,
  routerDraftHasRulesProgress,
  routerDisplayName,
  switchRouterDraftMode,
  toggleRouterDraftCandidate,
  validateRouterDraft,
  type RouterClassifier,
  type RouterDraft,
  type RouterPullRequest,
} from '../features/router/routerTypes';
import {
  describeRouterModelConnection,
  providerEndpointNeedsInsecureOptIn,
  validateProviderEndpoint,
} from '../features/router/routerConnections';
import { preflightRouter } from '../features/router/routerRuntime';

function modelName(model: ModelInfo | null | undefined): string {
  if (!model) return '';
  return String((model as any).model_name ?? model.name ?? model.id ?? '').trim();
}

function modelLabel(model: ModelInfo): string {
  return String(model.display_name || modelName(model));
}

function routerRequestToModelInfo(request: RouterPullRequest, draft: RouterDraft): ModelInfo {
  return {
    id: request.model_name,
    name: request.model_name,
    model_name: request.model_name,
    display_name: draft.name.trim() || routerDisplayName(request.model_name),
    recipe: request.recipe,
    type: 'chat',
    labels: ['custom', 'router', 'chat'],
    downloaded: true,
    custom: true,
    version: request.version,
    components: request.components,
    routing: request.routing,
  } as ModelInfo;
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
  try {
    textarea.select();
    const copied = document.execCommand('copy');
    if (!copied) throw new Error('Clipboard copy was not accepted.');
  } finally {
    textarea.remove();
  }
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

const CommittedTextInput: React.FC<{
  value: string;
  ariaLabel?: string;
  className?: string;
  onCommit: (next: string) => boolean;
  normalize?: (value: string) => string;
}> = ({ value, ariaLabel, className = 'input', onCommit, normalize = input => input.trim() }) => {
  const [editingValue, setEditingValue] = useState(value);
  const cancelCommitRef = useRef(false);
  useEffect(() => setEditingValue(value), [value]);

  const commit = () => {
    if (cancelCommitRef.current) {
      cancelCommitRef.current = false;
      setEditingValue(value);
      return;
    }
    const next = normalize(editingValue);
    if (next === value) {
      if (editingValue !== value) setEditingValue(value);
      return;
    }
    if (!onCommit(next)) setEditingValue(value);
  };

  return (
    <input
      className={className}
      value={editingValue}
      aria-label={ariaLabel}
      onChange={event => setEditingValue(event.target.value)}
      onBlur={commit}
      onKeyDown={event => {
        if (event.key === 'Enter') {
          event.preventDefault();
          event.currentTarget.blur();
        } else if (event.key === 'Escape') {
          event.preventDefault();
          cancelCommitRef.current = true;
          setEditingValue(value);
          event.currentTarget.blur();
        }
      }}
    />
  );
};

interface RouterEditorPanelProps {
  models: ModelInfo[];
  initialModel?: ModelInfo | null;
  onRegister: (request: RouterPullRequest, displayName?: string) => Promise<void>;
  onSaved?: (model: ModelInfo) => void;
  onDeleted?: (modelName: string) => Promise<string | void> | string | void;
  onClose: () => void;
}

type RouterConfirmationKind = 'switch-rules' | 'switch-llm' | 'reset' | 'delete' | 'discard';

interface RouterConfirmation {
  kind: RouterConfirmationKind;
  title: string;
  message: string;
  confirmLabel: string;
  tone: 'primary' | 'danger';
}

function moveItemTo<T>(items: T[], from: number, to: number): T[] {
  if (from === to || from < 0 || to < 0 || from >= items.length || to >= items.length) return items;
  const next = [...items];
  const [item] = next.splice(from, 1);
  next.splice(to, 0, item);
  return next;
}

export const RouterEditorPanel: React.FC<RouterEditorPanelProps> = ({
  models,
  initialModel,
  onRegister,
  onSaved,
  onDeleted,
  onClose,
}) => {
  const { t } = useI18n();
  const [draft, setDraft] = useState<RouterDraft>(() => createEmptyRouterDraft());
  const savedRecords = useMemo(
    () => models
      .filter(model => isRouterRecipe((model as any).recipe))
      .sort((a, b) => modelLabel(a).localeCompare(modelLabel(b))),
    [models],
  );
  const [candidateSearch, setCandidateSearch] = useState('');
  const [tab, setTab] = useState<'builder' | 'json'>('builder');
  const [saving, setSaving] = useState(false);
  const [deleting, setDeleting] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [notice, setNotice] = useState<string | null>(null);
  const [jsonCopied, setJsonCopied] = useState(false);
  const [confirmation, setConfirmation] = useState<RouterConfirmation | null>(null);
  const [dragRuleIndex, setDragRuleIndex] = useState<number | null>(null);
  const [selectedRuleIndex, setSelectedRuleIndex] = useState(0);
  const [expandedRuleIndex, setExpandedRuleIndex] = useState<number | null>(null);
  // Tracks rule indices whose graph has been committed at least once. Needed to
  // pass initialCommitted=true to the expanded RouterRuleGraph so a blank
  // keywords_any node (textValue='') isn't mistaken for the initial empty state.
  const committedRuleIndicesRef = useRef<Set<number>>(new Set());
  const [cloudProviders, setCloudProviders] = useState<CloudProviderRow[]>([]);
  const [connectionsError, setConnectionsError] = useState<string | null>(null);
  const [editingProvider, setEditingProvider] = useState<string | null>(null);
  const [editingConnectionModel, setEditingConnectionModel] = useState<string | null>(null);
  const [providerEndpointDraft, setProviderEndpointDraft] = useState('');
  const [providerAllowInsecureDraft, setProviderAllowInsecureDraft] = useState(false);
  const [savingProvider, setSavingProvider] = useState(false);
  const importRef = useRef<HTMLInputElement>(null);
  const jsonCopyTimeoutRef = useRef<number | null>(null);
  const baselineFingerprintRef = useRef(routerDraftFingerprint(createEmptyRouterDraft()));
  const draftFingerprint = useMemo(() => routerDraftFingerprint(draft), [draft]);
  const draftFingerprintRef = useRef(draftFingerprint);
  const pendingDiscardActionRef = useRef<(() => void) | null>(null);
  const initialModelKeyRef = useRef<string | null>(null);
  draftFingerprintRef.current = draftFingerprint;
  const isDirty = draftFingerprint !== baselineFingerprintRef.current;

  const markBaseline = (nextDraft: RouterDraft) => {
    baselineFingerprintRef.current = routerDraftFingerprint(nextDraft);
  };

  const requestDiscard = (title: string, message: string, confirmLabel: string, action: () => void) => {
    pendingDiscardActionRef.current = action;
    setConfirmation({ kind: 'discard', title, message, confirmLabel, tone: 'danger' });
  };

  const refreshCloudProviders = useCallback(async () => {
    try {
      setCloudProviders(await api.cloudProviders());
      setConnectionsError(null);
    } catch (providerError) {
      setConnectionsError(providerError instanceof Error ? providerError.message : 'Could not load external providers.');
    }
  }, []);

  useEffect(() => {
    void refreshCloudProviders();
  }, [refreshCloudProviders]);

  useEffect(() => {
    if (!notice) return;
    const timeout = window.setTimeout(() => setNotice(current => current === notice ? null : current), 4200);
    return () => window.clearTimeout(timeout);
  }, [notice]);

  useEffect(() => () => {
    if (jsonCopyTimeoutRef.current != null) window.clearTimeout(jsonCopyTimeoutRef.current);
  }, []);

  useEffect(() => {
    if (draft.rules.length === 0) {
      if (selectedRuleIndex !== 0) setSelectedRuleIndex(0);
      return;
    }
    if (selectedRuleIndex >= draft.rules.length) setSelectedRuleIndex(draft.rules.length - 1);
  }, [draft.rules.length, selectedRuleIndex]);

  useEffect(() => {
    if (expandedRuleIndex == null) return;
    if (draft.mode !== 'rules' || expandedRuleIndex < 0 || expandedRuleIndex >= draft.rules.length) {
      setExpandedRuleIndex(null);
    }
  }, [draft.mode, draft.rules.length, expandedRuleIndex]);

  useEffect(() => {
    if (!initialModel || !isRouterRecipe((initialModel as any).recipe)) return;

    const modelNameValue = modelName(initialModel);
    const modelKey = modelNameValue || '__inline-router__';
    if (initialModelKeyRef.current === modelKey) return;
    initialModelKeyRef.current = modelKey;

    const applyLoadedDraft = (nextDraft: RouterDraft) => {
      markBaseline(nextDraft);
      setDraft(nextDraft);
      setSelectedRuleIndex(0);
      setExpandedRuleIndex(null);
      setError(null);
      setNotice(null);
    };

    if (!modelNameValue) {
      try { applyLoadedDraft(routerDraftFromModelInfo(initialModel)); }
      catch (initialError) {
        initialModelKeyRef.current = null;
        setError(initialError instanceof Error ? initialError.message : 'Could not open this router.');
      }
      return;
    }

    // The list endpoint may expose summary-only router fields. Fetch the
    // authoritative detail before using any cached/list routing payload. A late
    // response is guarded by the dirty baseline so it can never clobber edits.
    const baselineAtDetailStart = baselineFingerprintRef.current;
    let cancelled = false;
    void api.modelDetail(modelNameValue)
      .then(detailedModel => {
        if (cancelled) return;
        let detailedDraft: RouterDraft;
        try {
          detailedDraft = routerDraftFromModelInfo(detailedModel);
        } catch (detailParseError) {
          initialModelKeyRef.current = null;
          setError(detailParseError instanceof Error ? detailParseError.message : 'Could not open this router.');
          return;
        }
        if (draftFingerprintRef.current === baselineAtDetailStart) {
          applyLoadedDraft(detailedDraft);
          return;
        }
        requestDiscard(
          'Load router policy?',
          'The full router policy finished loading after you started editing. Loading it now will discard the changes currently in the editor.',
          'Load and discard changes',
          () => applyLoadedDraft(detailedDraft),
        );
      })
      .catch(detailError => {
        if (cancelled) return;
        if ((initialModel as any).routing) {
          let fallbackDraft: RouterDraft;
          try {
            fallbackDraft = routerDraftFromModelInfo(initialModel);
          } catch (fallbackError) {
            initialModelKeyRef.current = null;
            setError(fallbackError instanceof Error ? fallbackError.message : 'Could not load this router policy.');
            return;
          }
          if (draftFingerprintRef.current === baselineAtDetailStart) {
            applyLoadedDraft(fallbackDraft);
          } else {
            requestDiscard(
              'Load cached router policy?',
              'The server detail request failed, but cached router data is available. Loading it will discard the changes currently in the editor.',
              'Load cached data',
              () => applyLoadedDraft(fallbackDraft),
            );
          }
          return;
        }
        initialModelKeyRef.current = null;
        setError(detailError instanceof Error ? detailError.message : 'Could not load this router policy.');
      });
    return () => { cancelled = true; };
  }, [initialModel]);

  const candidateModels = useMemo(() => models
    .filter(model => !isRouterRecipe((model as any).recipe))
    .filter(model => {
      const labels = (model.labels || []).map(label => label.toLowerCase());
      if (labels.includes('classification') || labels.includes('classifier')) return false;
      const capability = capabilityFromModelInfo(model);
      return capability === 'chat';
    })
    .sort((a, b) => modelLabel(a).localeCompare(modelLabel(b))), [models]);

  const filteredCandidateModels = useMemo(() => {
    const query = candidateSearch.trim().toLowerCase();
    if (!query) return candidateModels;
    return candidateModels.filter(model => `${modelLabel(model)} ${modelName(model)} ${(model.labels || []).join(' ')}`.toLowerCase().includes(query));
  }, [candidateModels, candidateSearch]);

  const embeddingModels = useMemo(() => models
    .filter(model => !isRouterRecipe((model as any).recipe))
    .filter(model => (model.labels || []).some(label => {
      const normalized = label.toLowerCase();
      return normalized === 'embedding' || normalized === 'embeddings';
    }))
    .sort((a, b) => modelLabel(a).localeCompare(modelLabel(b))), [models]);

  const classifierModels = useMemo(() => models
    .filter(model => !isRouterRecipe((model as any).recipe))
    .filter(model => (model.labels || []).some(label => label.toLowerCase() === 'classification'))
    .sort((a, b) => modelLabel(a).localeCompare(modelLabel(b))), [models]);

  const connectedModelNames = useMemo(() => {
    const names = new Set<string>();
    const add = (name: string) => {
      // Model/component identifiers are exact server identities. Do not trim
      // them here or the connection row can stop matching the draft/payload.
      if (!name.length) return;
      names.add(name);
    };
    draft.candidates.forEach(add);
    if (draft.mode === 'llm') add(draft.llmRouter.model);
    if (draft.mode === 'rules') draft.classifiers.forEach(classifier => add(classifier.model));
    return names;
  }, [draft.candidates, draft.mode, draft.llmRouter.model, draft.classifiers]);

  const selectedConnections = useMemo(() => [...connectedModelNames].map(candidate => ({
    ...describeRouterModelConnection(candidate, models, cloudProviders),
    // The connection helper normalizes for catalog lookup/display. Keep the
    // draft's exact component identity for default badges and candidate removal.
    modelName: candidate,
  })), [connectedModelNames, models, cloudProviders]);

  const validationErrors = useMemo(() => validateRouterDraft(draft, t), [draft, t]);
  const request = useMemo(() => {
    try { return buildRouterPullRequest(draft); } catch { return null; }
  }, [draft]);
  const jsonPreview = useMemo(() => request ? JSON.stringify(request, null, 2) : '', [request]);

  useEffect(() => {
    // A successful copy only describes the exact payload that was copied. As
    // soon as the draft changes, return the button to its normal state rather
    // than showing a stale "Copied" confirmation for a different payload.
    setJsonCopied(false);
  }, [jsonPreview]);

  const setPatch = (patch: Partial<RouterDraft>) => {
    setDraft(current => ({ ...current, ...patch }));
    setError(null);
    setNotice(null);
  };

  const copyJsonPreview = async () => {
    if (!jsonPreview) return;
    try {
      await copyText(jsonPreview);
      setJsonCopied(true);
      if (jsonCopyTimeoutRef.current != null) window.clearTimeout(jsonCopyTimeoutRef.current);
      jsonCopyTimeoutRef.current = window.setTimeout(() => {
        setJsonCopied(false);
        jsonCopyTimeoutRef.current = null;
      }, 2200);
    } catch (copyError) {
      setError(copyError instanceof Error ? copyError.message : t('Could not copy router JSON.'));
    }
  };

  const applyRoutingMode = (mode: RouterDraft['mode']) => {
    setDraft(current => switchRouterDraftMode(current, mode));
    setSelectedRuleIndex(0);
    setExpandedRuleIndex(null);
    setError(null);
    setNotice(null);
  };

  const setRoutingMode = (mode: RouterDraft['mode']) => {
    if (mode === draft.mode) return;
    if (draft.mode === 'rules' && routerDraftHasRulesProgress(draft)) {
      setConfirmation({
        kind: 'switch-llm',
        title: t('Switch routing strategy?'),
        message: t('Switching to the Natural-language router will clear the current ordered rules and classifiers. This cannot be undone.'),
        confirmLabel: t('Switch and clear rules'),
        tone: 'danger',
      });
      return;
    }
    if (draft.mode === 'llm' && routerDraftHasLlmProgress(draft)) {
      setConfirmation({
        kind: 'switch-rules',
        title: t('Switch routing strategy?'),
        message: t('Switching to Ordered rules will clear the current routing model and instruction. This cannot be undone.'),
        confirmLabel: t('Switch and clear NL router'),
        tone: 'danger',
      });
      return;
    }
    applyRoutingMode(mode);
  };

  const resetDraft = () => {
    const nextDraft = createEmptyRouterDraft();
    markBaseline(nextDraft);
    // Keep the last processed initial-model key. `initialModel` is an opener
    // prop, not the current draft identity; clearing it here would let a later
    // same-router list refresh silently rehydrate the router we just replaced
    // with a blank draft.
    setDraft(nextDraft);
    setSelectedRuleIndex(0);
    setError(null);
    setNotice(null);
    setTab('builder');
    setCandidateSearch('');
    setDragRuleIndex(null);
    setExpandedRuleIndex(null);
    setJsonCopied(false);
  };

  const requestResetDraft = () => {
    if (isDirty) {
      setConfirmation({
        kind: 'reset',
        title: t('Start a new router?'),
        message: t('This will discard the unsaved routing work currently in the editor. Saved routers are not affected.'),
        confirmLabel: t('Discard draft'),
        tone: 'danger',
      });
      return;
    }
    resetDraft();
  };

  const applySavedRecord = (record: ModelInfo) => {
    const recordName = modelName(record);
    if (!recordName) return;
    const baselineAtStart = draftFingerprintRef.current;
    void api.modelDetail(recordName)
      .then(detailedModel => {
        let nextDraft: RouterDraft;
        try {
          nextDraft = routerDraftFromModelInfo(detailedModel);
        } catch (loadError) {
          setError(loadError instanceof Error ? loadError.message : t('Could not load saved router.'));
          return;
        }
        const commit = () => {
          markBaseline(nextDraft);
          initialModelKeyRef.current = recordName;
          setDraft(nextDraft);
          setSelectedRuleIndex(0);
          setExpandedRuleIndex(null);
          setError(null);
          setNotice(`Loaded ${modelLabel(record)}.`);
        };
        if (draftFingerprintRef.current !== baselineAtStart) {
          requestDiscard(
            'Load another router?',
            'The router finished loading after you started editing. Loading it now will discard those changes.',
            'Load and discard changes',
            commit,
          );
          return;
        }
        commit();
      })
      .catch(loadError => {
        setError(loadError instanceof Error ? loadError.message : t('Could not load saved router.'));
      });
  };

  const loadSaved = (modelNameValue: string) => {
    if (!modelNameValue) {
      requestResetDraft();
      return;
    }
    if (modelNameValue === draft.modelName) return;
    const record = savedRecords.find(item => modelName(item) === modelNameValue);
    if (!record) return;
    if (isDirty) {
      requestDiscard(
        'Load another router?',
        'Loading a saved router will discard the unsaved changes currently in the editor.',
        'Load and discard changes',
        () => applySavedRecord(record),
      );
      return;
    }
    applySavedRecord(record);
  };

  const toggleCandidate = (name: string) => {
    const removing = draft.candidates.includes(name);
    const wasDefault = removing && draft.defaultModel === name;
    const affectedRules = removing ? draft.rules.filter(rule => rule.routeTo === name).length : 0;
    const remainingCandidates = removing ? draft.candidates.filter(candidate => candidate !== name) : draft.candidates;
    const replacement = remainingCandidates[0] || '';
    setDraft(current => toggleRouterDraftCandidate(current, name));
    setError(null);
    if (removing && (wasDefault || affectedRules > 0)) {
      const updates: string[] = [];
      if (wasDefault) {
        updates.push(replacement
          ? t('default changed to {replacement}', { replacement })
          : t('default cleared'));
      }
      if (affectedRules > 0) {
        updates.push(replacement
          ? t('{count} rule targets changed to {replacement}', { count: affectedRules, replacement })
          : t('{count} rule targets cleared', { count: affectedRules }));
      }
      setNotice(t('Removed {name}; {updates}.', { name, updates: updates.join('; ') }));
    } else {
      setNotice(null);
    }
  };

  const addClassifier = (type: RouterClassifier['type']) => {
    const classifier = createRouterClassifier(draft.classifiers.length, type);
    classifier.id = nextSafeId('classifier', draft.classifiers.map(item => item.id));
    setPatch({ classifiers: [...draft.classifiers, classifier] });
  };

  const updateClassifier = (index: number, patch: Partial<RouterClassifier>) => {
    setDraft(current => ({
      ...current,
      classifiers: current.classifiers.map((item, itemIndex) => itemIndex === index ? { ...item, ...patch } : item),
    }));
    setError(null);
    setNotice(null);
  };

  const commitClassifierId = (index: number, nextId: string): boolean => {
    const previous = draft.classifiers[index];
    if (!previous) return false;
    if (!nextId) {
      setError(t('Classifier ID cannot be empty.'));
      return false;
    }
    if (draft.classifiers.some((item, itemIndex) => itemIndex !== index && item.id === nextId)) {
      setError(`Classifier ID "${nextId}" is already in use.`);
      return false;
    }
    setDraft(current => ({
      ...current,
      classifiers: current.classifiers.map((item, itemIndex) => itemIndex === index ? { ...item, id: nextId } : item),
      rules: current.rules.map(rule => ({
        ...rule,
        condition: renameClassifierReference(rule.condition, previous.id, nextId),
      })),
    }));
    setError(null);
    setNotice(null);
    return true;
  };

  const removeClassifier = (index: number) => {
    const classifier = draft.classifiers[index];
    if (classifier && draft.rules.some(rule => routerNodeReferencesClassifier(rule.condition, classifier.id))) {
      setError(`Classifier "${classifier.id}" is still used by a rule. Change those conditions before removing it.`);
      return;
    }
    setPatch({ classifiers: draft.classifiers.filter((_, itemIndex) => itemIndex !== index) });
  };

  const commitSemanticConceptName = (classifierIndex: number, previousName: string, nextName: string): boolean => {
    const classifier = draft.classifiers[classifierIndex];
    if (!classifier || classifier.type !== 'semantic_similarity') return false;
    if (!nextName) {
      setError(t('Semantic concept name cannot be empty.'));
      return false;
    }
    if (nextName !== previousName && Object.keys(classifier.referencePhrases).some(name => name === nextName)) {
      setError(`Semantic concept "${nextName}" already exists.`);
      return false;
    }
    setDraft(current => {
      const currentClassifier = current.classifiers[classifierIndex];
      if (!currentClassifier || currentClassifier.type !== 'semantic_similarity') return current;
      const entries = Object.entries(currentClassifier.referencePhrases).map(([name, phrases]) =>
        name === previousName ? [nextName, phrases] as const : [name, phrases] as const,
      );
      const nextClassifier = {
        ...currentClassifier,
        referencePhrases: Object.fromEntries(entries),
        defaultLabel: currentClassifier.defaultLabel === previousName ? nextName : currentClassifier.defaultLabel,
      };
      return {
        ...current,
        classifiers: current.classifiers.map((item, index) => index === classifierIndex ? nextClassifier : item),
        rules: current.rules.map(rule => ({
          ...rule,
          condition: renameClassifierLabelReference(rule.condition, currentClassifier.id, previousName, nextName),
        })),
      };
    });
    setError(null);
    setNotice(null);
    return true;
  };

  const addRule = () => {
    const rule = createRouterRule(draft.rules.length, draft.defaultModel);
    rule.id = nextSafeId('rule', draft.rules.map(item => item.id));
    setPatch({ rules: [...draft.rules, rule] });
    setSelectedRuleIndex(draft.rules.length);
  };

  const moveRuleTo = (from: number, to: number) => {
    if (from === to || from < 0 || to < 0 || from >= draft.rules.length || to >= draft.rules.length) return;
    setPatch({ rules: moveItemTo(draft.rules, from, to) });
    setSelectedRuleIndex(current => {
      if (current === from) return to;
      if (from < current && to >= current) return current - 1;
      if (from > current && to <= current) return current + 1;
      return current;
    });
  };

  const removeRuleAt = (index: number) => {
    setPatch({ rules: draft.rules.filter((_, itemIndex) => itemIndex !== index) });
    setSelectedRuleIndex(current => {
      if (current > index) return current - 1;
      if (current === index) return Math.max(0, Math.min(index, draft.rules.length - 2));
      return current;
    });
  };

  const importFile = async (file: File | undefined) => {
    if (!file) return;
    try {
      const parsedPayload = JSON.parse(await file.text());
      const importedDraft = parseRouterPayload(parsedPayload);
      const applyImport = () => {
        // Imported JSON is editable but not yet registered in this session, so
        // closing it should still count as discarding unsaved work.
        baselineFingerprintRef.current = '__imported-router__';
        // Preserve the processed initial-model key for the same reason as New:
        // a background refresh of the opener must not replace this import.
        setDraft(importedDraft);
        setSelectedRuleIndex(0);
        setExpandedRuleIndex(null);
        setError(null);
        setNotice(`Imported ${file.name}. Save & register to persist it.`);
        setTab('builder');
      };
      if (isDirty) {
        requestDiscard(
          'Import router JSON?',
          'Importing this file will replace the unsaved changes currently in the editor.',
          'Import and discard changes',
          applyImport,
        );
      } else {
        applyImport();
      }
    } catch (importError) {
      setError(importError instanceof Error ? importError.message : t('Could not import router JSON.'));
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
      setConnectionsError(providerError instanceof Error ? providerError.message : t('Could not update provider endpoint.'));
    } finally {
      setSavingProvider(false);
    }
  };

  const save = async () => {
    if (saving || deleting || savingProvider) return;
    setError(null);
    setNotice(null);
    const submittedDraft = draft;
    const submittedFingerprint = routerDraftFingerprint(submittedDraft);
    let nextRequest: RouterPullRequest;
    try {
      nextRequest = buildRouterPullRequest(submittedDraft);
    } catch (buildError) {
      setError(buildError instanceof Error ? buildError.message : t('Router validation failed.'));
      return;
    }
    const dependencyPreflight = preflightRouter(nextRequest as any, models, []);
    if (!dependencyPreflight.ok) {
      setError(dependencyPreflight.errors.join(' '));
      return;
    }
    setSaving(true);
    try {
      await onRegister(nextRequest, submittedDraft.name.trim());

      const savedDraft: RouterDraft = { ...submittedDraft, modelName: nextRequest.model_name };
      markBaseline(savedDraft);
      initialModelKeyRef.current = nextRequest.model_name;

      const savedModel = routerRequestToModelInfo(nextRequest, savedDraft);

      setDraft(current => {
        // If the user edited while server registration was in flight, preserve those newer
        // edits. Only attach the now-authoritative model ID. If nothing changed,
        // commit the exact submitted snapshot as the clean baseline.
        if (routerDraftFingerprint(current) === submittedFingerprint) return savedDraft;
        return { ...current, modelName: current.modelName || nextRequest.model_name };
      });
      setNotice(`Registered ${nextRequest.model_name}.`);
onSaved?.(savedModel);
    } catch (saveError) {
      setError(saveError instanceof Error ? saveError.message : t('Could not register router.'));
    } finally {
      setSaving(false);
    }
  };

  const deleteCurrent = async () => {
    const modelNameValue = draft.modelName;
    if (!modelNameValue || deleting) return;
    if (!onDeleted) {
      setConfirmation(null);
      setError(t('Router deletion is unavailable in this context.'));
      return;
    }
    setDeleting(true);
    setError(null);
    try {
      await onDeleted(modelNameValue);
      setConfirmation(null);
      resetDraft();
      setNotice(`Deleted ${modelNameValue}.`);
    } catch (deleteError) {
      // Close the modal so the persistent editor error is immediately visible;
      // the router remains loaded and the user can retry intentionally.
      setConfirmation(null);
      setError(deleteError instanceof Error ? deleteError.message : t('Could not delete router.'));
    } finally {
      setDeleting(false);
    }
  };

  const requestDeleteCurrent = () => {
    if (!draft.modelName || saving || deleting) return;
    setConfirmation({
      kind: 'delete',
      title: t('Delete router?'),
      message: t('Delete {model}? This removes the saved router definition from Lemonade.', { model: draft.modelName })
        + (isDirty ? ` ${t('Unsaved edits in this editor will also be discarded.')}` : ''),
      confirmLabel: t('Delete router'),
      tone: 'danger',
    });
  };

  const requestClose = () => {
    if (!isDirty) {
      onClose();
      return;
    }
    requestDiscard(
      'Close router editor?',
      'Closing now will discard the unsaved changes currently in the editor.',
      'Close and discard changes',
      onClose,
    );
  };

  const dismissConfirmation = () => {
    if (deleting) return;
    if (confirmation?.kind === 'discard') pendingDiscardActionRef.current = null;
    setConfirmation(null);
  };

  const confirmPendingAction = () => {
    const pending = confirmation;
    if (!pending || deleting) return;
    if (pending.kind === 'delete') {
      // Keep the modal/focus trap mounted for the entire server mutation so the
      // underlying draft cannot be edited and deleted concurrently.
      void deleteCurrent();
      return;
    }
    setConfirmation(null);
    if (pending.kind === 'switch-llm') {
      applyRoutingMode('llm');
      return;
    }
    if (pending.kind === 'switch-rules') {
      applyRoutingMode('rules');
      return;
    }
    if (pending.kind === 'reset') {
      resetDraft();
      return;
    }
    if (pending.kind === 'discard') {
      const action = pendingDiscardActionRef.current;
      pendingDiscardActionRef.current = null;
      action?.();
      return;
    }
  };

  const selectedRule = draft.rules[selectedRuleIndex] || null;
  const expandedRule = expandedRuleIndex == null ? null : (draft.rules[expandedRuleIndex] || null);

  return (
    <WorkspaceDetailPanel
      className="router-editor"
      ariaLabel={t('Router editor')}
      leading={<Icon name="router" size={20} aria-hidden="true" />}
      title={<h2 className="workspace-detail-panel__title">{t('Router')}</h2>}
      metadata={<WorkspaceMetadataChip emphasis="high" tone="accent">collection.router</WorkspaceMetadataChip>}
      description={<p>{t('Build and register a virtual model that routes requests across compatible candidates.')}</p>}
      descriptionPlacement="identity"
      actions={(
        <WorkspaceActionGroup label={t('Router editor actions')}>
          <WorkspaceActionButton appearance="primary" icon="check" disabled={saving || deleting || savingProvider || validationErrors.length > 0} onClick={() => { void save(); }}>
            {saving ? t('Saving…') : t('Save')}
          </WorkspaceActionButton>
          {draft.modelName && (
            <WorkspaceActionButton appearance="danger" icon="trash" disabled={saving || deleting || savingProvider} onClick={requestDeleteCurrent}>{t('Delete')}</WorkspaceActionButton>
          )}
          <WorkspaceActionButton appearance="secondary" icon="x" disabled={saving || deleting || savingProvider} onClick={requestClose}>{t('Close')}</WorkspaceActionButton>
          <span className="workspace-action-group__spacer" />
          <WorkspaceActionButton appearance="quiet" icon="file" disabled={!request} onClick={() => request && downloadJson(routerDisplayName(request.model_name), request)}>{t('Export')}</WorkspaceActionButton>
          <WorkspaceActionButton appearance="quiet" icon="file-up" disabled={saving} onClick={() => importRef.current?.click()}>{t('Import')}</WorkspaceActionButton>
        </WorkspaceActionGroup>
      )}
      titleExtras={(
        <div className="router-editor__toolbar" aria-label={t('Router file actions')}>
          <div className="router-editor__toolbar-row">
            <WorkspaceActionButton size="small" icon="compose" disabled={saving || deleting || savingProvider} onClick={requestResetDraft}>{t('New')}</WorkspaceActionButton>
            <label className="router-editor__saved-select">
              <span className="sr-only">{t('Saved routers')}</span>
              <RouterSelect
                value={draft.modelName || ''}
                options={[{ value: '', label: t('Unsaved router') }, ...savedRecords.map(r => ({ value: modelName(r), label: modelLabel(r) }))]}
                onChange={(val: string) => loadSaved(val)}
                ariaLabel={t('Saved routers')}
                disabled={saving || deleting || savingProvider}
              />
            </label>
          </div>
          <input ref={importRef} className="hidden-file-input" type="file" accept="application/json,.json" onChange={event => { void importFile(event.target.files?.[0]); }} />
        </div>
      )}
    >
      <div className="router-editor__tabs" role="tablist" aria-label={t('Router editor view')}>
        <button type="button" className={tab === 'builder' ? 'is-active' : ''} role="tab" aria-selected={tab === 'builder'} onClick={() => setTab('builder')}>{t('Builder')}</button>
        <button type="button" className={tab === 'json' ? 'is-active' : ''} role="tab" aria-selected={tab === 'json'} onClick={() => setTab('json')}>{t('JSON Preview')}</button>
      </div>

      <div className="router-editor__body">
        {tab === 'json' ? (
          <section className="router-editor__json-panel">
            <div className="router-editor__section-head">
              <div><h3>{t('Registration Payload')}</h3><p>{t('Exact body sent to')} <code>/api/v1/pull</code>.</p></div>
              <WorkspaceActionButton
                className={`router-editor__copy-button${jsonCopied ? ' is-copied' : ''}`}
                size="small"
                icon={jsonCopied ? 'check' : 'copy'}
                disabled={!jsonPreview}
                onClick={() => { void copyJsonPreview(); }}
                aria-live="polite"
              >
                {jsonCopied ? t('Copied') : t('Copy')}
              </WorkspaceActionButton>
            </div>
            {jsonPreview ? <pre>{jsonPreview}</pre> : <div className="router-editor__empty">{t('Fix validation errors to generate the payload.')}</div>}
          </section>
        ) : (
          <>
            <section className="router-editor__section">
              <div className="router-editor__section-head">
                <div><h3>{t('Identity')}</h3><p>{t('Appears in your model list like any other model.')}</p></div>
              </div>
              <div className="router-editor__form-grid">
                <label><span>{t('Router Name')}</span><input className="input" value={draft.name} placeholder={t('Fast-or-smart')} onChange={event => setPatch({ name: event.target.value })} /></label>
                <label><span>{t('Model ID')}</span><input className="input" value={draft.modelName || (draft.name ? normalizeRouterModelName(draft.name) : '')} readOnly /></label>
              </div>
            </section>

            <section className="router-editor__section">
              <div className="router-editor__section-head">
                <div><h3>{t('Candidate Models')}</h3><p>{t('Traffic is distributed only among these models.')}</p></div>
                <span className="router-editor__count">{draft.candidates.length} selected</span>
              </div>
              <div className="router-editor__candidate-search"><Icon name="search" size={14} /><input value={candidateSearch} placeholder={t('Search registered models')} onChange={event => setCandidateSearch(event.target.value)} /></div>
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
                        {connection.kind === 'external' ? `External · ${connection.provider || 'provider'}` : t('Internal')}
                      </span>
                    </label>
                  );
                })}
                {filteredCandidateModels.length === 0 && <div className="router-editor__empty">{t('No compatible models match this search.')}</div>}
              </div>
              <label className="router-editor__default-model">
                <span>{t('Default Model')} <small>{t('Used when no rule matches or evaluation fails.')}</small></span>
                <RouterSelect
                  value={draft.defaultModel}
                  options={[{ value: '', label: t('Select default') }, ...draft.candidates.map(c => ({ value: c, label: c }))]}
                  onChange={(val: string) => setPatch({ defaultModel: val })}
                  ariaLabel={t('Default model')}
                />
              </label>

              <div className="router-editor__connections" aria-label={t('Connected model topology')}>
                <div className="router-editor__mini-head">
                  <span>{t('Connected Model Topology')}</span>
                </div>
                {selectedConnections.length === 0 ? (
                  <div className="router-editor__empty">{t('Select candidate models to review their connections.')}</div>
                ) : (
                  <div className="router-editor__connection-list">
                    {selectedConnections.map(connection => (
                      <div className={`router-editor__connection router-editor__connection--${connection.kind}`} key={connection.modelName}>
                        <div className="router-editor__connection-main">
                          <div>
                            <strong>{connection.displayName}</strong>
                            {connection.modelName === draft.defaultModel && <span className="router-editor__default-badge">{t('Default')}</span>}
                          </div>
                          <div className="router-editor__connection-source">
                            <span className={`router-editor__source-badge router-editor__source-badge--${connection.kind}`}>
                              {connection.kind === 'external' ? t('External') : connection.kind === 'internal' ? t('Internal') : t('Unresolved')}
                            </span>
                            <small>{connection.kind === 'external' ? (connection.provider || 'Unknown provider') : (connection.backend || connection.recipe || 'Unknown source')}</small>
                          </div>
                        </div>
                        {connection.kind === 'external' && (
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
                                    <span>{t('Allow insecure HTTP')}</span>
                                  </label>
                                )}
                                <WorkspaceActionButton size="small" appearance="primary" disabled={savingProvider} onClick={() => { void saveProviderEndpoint(); }}>
                                  {savingProvider ? t('Saving…') : t('Save')}
                                </WorkspaceActionButton>
                                <WorkspaceActionButton size="small" onClick={() => { setEditingProvider(null); setEditingConnectionModel(null); setProviderAllowInsecureDraft(false); setConnectionsError(null); }}>{t('Cancel')}</WorkspaceActionButton>
                              </div>
                            ) : (
                              <>
                                <span title={connection.endpoint || 'Endpoint unavailable'}>{connection.endpoint || 'Endpoint not configured'}</span>
                                <small>{connection.authConfigured ? t('Authentication configured') : t('Authentication required')}</small>
                                {connection.provider && (
                                  <WorkspaceActionButton size="small" icon="edit" onClick={() => startEditingProvider(connection.provider, connection.endpoint, connection.modelName, connection.allowInsecureHttp)}>
                                    {t('Edit Endpoint')}
                                  </WorkspaceActionButton>
                                )}
                              </>
                            )}
                          </div>
                        )}
                        <div className="router-editor__connection-trailing">
                          {draft.candidates.includes(connection.modelName) && (
                            <WorkspaceActionButton
                              appearance="danger"
                              size="small"
                              icon="trash"
                              iconOnly
                              onClick={() => toggleCandidate(connection.modelName)}
                              aria-label={`Remove ${connection.displayName} from candidate models`}
                              title={t('Remove candidate')}
                            />
                          )}
                        </div>
                      </div>
                    ))}
                  </div>
                )}
                {connectionsError && selectedConnections.some(connection => connection.kind === 'external') && <div className="router-editor__message router-editor__message--error"><Icon name="alert" size={14} /> {connectionsError}</div>}
              </div>
            </section>

            <section className="router-editor__section">
              <div className="router-editor__section-head">
                <div><h3>{t('Routing Strategy')}</h3><p>{t('Pick the mechanism that decides which model handles each request.')}</p></div>
              </div>
              <div className="router-editor__strategy" role="radiogroup" aria-label={t('Routing strategy')}>
                <button
                  type="button"
                  className={`router-editor__strategy-option ${draft.mode === 'rules' ? 'is-active' : ''}`}
                  role="radio"
                  aria-checked={draft.mode === 'rules'}
                  onClick={() => setRoutingMode('rules')}
                >
                  <Icon name="layers" size={18} />
                  <span><strong>{t('Ordered Rules')}</strong><small>{t('Pattern-based rules with optional classifier signals - first match wins.')}</small></span>
                </button>
                <button
                  type="button"
                  className={`router-editor__strategy-option ${draft.mode === 'llm' ? 'is-active' : ''}`}
                  role="radio"
                  aria-checked={draft.mode === 'llm'}
                  onClick={() => setRoutingMode('llm')}
                >
                  <Icon name="brain-circuit" size={18} />
                  <span><strong>{t('Natural-Language Router')}</strong><small>{t('An LLM model reads your instruction and picks the right candidate for each request.')}</small></span>
                </button>
              </div>
            </section>

            {draft.mode === 'llm' ? (
              <section className="router-editor__section" aria-label={t('Natural-Language Router settings')}>
                <div className="router-editor__form-grid">
                  <div className="router-editor__wide router-editor__field">
                    <span>{t('Routing Model')} <small>{t('Usually a small, fast chat model.')}</small></span>
                    <RouterModelPicker
                      models={candidateModels}
                      value={draft.llmRouter.model}
                      onChange={model => setPatch({ llmRouter: { ...draft.llmRouter, model } })}
                      placeholder={t('Select routing model')}
                      searchPlaceholder={t('Search routing models')}
                      ariaLabel={t('Natural-language routing model')}
                    />
                  </div>
                  <label className="router-editor__wide">
                    <span>{t('Routing Instruction')} <small>{t('Describe clearly when each candidate should be selected.')}</small></span>
                    <textarea
                      className="textarea router-editor__prompt"
                      value={draft.llmRouter.prompt}
                      placeholder={t('Use the fast model for everyday questions. Use the larger model for difficult reasoning, coding, or long context.')}
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
                <div><h3>{t('Classifiers')}</h3><p>{t('Model-scored signals you can reference inside your rules.')}</p></div>
                <div className="router-editor__section-actions">
                  <WorkspaceActionButton size="small" icon="plus" onClick={() => addClassifier('classifier')}>{t('Classifier')}</WorkspaceActionButton>
                  <WorkspaceActionButton size="small" icon="plus" onClick={() => addClassifier('semantic_similarity')}>{t('Semantic')}</WorkspaceActionButton>
                  <WorkspaceActionButton size="small" icon="plus" onClick={() => addClassifier('llm')}>{t('LLM signal')}</WorkspaceActionButton>
                </div>
              </div>
              {draft.classifiers.length === 0 ? <div className="router-editor__empty">{t('No classifiers. Deterministic rules need none.')}</div> : (
                <div className="router-editor__classifier-list">
                  {draft.classifiers.map((classifier, index) => {
                    const labels = classifierLabels(classifier);
                    return (
                      <article className="router-editor__classifier" key={index}>
                        <div className="router-editor__card-head">
                          <strong>{classifier.type === 'semantic_similarity' ? t('Semantic Similarity') : classifier.type === 'llm' ? t('LLM Classifier') : t('Text Classifier')}</strong>
                          <WorkspaceActionButton appearance="danger" size="small" icon="trash" iconOnly onClick={() => removeClassifier(index)} aria-label={t('Remove classifier')} title={t('Remove classifier')} />
                        </div>
                        <div className="router-editor__form-grid router-editor__form-grid--classifier">
                          <label><span>ID</span><CommittedTextInput value={classifier.id} ariaLabel={`Classifier ${index + 1} ID`} normalize={input => input} onCommit={nextId => commitClassifierId(index, nextId)} /></label>
                          <label><span>{t('Type')}</span><RouterSelect value={classifier.type} options={[{ value: 'classifier', label: 'classifier' }, { value: 'semantic_similarity', label: 'semantic_similarity' }, { value: 'llm', label: 'llm' }]} onChange={(val: string) => updateClassifier(index, { ...createRouterClassifier(index, val as RouterClassifier['type']), id: classifier.id })} ariaLabel={t('Classifier type')} /></label>
                          <div className="router-editor__wide router-editor__field">
                            <span>{t('Model')}</span>
                            <RouterModelPicker
                              models={classifier.type === 'semantic_similarity' ? embeddingModels : classifier.type === 'llm' ? candidateModels : classifierModels}
                              value={classifier.model}
                              onChange={model => updateClassifier(index, { model })}
                              placeholder={t('Select model')}
                              searchPlaceholder={classifier.type === 'semantic_similarity' ? t('Search embedding models') : classifier.type === 'llm' ? t('Search chat models') : t('Search classification models')}
                              ariaLabel={`${classifier.id || `Classifier ${index + 1}`} model`}
                            />
                          </div>
                          {classifier.type === 'semantic_similarity' ? (
                            <div className="router-editor__wide router-editor__concepts">
                              <div className="router-editor__mini-head"><span>{t('Concepts and Reference Phrases')}</span><WorkspaceActionButton size="small" icon="plus" onClick={() => updateClassifier(index, { referencePhrases: { ...classifier.referencePhrases, [nextConceptName(classifier.referencePhrases)]: ['example phrase'] } })}>{t('Concept')}</WorkspaceActionButton></div>
                              <div className="router-editor__concept-list">
                                {Object.entries(classifier.referencePhrases).map(([concept, phrases], conceptIndex) => (
                                  <div className="router-editor__concept" key={conceptIndex}>
                                    <CommittedTextInput value={concept} ariaLabel={t('Concept name')} onCommit={nextName => commitSemanticConceptName(index, concept, nextName)} />
                                    <textarea className="textarea" rows={3} value={phrases.join('\n')} aria-label={t('Reference phrases')} placeholder={t('One reference phrase per line')} onChange={event => updateClassifier(index, { referencePhrases: { ...classifier.referencePhrases, [concept]: event.target.value.split(/\r?\n/) } })} />
                                    <WorkspaceActionButton appearance="danger" size="small" icon="trash" iconOnly title={t('Remove concept')} aria-label={t('Remove concept')} onClick={() => {
                                      const next = { ...classifier.referencePhrases };
                                      delete next[concept];
                                      updateClassifier(index, {
                                        referencePhrases: next,
                                        defaultLabel: classifier.defaultLabel === concept ? undefined : classifier.defaultLabel,
                                      });
                                    }} />
                                  </div>
                                ))}
                              </div>
                            </div>
                          ) : (
                            <>
                              {classifier.type === 'llm' && (
                                <label className="router-editor__wide">
                                  <span>{t('Classification Prompt')} <small>{t('Explain when each label applies.')}</small></span>
                                  <textarea className="textarea router-editor__prompt" value={classifier.prompt} placeholder={t('Choose SAFE for routine requests and RISKY for requests that could cause external side effects.')} spellCheck={false} onChange={event => updateClassifier(index, { prompt: event.target.value })} />
                                </label>
                              )}
                              <label className="router-editor__wide"><span>{t('Output Labels')} <small>{t('one per line')}</small></span><textarea
                                className="textarea"
                                rows={3}
                                value={classifier.labels.join('\n')}
                                onChange={event => {
                                  const labels = event.target.value.split(/\r?\n/);
                                  const normalized = labels.map(label => label.trim()).filter(Boolean);
                                  updateClassifier(index, {
                                    labels,
                                    defaultLabel: classifier.defaultLabel && normalized.includes(classifier.defaultLabel)
                                      ? classifier.defaultLabel
                                      : undefined,
                                  });
                                }}
                              /></label>
                            </>
                          )}
                          <label><span>{t('Default Label')}</span><RouterSelect value={classifier.defaultLabel || ''} options={[{ value: '', label: t('None') }, ...labels.map(label => ({ value: label, label }))]} onChange={(val: string) => updateClassifier(index, { defaultLabel: val || undefined })} ariaLabel={t('Default label')} /></label>
                          <label><span>{t('On Error')}</span><RouterSelect value={classifier.onError} options={[{ value: 'match_false', label: t('Do not match') }, { value: 'match_true', label: t('Match rule') }]} onChange={(val: string) => updateClassifier(index, { onError: val as RouterClassifier['onError'] })} ariaLabel={t('On error')} /></label>
                        </div>
                      </article>
                    );
                  })}
                </div>
              )}
            </section>

            <section className="router-editor__section">
              <div className="router-editor__section-head">
                <div><h3>{t('Ordered Rules')}</h3><p>{t('Evaluated top to bottom - the first match wins, everything else falls back to the default.')}</p></div>
                <WorkspaceActionButton size="small" icon="plus" onClick={addRule}>{t('Rule')}</WorkspaceActionButton>
              </div>
              <div className="router-editor__rules-workspace">
                <div className="router-editor__rule-list" aria-label={t('Ordered routing rules')}>
                  {draft.rules.map((rule, index) => (
                    <div
                      className={`router-editor__rule-summary ${selectedRuleIndex === index ? 'is-selected' : ''} ${dragRuleIndex === index ? 'is-dragging' : ''}`}
                      key={index}
                      onDragOver={event => {
                        if (dragRuleIndex == null || dragRuleIndex === index) return;
                        event.preventDefault();
                        event.dataTransfer.dropEffect = 'move';
                      }}
                      onDrop={event => {
                        if (dragRuleIndex == null || dragRuleIndex === index) return;
                        event.preventDefault();
                        moveRuleTo(dragRuleIndex, index);
                        setDragRuleIndex(null);
                      }}
                    >
                      <span
                        className="router-editor__drag-handle"
                        draggable
                        aria-hidden="true"
                        title={t('Drag to reorder rule')}
                        onDragStart={event => {
                          setDragRuleIndex(index);
                          event.dataTransfer.effectAllowed = 'move';
                          event.dataTransfer.setData('text/plain', rule.id || String(index));
                        }}
                        onDragEnd={() => setDragRuleIndex(null)}
                      >
                        ⠿
                      </span>
                      <button
                        type="button"
                        className="router-editor__rule-summary-main"
                        onClick={() => setSelectedRuleIndex(index)}
                        aria-current={selectedRuleIndex === index ? 'true' : undefined}
                      >
                        <span className="router-editor__rule-order">{index + 1}</span>
                        <span className="router-editor__rule-summary-copy">
                          <strong>{rule.id || `Rule ${index + 1}`}</strong>
                          <small>{rule.routeTo ? `→ ${rule.routeTo}` : t('No route selected')}</small>
                        </span>
                      </button>
                      <div className="router-editor__rule-actions">
                        <div className="router-editor__rule-stepper">
                          <button className="router-editor__rule-stepper-button router-editor__rule-stepper-button--up" type="button" disabled={index === 0} onClick={() => moveRuleTo(index, index - 1)} title={t('Move rule up')}><Icon name="chevron-up" size={10} /></button>
                          <button className="router-editor__rule-stepper-button router-editor__rule-stepper-button--down" type="button" disabled={index === draft.rules.length - 1} onClick={() => moveRuleTo(index, index + 1)} title={t('Move rule down')}><Icon name="chevron-down" size={10} /></button>
                        </div>
                        <button className="router-editor__rule-remove" type="button" onClick={() => removeRuleAt(index)} title={t('Remove rule')}><Icon name="trash" size={14} /></button>
                      </div>
                    </div>
                  ))}
                  {draft.rules.length === 0 && <div className="router-editor__empty">{t('At least one rule is required.')}</div>}
                  {draft.defaultModel && (
                    <div className="router-editor__default-rule">
                      <span className="router-editor__rule-order">↩</span>
                      <span><strong>{t('Default')}</strong><small>→ {draft.defaultModel}</small></span>
                    </div>
                  )}
                </div>

                <div className="router-editor__rule-builder">
                  {selectedRule ? (
                    <>
                      <div className="router-editor__rule-builder-head">
                        <span className="router-editor__rule-builder-label">Rule {selectedRuleIndex + 1}:</span>
                        <input
                          className="input router-editor__rule-id-input"
                          value={selectedRule.id}
                          aria-label={t('Rule ID')}
                          onChange={event => setPatch({ rules: draft.rules.map((item, itemIndex) => itemIndex === selectedRuleIndex ? { ...item, id: event.target.value } : item) })}
                        />
                        <RouterSelect
                          className="router-editor__rule-route-select"
                          value={selectedRule.routeTo}
                          options={[{ value: '', label: t('Route to…') }, ...draft.candidates.map(c => ({ value: c, label: c }))]}
                          onChange={(val: string) => setPatch({ rules: draft.rules.map((item, itemIndex) => itemIndex === selectedRuleIndex ? { ...item, routeTo: val } : item) })}
                          ariaLabel={t('Route To')}
                        />
                      </div>
                      <RouterRuleGraph
                        key={`rule-${selectedRuleIndex}`}
                        node={selectedRule.condition}
                        classifiers={draft.classifiers}
                        onChange={condition => {
                          committedRuleIndicesRef.current.add(selectedRuleIndex);
                          setPatch({ rules: draft.rules.map((item, itemIndex) => itemIndex === selectedRuleIndex ? { ...item, condition } : item) });
                        }}
                        onExpand={() => setExpandedRuleIndex(selectedRuleIndex)}
                      />
                      <details className="router-editor__outputs">
                        <summary>{t('Optional Decision Outputs')}</summary>
                        <textarea className="textarea" value={selectedRule.outputsText || ''} placeholder={'{\n  "tier": "fast"\n}'} spellCheck={false} onChange={event => setPatch({ rules: draft.rules.map((item, itemIndex) => itemIndex === selectedRuleIndex ? { ...item, outputsText: event.target.value } : item) })} />
                      </details>
                    </>
                  ) : (
                    <div className="router-editor__rule-builder-empty">
                      <Icon name="router" size={18} />
                      <strong>{t('Select or add a rule')}</strong>
                      <span>{t('The graph editor will appear here.')}</span>
                    </div>
                  )}
                </div>
              </div>
            </section>
              </>
            )}
          </>
        )}

        {validationErrors.length > 0 && (
          <section className="router-editor__validation" aria-live="polite">
            <strong><Icon name="alert" size={14} /> {validationErrors.length === 1
              ? t('{count} validation issue', { count: validationErrors.length })
              : t('{count} validation issues', { count: validationErrors.length })}</strong>
            <ul>{validationErrors.slice(0, 8).map((message, index) => <li key={`${message}-${index}`}>{message}</li>)}</ul>
          </section>
        )}
        {error && <div className="router-editor__message router-editor__message--error"><Icon name="alert" size={14} /> {error}</div>}
      </div>

      {notice && (
        <div className="router-editor__toast" role="status" aria-live="polite">
          <Icon name="check" size={15} />
          <span>{notice}</span>
          <button type="button" onClick={() => setNotice(null)} aria-label={t('Dismiss notification')}><Icon name="x" size={12} /></button>
        </div>
      )}

      <Modal
        isOpen={expandedRule != null && expandedRuleIndex != null}
        onClose={() => setExpandedRuleIndex(null)}
        title={expandedRule && expandedRuleIndex != null ? `Rule ${expandedRuleIndex + 1}: ${expandedRule.id || 'Untitled Rule'}` : t('Graph Builder')}
        maxWidth="calc(100vw - 48px)"
        className="inspect-modal-content--full-height"
        ariaLabelledBy="router-graph-expanded-title"
      >
        {expandedRule && expandedRuleIndex != null && (
          <div className="inspect-modal-body router-editor__graph-modal">
            <RouterRuleGraph
              key={`expanded-${expandedRuleIndex}`}
              node={expandedRule.condition}
              classifiers={draft.classifiers}
              expanded
              initialCommitted={committedRuleIndicesRef.current.has(expandedRuleIndex)}
              onChange={condition => setPatch({
                rules: draft.rules.map((item, itemIndex) => itemIndex === expandedRuleIndex ? { ...item, condition } : item),
              })}
            />
          </div>
        )}
      </Modal>

      <Modal
        isOpen={confirmation != null}
        onClose={dismissConfirmation}
        title={confirmation?.title || 'Confirm action'}
        maxWidth="480px"
        ariaLabelledBy="router-confirm-dialog-title"
      >
        <div className="inspect-modal-body router-confirm-dialog__body">
          <div className="router-confirm-dialog__icon"><Icon name="alert" size={18} /></div>
          <p>{confirmation?.message}</p>
        </div>
        <div className="inspect-modal-footer">
          <WorkspaceActionButton appearance="secondary" disabled={deleting} onClick={dismissConfirmation}>{t('Close')}</WorkspaceActionButton>
          <WorkspaceActionButton appearance={confirmation?.tone || 'primary'} disabled={deleting} onClick={confirmPendingAction}>
            {deleting && confirmation?.kind === 'delete' ? t('Deleting…') : (confirmation?.confirmLabel || 'Continue')}
          </WorkspaceActionButton>
        </div>
      </Modal>

    </WorkspaceDetailPanel>
  );
};

export default RouterEditorPanel;
