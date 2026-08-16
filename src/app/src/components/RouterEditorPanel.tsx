import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import api, { type CloudProviderRow, type ModelInfo } from '../api';
import { capabilityFromModelInfo, isRouterRecipe } from '../modelCapabilities';
import { Icon } from './Icon';
import { useI18n } from '../i18n';
import type { TranslationParams } from '../i18n/types';
import { copyTextToClipboard } from '../clipboard';
import { localizeProviderEndpointError, localizeRouterImportError, localizeRouterValidationMessage } from '../features/router/routerPresentation';
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
  await copyTextToClipboard(text);
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
  onRegister: (request: RouterPullRequest) => Promise<void>;
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

type RouterNotice =
  | { kind: 'translated'; key: string; params?: TranslationParams }
  | { kind: 'raw'; text: string }
  | {
      kind: 'candidate-repairs';
      name: string;
      replacement: string;
      wasDefault: boolean;
      affectedRules: number;
    };

function routerNoticeText(
  notice: RouterNotice,
  translate: (key: string, params?: TranslationParams) => string,
): string {
  if (notice.kind === 'raw') return notice.text;
  if (notice.kind === 'translated') return translate(notice.key, notice.params);
  const updates: string[] = [];
  if (notice.wasDefault) {
    updates.push(notice.replacement
      ? translate('editor.candidates.repairDefaultChanged', { model: notice.replacement })
      : translate('editor.candidates.repairDefaultCleared'));
  }
  if (notice.affectedRules > 0) {
    updates.push(notice.replacement
      ? translate('editor.candidates.repairRulesChanged', { count: notice.affectedRules, model: notice.replacement })
      : translate('editor.candidates.repairRulesCleared', { count: notice.affectedRules }));
  }
  return translate('editor.candidates.removedWithRepairs', { name: notice.name, updates: updates.join('; ') });
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
  const { t } = useI18n('router');
  const routerTranslateRef = useRef(t);
  routerTranslateRef.current = t;
  const [draft, setDraft] = useState<RouterDraft>(() => createEmptyRouterDraft());
  const [savedRecords, setSavedRecords] = useState(() => loadRouterRecords());
  const [candidateSearch, setCandidateSearch] = useState('');
  const [tab, setTab] = useState<'builder' | 'json'>('builder');
  const [saving, setSaving] = useState(false);
  const [deleting, setDeleting] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [notice, setNotice] = useState<RouterNotice | null>(null);
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

  const refreshSaved = () => setSavedRecords(loadRouterRecords());
  const refreshCloudProviders = useCallback(async () => {
    try {
      setCloudProviders(await api.cloudProviders());
      setConnectionsError(null);
    } catch (providerError) {
      setConnectionsError(providerError instanceof Error ? providerError.message : routerTranslateRef.current('editor.errors.providers'));
    }
  }, []);

  useEffect(() => {
    setSavedRecords(loadRouterRecords());
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
        setError(initialError instanceof Error ? localizeRouterImportError(initialError.message, routerTranslateRef.current) : routerTranslateRef.current('editor.errors.open'));
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
          setError(detailParseError instanceof Error ? localizeRouterImportError(detailParseError.message, routerTranslateRef.current) : routerTranslateRef.current('editor.errors.open'));
          return;
        }
        if (draftFingerprintRef.current === baselineAtDetailStart) {
          applyLoadedDraft(detailedDraft);
          return;
        }
        requestDiscard(
          routerTranslateRef.current('editor.confirm.loadPolicy.title'),
          routerTranslateRef.current('editor.confirm.loadPolicy.message'),
          routerTranslateRef.current('editor.confirm.loadPolicy.confirm'),
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
            setError(fallbackError instanceof Error ? localizeRouterImportError(fallbackError.message, routerTranslateRef.current) : routerTranslateRef.current('editor.errors.loadPolicy'));
            return;
          }
          if (draftFingerprintRef.current === baselineAtDetailStart) {
            applyLoadedDraft(fallbackDraft);
          } else {
            requestDiscard(
              routerTranslateRef.current('editor.confirm.loadCached.title'),
              routerTranslateRef.current('editor.confirm.loadCached.message'),
              routerTranslateRef.current('editor.confirm.loadCached.confirm'),
              () => applyLoadedDraft(fallbackDraft),
            );
          }
          return;
        }
        initialModelKeyRef.current = null;
        setError(detailError instanceof Error ? detailError.message : routerTranslateRef.current('editor.errors.loadPolicy'));
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

  const validationErrors = useMemo(() => validateRouterDraft(draft).map(message => localizeRouterValidationMessage(message, t)), [draft, t]);
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
      setError(copyError instanceof Error ? copyError.message : routerTranslateRef.current('editor.errors.copyJson'));
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
        title: t('editor.confirm.switch.title'),
        message: t('editor.confirm.switch.toNatural'),
        confirmLabel: t('editor.confirm.switch.clearRules'),
        tone: 'danger',
      });
      return;
    }
    if (draft.mode === 'llm' && routerDraftHasLlmProgress(draft)) {
      setConfirmation({
        kind: 'switch-rules',
        title: t('editor.confirm.switch.title'),
        message: t('editor.confirm.switch.toRules'),
        confirmLabel: t('editor.confirm.switch.clearNatural'),
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
        title: t('editor.confirm.new.title'),
        message: t('editor.confirm.new.message'),
        confirmLabel: t('editor.confirm.new.confirm'),
        tone: 'danger',
      });
      return;
    }
    resetDraft();
  };

  const applySavedRecord = (record: (typeof savedRecords)[number]) => {
    try {
      const nextDraft = routerRecordToDraft(record);
      markBaseline(nextDraft);
      // Loading a saved record is a user action and does not mean the parent
      // `initialModel` prop changed. Preserve the processed prop key so an
      // unrelated refresh of the opener cannot re-apply itself.
      setDraft(nextDraft);
      setSelectedRuleIndex(0);
      setExpandedRuleIndex(null);
      setError(null);
      setNotice({ kind: 'translated', key: 'editor.notices.loaded', params: { name: record.display_name } });
    } catch (loadError) {
      setError(loadError instanceof Error ? loadError.message : t('editor.errors.loadSaved'));
    }
  };

  const loadSaved = (modelNameValue: string) => {
    if (!modelNameValue) {
      requestResetDraft();
      return;
    }
    if (modelNameValue === draft.modelName) return;
    const record = savedRecords.find(item => item.model_name === modelNameValue);
    if (!record) return;
    if (isDirty) {
      requestDiscard(
        t('editor.confirm.loadAnother.title'),
        t('editor.confirm.loadAnother.message'),
        routerTranslateRef.current('editor.confirm.loadPolicy.confirm'),
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
      setNotice({ kind: 'candidate-repairs', name, replacement, wasDefault, affectedRules });
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
      setError(t('editor.errors.classifierIdEmpty'));
      return false;
    }
    if (draft.classifiers.some((item, itemIndex) => itemIndex !== index && item.id === nextId)) {
      setError(t('editor.errors.classifierIdUsed', { id: nextId }));
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
      setError(t('editor.errors.classifierInUse', { id: classifier.id }));
      return;
    }
    setPatch({ classifiers: draft.classifiers.filter((_, itemIndex) => itemIndex !== index) });
  };

  const commitSemanticConceptName = (classifierIndex: number, previousName: string, nextName: string): boolean => {
    const classifier = draft.classifiers[classifierIndex];
    if (!classifier || classifier.type !== 'semantic_similarity') return false;
    if (!nextName) {
      setError(t('editor.errors.semanticNameEmpty'));
      return false;
    }
    if (nextName !== previousName && Object.keys(classifier.referencePhrases).some(name => name === nextName)) {
      setError(t('editor.errors.semanticExists', { name: nextName }));
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
        setNotice({ kind: 'translated', key: 'editor.notices.imported', params: { file: file.name } });
        setTab('builder');
      };
      if (isDirty) {
        requestDiscard(
          t('editor.confirm.import.title'),
          t('editor.confirm.import.message'),
          t('editor.confirm.import.confirm'),
          applyImport,
        );
      } else {
        applyImport();
      }
    } catch (importError) {
      setError(importError instanceof Error ? importError.message : t('editor.errors.import'));
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
      setConnectionsError(localizeProviderEndpointError(endpointError, t));
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
      setNotice({ kind: 'translated', key: 'editor.notices.providerUpdated', params: { provider: editingProvider } });
    } catch (providerError) {
      setConnectionsError(providerError instanceof Error ? providerError.message : t('editor.errors.updateProvider'));
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
      setError(buildError instanceof Error ? buildError.message : t('editor.errors.validation'));
      return;
    }
    setSaving(true);
    try {
      await onRegister(nextRequest);

      const savedDraft: RouterDraft = { ...submittedDraft, modelName: nextRequest.model_name };
      markBaseline(savedDraft);
      initialModelKeyRef.current = nextRequest.model_name;

      let savedModel = routerRequestToModelInfo(nextRequest, savedDraft);
      let cacheWarning = '';
      try {
        const record = upsertRouterRecord(savedDraft);
        refreshSaved();
        savedModel = routerRecordToModelInfo(record);
      } catch (cacheError) {
        // The server registration already succeeded. A local cache failure must
        // not be reported as a failed registration or cause a duplicate retry.
        cacheWarning = cacheError instanceof Error ? cacheError.message : String(cacheError);
      }

      setDraft(current => {
        // If the user edited while /pull was in flight, preserve those newer
        // edits. Only attach the now-authoritative model ID. If nothing changed,
        // commit the exact submitted snapshot as the clean baseline.
        if (routerDraftFingerprint(current) === submittedFingerprint) return savedDraft;
        return { ...current, modelName: current.modelName || nextRequest.model_name };
      });
      setNotice({
        kind: 'translated',
        key: cacheWarning ? 'editor.notices.registeredCacheWarning' : 'editor.notices.registered',
        params: { name: nextRequest.model_name },
      });
      onSaved?.(savedModel);
    } catch (saveError) {
      setError(saveError instanceof Error ? saveError.message : t('editor.errors.register'));
    } finally {
      setSaving(false);
    }
  };

  const deleteCurrent = async () => {
    const modelNameValue = draft.modelName;
    if (!modelNameValue || deleting) return;
    if (!onDeleted) {
      setConfirmation(null);
      setError(routerTranslateRef.current('editor.errors.deleteUnavailable'));
      return;
    }
    setDeleting(true);
    setError(null);
    try {
      const deletionWarning = await onDeleted?.(modelNameValue);
      if (typeof deletionWarning === 'string' && deletionWarning) {
        // The server delete succeeded but persistent cache cleanup did not. Do
        // not immediately re-read that stale cache and resurrect the deleted
        // router in this editor session.
        setSavedRecords(current => current.filter(record => record.model_name !== modelNameValue));
      } else {
        refreshSaved();
      }
      setConfirmation(null);
      resetDraft();
      setNotice(typeof deletionWarning === 'string' && deletionWarning
        ? { kind: 'raw', text: deletionWarning }
        : { kind: 'translated', key: 'editor.notices.deleted', params: { name: modelNameValue } });
    } catch (deleteError) {
      // Close the modal so the persistent editor error is immediately visible;
      // the router remains loaded and the user can retry intentionally.
      setConfirmation(null);
      setError(deleteError instanceof Error ? deleteError.message : routerTranslateRef.current('editor.errors.delete'));
    } finally {
      setDeleting(false);
    }
  };

  const requestDeleteCurrent = () => {
    if (!draft.modelName || saving || deleting) return;
    setConfirmation({
      kind: 'delete',
      title: t('editor.confirm.delete.title'),
      message: t(isDirty ? 'editor.confirm.delete.messageDirty' : 'editor.confirm.delete.message', { name: draft.modelName }),
      confirmLabel: t('editor.confirm.delete.confirm'),
      tone: 'danger',
    });
  };

  const requestClose = () => {
    if (!isDirty) {
      onClose();
      return;
    }
    requestDiscard(
      t('editor.confirm.close.title'),
      t('editor.confirm.close.message'),
      t('editor.confirm.close.confirm'),
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
      ariaLabel={t('editor.aria')}
      leading={<Icon name="router" size={20} aria-hidden="true" />}
      title={<h2 className="workspace-detail-panel__title">{t('editor.title')}</h2>}
      metadata={<WorkspaceMetadataChip emphasis="high" tone="accent">collection.router</WorkspaceMetadataChip>}
      description={<p>{t('editor.description')}</p>}
      descriptionPlacement="identity"
      actions={(
        <WorkspaceActionGroup label={t('editor.actions')}>
          <WorkspaceActionButton appearance="primary" icon="check" disabled={saving || deleting || savingProvider || validationErrors.length > 0} onClick={() => { void save(); }}>
            {saving ? t('editor.saving') : t('editor.save')}
          </WorkspaceActionButton>
          {draft.modelName && (
            <WorkspaceActionButton appearance="danger" icon="trash" disabled={saving || deleting || savingProvider} onClick={requestDeleteCurrent}>{t('editor.delete')}</WorkspaceActionButton>
          )}
          <WorkspaceActionButton appearance="secondary" icon="x" disabled={saving || deleting || savingProvider} onClick={requestClose}>{t('editor.close')}</WorkspaceActionButton>
          <span className="workspace-action-group__spacer" />
          <WorkspaceActionButton appearance="quiet" icon="file" disabled={!request} onClick={() => request && downloadJson(routerDisplayName(request.model_name), request)}>{t('editor.export')}</WorkspaceActionButton>
          <WorkspaceActionButton appearance="quiet" icon="file-up" disabled={saving} onClick={() => importRef.current?.click()}>{t('editor.import')}</WorkspaceActionButton>
        </WorkspaceActionGroup>
      )}
      titleExtras={(
        <div className="router-editor__toolbar" aria-label={t('editor.fileActions')}>
          <div className="router-editor__toolbar-row">
            <WorkspaceActionButton size="small" icon="compose" disabled={saving || deleting || savingProvider} onClick={requestResetDraft}>{t('editor.new')}</WorkspaceActionButton>
            <label className="router-editor__saved-select">
              <span className="sr-only">{t('editor.savedRouters')}</span>
              <RouterSelect
                value={draft.modelName || ''}
                options={[{ value: '', label: t('editor.unsavedRouter') }, ...savedRecords.map(r => ({ value: r.model_name, label: r.display_name }))]}
                onChange={(val: string) => loadSaved(val)}
                ariaLabel={t('editor.savedRouters')}
                disabled={saving || deleting || savingProvider}
              />
            </label>
          </div>
          <input ref={importRef} className="hidden-file-input" type="file" accept="application/json,.json" onChange={event => { void importFile(event.target.files?.[0]); }} />
        </div>
      )}
    >
      <div className="router-editor__tabs" role="tablist" aria-label={t('editor.viewAria')}>
        <button type="button" className={tab === 'builder' ? 'is-active' : ''} role="tab" aria-selected={tab === 'builder'} onClick={() => setTab('builder')}>{t('editor.builder')}</button>
        <button type="button" className={tab === 'json' ? 'is-active' : ''} role="tab" aria-selected={tab === 'json'} onClick={() => setTab('json')}>{t('editor.jsonPreview')}</button>
      </div>

      <div className="router-editor__body">
        {tab === 'json' ? (
          <section className="router-editor__json-panel">
            <div className="router-editor__section-head">
              <div><h3>{t('editor.registrationPayload')}</h3><p>{t('editor.registrationDescription')} <code>/api/v1/pull</code>.</p></div>
              <WorkspaceActionButton
                className={`router-editor__copy-button${jsonCopied ? ' is-copied' : ''}`}
                size="small"
                icon={jsonCopied ? 'check' : 'copy'}
                disabled={!jsonPreview}
                onClick={() => { void copyJsonPreview(); }}
                aria-live="polite"
              >
                {jsonCopied ? t('editor.copied') : t('editor.copy')}
              </WorkspaceActionButton>
            </div>
            {jsonPreview ? <pre>{jsonPreview}</pre> : <div className="router-editor__empty">{t('editor.fixValidation')}</div>}
          </section>
        ) : (
          <>
            <section className="router-editor__section">
              <div className="router-editor__section-head">
                <div><h3>{t('editor.identity')}</h3><p>{t('editor.identityDescription')}</p></div>
              </div>
              <div className="router-editor__form-grid">
                <label><span>{t('editor.routerName')}</span><input className="input" value={draft.name} placeholder="Fast-or-smart" onChange={event => setPatch({ name: event.target.value })} /></label>
                <label><span>{t('editor.modelId')}</span><input className="input" value={draft.modelName || (draft.name ? normalizeRouterModelName(draft.name) : '')} readOnly /></label>
              </div>
            </section>

            <section className="router-editor__section">
              <div className="router-editor__section-head">
                <div><h3>{t('editor.candidates.title')}</h3><p>{t('editor.candidates.description')}</p></div>
                <span className="router-editor__count">{t('editor.candidates.selected', { count: draft.candidates.length })}</span>
              </div>
              <div className="router-editor__candidate-search"><Icon name="search" size={14} /><input value={candidateSearch} placeholder={t('editor.candidates.search')} onChange={event => setCandidateSearch(event.target.value)} /></div>
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
                        {connection.kind === 'external' ? t('editor.connections.externalProvider', { provider: connection.provider || t('editor.connections.providerFallback') }) : t('editor.connections.internal')}
                      </span>
                    </label>
                  );
                })}
                {filteredCandidateModels.length === 0 && <div className="router-editor__empty">{t('editor.candidates.noMatches')}</div>}
              </div>
              <label className="router-editor__default-model">
                <span>{t('editor.defaultModel')} <small>{t('editor.defaultDescription')}</small></span>
                <RouterSelect
                  value={draft.defaultModel}
                  options={[{ value: '', label: t('editor.selectDefault') }, ...draft.candidates.map(c => ({ value: c, label: c }))]}
                  onChange={(val: string) => setPatch({ defaultModel: val })}
                  ariaLabel={t('editor.defaultModel')}
                />
              </label>

              <div className="router-editor__connections" aria-label={t('editor.connections.aria')}>
                <div className="router-editor__mini-head">
                  <span>{t('editor.connections.title')}</span>
                </div>
                {selectedConnections.length === 0 ? (
                  <div className="router-editor__empty">{t('editor.connections.empty')}</div>
                ) : (
                  <div className="router-editor__connection-list">
                    {selectedConnections.map(connection => (
                      <div className={`router-editor__connection router-editor__connection--${connection.kind}`} key={connection.modelName}>
                        <div className="router-editor__connection-main">
                          <div>
                            <strong>{connection.displayName}</strong>
                            {connection.modelName === draft.defaultModel && <span className="router-editor__default-badge">{t('editor.default')}</span>}
                          </div>
                          <div className="router-editor__connection-source">
                            <span className={`router-editor__source-badge router-editor__source-badge--${connection.kind}`}>
                              {connection.kind === 'external' ? t('editor.connections.external') : connection.kind === 'internal' ? t('editor.connections.internal') : t('editor.connections.unresolved')}
                            </span>
                            <small>{connection.kind === 'external' ? (connection.provider || t('editor.connections.unknownProvider')) : (connection.backend || connection.recipe || t('editor.connections.unknownSource'))}</small>
                          </div>
                        </div>
                        {connection.kind === 'external' && (
                          <div className="router-editor__connection-endpoint">
                            {editingProvider === connection.provider && editingConnectionModel === connection.modelName ? (
                              <div className="router-editor__endpoint-editor">
                                <input
                                  className="input"
                                  value={providerEndpointDraft}
                                  aria-label={t('editor.connections.endpointAria', { provider: connection.provider || '' })}
                                  placeholder="https://api.example.com/v1"
                                  onChange={event => setProviderEndpointDraft(event.target.value)}
                                />
                                {providerEndpointNeedsInsecureOptIn(providerEndpointDraft) && (
                                  <label className="router-editor__insecure-opt-in">
                                    <input type="checkbox" checked={providerAllowInsecureDraft} onChange={event => setProviderAllowInsecureDraft(event.target.checked)} />
                                    <span>{t('editor.connections.allowInsecure')}</span>
                                  </label>
                                )}
                                <WorkspaceActionButton size="small" appearance="primary" disabled={savingProvider} onClick={() => { void saveProviderEndpoint(); }}>
                                  {savingProvider ? t('editor.saving') : t('editor.save')}
                                </WorkspaceActionButton>
                                <WorkspaceActionButton size="small" onClick={() => { setEditingProvider(null); setEditingConnectionModel(null); setProviderAllowInsecureDraft(false); setConnectionsError(null); }}>{t('editor.cancel')}</WorkspaceActionButton>
                              </div>
                            ) : (
                              <>
                                <span title={connection.endpoint || t('editor.connections.endpointUnavailable')}>{connection.endpoint || t('editor.connections.endpointNotConfigured')}</span>
                                <small>{connection.authConfigured ? t('editor.connections.authConfigured') : t('editor.connections.authRequired')}</small>
                                {connection.provider && (
                                  <WorkspaceActionButton size="small" icon="edit" onClick={() => startEditingProvider(connection.provider, connection.endpoint, connection.modelName, connection.allowInsecureHttp)}>
                                    {t('editor.connections.editEndpoint')}
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
                              aria-label={t('editor.connections.removeCandidateNamed', { name: connection.displayName })}
                              title={t('editor.connections.removeCandidate')}
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
                <div><h3>{t('editor.strategy.title')}</h3><p>{t('editor.strategy.descriptionNew')}</p></div>
              </div>
              <div className="router-editor__strategy" role="radiogroup" aria-label={t('editor.strategy.aria')}>
                <button
                  type="button"
                  className={`router-editor__strategy-option ${draft.mode === 'rules' ? 'is-active' : ''}`}
                  role="radio"
                  aria-checked={draft.mode === 'rules'}
                  onClick={() => setRoutingMode('rules')}
                >
                  <Icon name="layers" size={18} />
                  <span><strong>{t('editor.strategy.rules')}</strong><small>{t('editor.strategy.rulesDescriptionNew')}</small></span>
                </button>
                <button
                  type="button"
                  className={`router-editor__strategy-option ${draft.mode === 'llm' ? 'is-active' : ''}`}
                  role="radio"
                  aria-checked={draft.mode === 'llm'}
                  onClick={() => setRoutingMode('llm')}
                >
                  <Icon name="brain-circuit" size={18} />
                  <span><strong>{t('editor.strategy.natural')}</strong><small>{t('editor.strategy.naturalDescriptionNew')}</small></span>
                </button>
              </div>
            </section>

            {draft.mode === 'llm' ? (
              <section className="router-editor__section" aria-label={t('editor.natural.aria')}>
                <div className="router-editor__form-grid">
                  <div className="router-editor__wide router-editor__field">
                    <span>{t('editor.natural.model')} <small>{t('editor.natural.modelHint')}</small></span>
                    <RouterModelPicker
                      models={candidateModels}
                      value={draft.llmRouter.model}
                      onChange={model => setPatch({ llmRouter: { ...draft.llmRouter, model } })}
                      placeholder={t('editor.natural.selectModel')}
                      searchPlaceholder={t('editor.natural.searchModels')}
                      ariaLabel={t('editor.natural.modelAria')}
                    />
                  </div>
                  <label className="router-editor__wide">
                    <span>{t('editor.natural.instruction')} <small>{t('editor.natural.instructionHint')}</small></span>
                    <textarea
                      className="textarea router-editor__prompt"
                      value={draft.llmRouter.prompt}
                      placeholder={t('editor.natural.placeholder')}
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
                <div><h3>{t('editor.classifiers.title')}</h3><p>{t('editor.classifiers.descriptionNew')}</p></div>
                <div className="router-editor__section-actions">
                  <WorkspaceActionButton size="small" icon="plus" onClick={() => addClassifier('classifier')}>{t('editor.classifiers.addClassifier')}</WorkspaceActionButton>
                  <WorkspaceActionButton size="small" icon="plus" onClick={() => addClassifier('semantic_similarity')}>{t('editor.classifiers.addSemantic')}</WorkspaceActionButton>
                  <WorkspaceActionButton size="small" icon="plus" onClick={() => addClassifier('llm')}>{t('editor.classifiers.addLlm')}</WorkspaceActionButton>
                </div>
              </div>
              {draft.classifiers.length === 0 ? <div className="router-editor__empty">{t('editor.classifiers.empty')}</div> : (
                <div className="router-editor__classifier-list">
                  {draft.classifiers.map((classifier, index) => {
                    const labels = classifierLabels(classifier);
                    return (
                      <article className="router-editor__classifier" key={index}>
                        <div className="router-editor__card-head">
                          <strong>{classifier.type === 'semantic_similarity' ? t('editor.classifiers.semantic') : classifier.type === 'llm' ? t('editor.classifiers.llm') : t('editor.classifiers.text')}</strong>
                          <WorkspaceActionButton appearance="danger" size="small" icon="trash" iconOnly onClick={() => removeClassifier(index)} aria-label={t('editor.classifiers.remove')} title={t('editor.classifiers.remove')} />
                        </div>
                        <div className="router-editor__form-grid router-editor__form-grid--classifier">
                          <label><span>ID</span><CommittedTextInput value={classifier.id} ariaLabel={t('editor.classifiers.idAria', { index: index + 1 })} normalize={input => input} onCommit={nextId => commitClassifierId(index, nextId)} /></label>
                          <label><span>{t('editor.classifiers.type')}</span><RouterSelect value={classifier.type} options={[{ value: 'classifier', label: 'classifier' }, { value: 'semantic_similarity', label: 'semantic_similarity' }, { value: 'llm', label: 'llm' }]} onChange={(val: string) => updateClassifier(index, { ...createRouterClassifier(index, val as RouterClassifier['type']), id: classifier.id })} ariaLabel={t('editor.classifiers.type')} /></label>
                          <div className="router-editor__wide router-editor__field">
                            <span>{t('editor.classifiers.model')}</span>
                            <RouterModelPicker
                              models={classifier.type === 'semantic_similarity' ? embeddingModels : classifier.type === 'llm' ? candidateModels : classifierModels}
                              value={classifier.model}
                              onChange={model => updateClassifier(index, { model })}
                              placeholder={t('editor.classifiers.selectModel')}
                              searchPlaceholder={classifier.type === 'semantic_similarity' ? t('editor.classifiers.searchEmbedding') : classifier.type === 'llm' ? t('editor.classifiers.searchChat') : t('editor.classifiers.searchClassification')}
                              ariaLabel={t('editor.classifiers.modelAria', { id: classifier.id || t('editor.classifiers.numbered', { index: index + 1 }) })}
                            />
                          </div>
                          {classifier.type === 'semantic_similarity' ? (
                            <div className="router-editor__wide router-editor__concepts">
                              <div className="router-editor__mini-head"><span>{t('editor.classifiers.concepts')}</span><WorkspaceActionButton size="small" icon="plus" onClick={() => updateClassifier(index, { referencePhrases: { ...classifier.referencePhrases, [nextConceptName(classifier.referencePhrases)]: ['example phrase'] } })}>{t('editor.classifiers.concept')}</WorkspaceActionButton></div>
                              <div className="router-editor__concept-list">
                                {Object.entries(classifier.referencePhrases).map(([concept, phrases], conceptIndex) => (
                                  <div className="router-editor__concept" key={conceptIndex}>
                                    <CommittedTextInput value={concept} ariaLabel={t('editor.classifiers.conceptName')} onCommit={nextName => commitSemanticConceptName(index, concept, nextName)} />
                                    <textarea className="textarea" rows={3} value={phrases.join('\n')} aria-label={t('editor.classifiers.referencePhrases')} placeholder={t('editor.classifiers.onePhrasePerLine')} onChange={event => updateClassifier(index, { referencePhrases: { ...classifier.referencePhrases, [concept]: event.target.value.split(/\r?\n/) } })} />
                                    <WorkspaceActionButton appearance="danger" size="small" icon="trash" iconOnly title={t('editor.classifiers.removeConcept')} aria-label={t('editor.classifiers.removeConcept')} onClick={() => {
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
                                  <span>{t('editor.classifiers.prompt')} <small>{t('editor.classifiers.promptHint')}</small></span>
                                  <textarea className="textarea router-editor__prompt" value={classifier.prompt} placeholder={t('editor.classifiers.promptPlaceholder')} spellCheck={false} onChange={event => updateClassifier(index, { prompt: event.target.value })} />
                                </label>
                              )}
                              <label className="router-editor__wide"><span>{t('editor.classifiers.outputLabels')} <small>{t('node.onePerLine')}</small></span><textarea
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
                          <label><span>{t('editor.classifiers.defaultLabel')}</span><RouterSelect value={classifier.defaultLabel || ''} options={[{ value: '', label: t('editor.none') }, ...labels.map(label => ({ value: label, label }))]} onChange={(val: string) => updateClassifier(index, { defaultLabel: val || undefined })} ariaLabel={t('editor.classifiers.defaultLabel')} /></label>
                          <label><span>{t('editor.classifiers.onError')}</span><RouterSelect value={classifier.onError} options={[{ value: 'match_false', label: t('editor.classifiers.doNotMatch') }, { value: 'match_true', label: t('editor.classifiers.matchRule') }]} onChange={(val: string) => updateClassifier(index, { onError: val as RouterClassifier['onError'] })} ariaLabel={t('editor.classifiers.onError')} /></label>
                        </div>
                      </article>
                    );
                  })}
                </div>
              )}
            </section>

            <section className="router-editor__section">
              <div className="router-editor__section-head">
                <div><h3>{t('editor.rules.title')}</h3><p>{t('editor.rules.descriptionNew')}</p></div>
                <WorkspaceActionButton size="small" icon="plus" onClick={addRule}>{t('editor.rules.add')}</WorkspaceActionButton>
              </div>
              <div className="router-editor__rules-workspace">
                <div className="router-editor__rule-list" aria-label={t('editor.rules.aria')}>
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
                        title={t('editor.rules.drag')}
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
                          <strong>{rule.id || t('editor.rules.defaultName', { index: index + 1 })}</strong>
                          <small>{rule.routeTo ? `→ ${rule.routeTo}` : t('editor.rules.noRoute')}</small>
                        </span>
                      </button>
                      <div className="router-editor__rule-actions">
                        <div className="router-editor__rule-stepper">
                          <button className="router-editor__rule-stepper-button router-editor__rule-stepper-button--up" type="button" disabled={index === 0} onClick={() => moveRuleTo(index, index - 1)} title={t('editor.rules.moveUp')}><Icon name="chevron-up" size={10} /></button>
                          <button className="router-editor__rule-stepper-button router-editor__rule-stepper-button--down" type="button" disabled={index === draft.rules.length - 1} onClick={() => moveRuleTo(index, index + 1)} title={t('editor.rules.moveDown')}><Icon name="chevron-down" size={10} /></button>
                        </div>
                        <button className="router-editor__rule-remove" type="button" onClick={() => removeRuleAt(index)} title={t('editor.rules.remove')}><Icon name="trash" size={14} /></button>
                      </div>
                    </div>
                  ))}
                  {draft.rules.length === 0 && <div className="router-editor__empty">{t('editor.rules.required')}</div>}
                  {draft.defaultModel && (
                    <div className="router-editor__default-rule">
                      <span className="router-editor__rule-order">↩</span>
                      <span><strong>{t('editor.default')}</strong><small>→ {draft.defaultModel}</small></span>
                    </div>
                  )}
                </div>

                <div className="router-editor__rule-builder">
                  {selectedRule ? (
                    <>
                      <div className="router-editor__rule-builder-head">
                        <span className="router-editor__rule-builder-label">{t('editor.rules.numbered', { index: selectedRuleIndex + 1 })}:</span>
                        <input
                          className="input router-editor__rule-id-input"
                          value={selectedRule.id}
                          aria-label={t('editor.rules.id')}
                          onChange={event => setPatch({ rules: draft.rules.map((item, itemIndex) => itemIndex === selectedRuleIndex ? { ...item, id: event.target.value } : item) })}
                        />
                        <RouterSelect
                          className="router-editor__rule-route-select"
                          value={selectedRule.routeTo}
                          options={[{ value: '', label: t('editor.rules.routeToEllipsis') }, ...draft.candidates.map(c => ({ value: c, label: c }))]}
                          onChange={(val: string) => setPatch({ rules: draft.rules.map((item, itemIndex) => itemIndex === selectedRuleIndex ? { ...item, routeTo: val } : item) })}
                          ariaLabel={t('editor.rules.routeTo')}
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
                        <summary>{t('editor.rules.outputs')}</summary>
                        <textarea className="textarea" value={selectedRule.outputsText || ''} placeholder={'{\n  "tier": "fast"\n}'} spellCheck={false} onChange={event => setPatch({ rules: draft.rules.map((item, itemIndex) => itemIndex === selectedRuleIndex ? { ...item, outputsText: event.target.value } : item) })} />
                      </details>
                    </>
                  ) : (
                    <div className="router-editor__rule-builder-empty">
                      <Icon name="router" size={18} />
                      <strong>{t('editor.rules.selectOrAdd')}</strong>
                      <span>{t('editor.rules.graphAppears')}</span>
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
            <strong><Icon name="alert" size={14} /> {t('editor.validationIssues', { count: validationErrors.length })}</strong>
            <ul>{validationErrors.slice(0, 8).map((message, index) => <li key={`${message}-${index}`}>{message}</li>)}</ul>
          </section>
        )}
        {error && <div className="router-editor__message router-editor__message--error"><Icon name="alert" size={14} /> {error}</div>}
      </div>

      {notice && (
        <div className="router-editor__toast" role="status" aria-live="polite">
          <Icon name="check" size={15} />
          <span>{routerNoticeText(notice, t)}</span>
          <button type="button" onClick={() => setNotice(null)} aria-label={t('editor.dismissNotification')}><Icon name="x" size={12} /></button>
        </div>
      )}

      <Modal
        isOpen={expandedRule != null && expandedRuleIndex != null}
        onClose={() => setExpandedRuleIndex(null)}
        title={expandedRule && expandedRuleIndex != null ? t('editor.rules.graphTitle', { index: expandedRuleIndex + 1, id: expandedRule.id || t('editor.rules.untitled') }) : t('editor.rules.graphBuilder')}
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
        title={confirmation?.title || t('editor.confirm.fallbackTitle')}
        maxWidth="480px"
        ariaLabelledBy="router-confirm-dialog-title"
      >
        <div className="inspect-modal-body router-confirm-dialog__body">
          <div className="router-confirm-dialog__icon"><Icon name="alert" size={18} /></div>
          <p>{confirmation?.message}</p>
        </div>
        <div className="inspect-modal-footer">
          <WorkspaceActionButton appearance="secondary" disabled={deleting} onClick={dismissConfirmation}>{t('editor.close')}</WorkspaceActionButton>
          <WorkspaceActionButton appearance={confirmation?.tone || 'primary'} disabled={deleting} onClick={confirmPendingAction}>
            {deleting && confirmation?.kind === 'delete' ? t('editor.deleting') : (confirmation?.confirmLabel || t('editor.continue'))}
          </WorkspaceActionButton>
        </div>
      </Modal>

    </WorkspaceDetailPanel>
  );
};

export default RouterEditorPanel;
