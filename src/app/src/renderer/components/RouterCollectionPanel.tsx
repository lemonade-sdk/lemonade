import React, { useEffect, useMemo, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { useModels } from '../hooks/useModels';
import { getModelDisplayName } from '../utils/modelDisplayName';
import { serverFetch } from '../utils/serverConfig';
import {
  RouterClassifier,
  RouterCollectionDraft,
  RouterRule,
  buildRouterCollectionPullRequest,
  getRouterCandidateOptions,
  makeCollectionId,
  routingToRouterCollectionDraft,
} from '../utils/customCollections';
import { isCollectionRecipe } from '../utils/recipeNames';
import { isLeaf, isOperatorNode, makeDefaultLeaf, SIGNAL_COLORS, validateRuleNode, type ConditionSignalType } from '../utils/routerTree';
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

// Rewrite a classifier id in all condition leaves
function rewriteClassifierId(
  node: RouterRule['conditionTree'],
  oldId: string,
  newId: string,
): RouterRule['conditionTree'] {
  if (!node) return null;
  if (isLeaf(node)) {
    return node.signalType === 'classifier' && node.classifierId === oldId
      ? { ...node, classifierId: newId }
      : node;
  }
  if (isOperatorNode(node)) {
    return { ...node, conditions: node.conditions.map(c => rewriteClassifierId(c, oldId, newId) ?? c) };
  }
  return node;
}

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

type QuickConditionType = 'min_chars' | 'max_chars' | 'keywords_any' | 'regex' | 'has_images' | 'has_tools';

interface QCDef { type: QuickConditionType; label: string; shortLabel: string; placeholder?: string; inputType?: 'number' | 'text'; boolean?: true }

const QUICK_CONDITIONS: QCDef[] = [
  { type: 'min_chars',    label: 'Prompt is longer than…', shortLabel: 'Prompt is longer', inputType: 'number', placeholder: '500' },
  { type: 'max_chars',    label: 'Prompt is shorter than…', shortLabel: 'Prompt is shorter', inputType: 'number', placeholder: '200' },
  { type: 'keywords_any', label: 'Contains keywords',    shortLabel: 'Contains keywords', inputType: 'text',   placeholder: 'word1, word2' },
  { type: 'regex',        label: 'Matches regex',        shortLabel: 'Matches regex',     inputType: 'text',   placeholder: 'e.g. \\d{4}' },
  { type: 'has_images',   label: 'Has attached images',  shortLabel: 'Has images',        boolean: true },
  { type: 'has_tools',    label: 'Has tool calls',       shortLabel: 'Has tool calls',    boolean: true },
];

interface QuickChip { conditionType: QuickConditionType; value: string; not: boolean }

const QUICK_CONDITION_TYPES = new Set<string>(QUICK_CONDITIONS.map(c => c.type));

function leafToChip(signalType: string, signalValue: unknown, not: boolean): QuickChip | null {
  if (!QUICK_CONDITION_TYPES.has(signalType)) return null;
  return { conditionType: signalType as QuickConditionType, value: signalValue !== undefined ? String(signalValue) : '', not };
}

function getRowChips(rule: RouterRule): { op: 'AND' | 'OR'; chips: QuickChip[] } {
  const tree = rule.conditionTree;
  if (!tree) return { op: 'AND', chips: [] };
  if ('signalType' in tree) {
    const chip = leafToChip(tree.signalType, tree.signalValue, tree.not ?? false);
    return { op: 'AND', chips: chip ? [chip] : [] };
  }
  if ('operator' in tree && (tree.operator === 'AND' || tree.operator === 'OR')) {
    const chips = tree.conditions
      .filter(isLeaf)
      .map(c => leafToChip(c.signalType, c.signalValue, c.not ?? false))
      .filter((c): c is QuickChip => c !== null);
    return { op: tree.operator, chips };
  }
  return { op: 'AND', chips: [] };
}

function chipsToRouterRule(rule: RouterRule, op: 'AND' | 'OR', chips: QuickChip[]): RouterRule {
  if (chips.length === 0) return { ...rule, conditionTree: null };
  const leaves = chips.map(chip => {
    const leaf = makeDefaultLeaf(chip.conditionType as ConditionSignalType);
    if (chip.conditionType === 'min_chars' || chip.conditionType === 'max_chars') {
      leaf.signalValue = chip.value ? parseInt(chip.value, 10) : undefined;
    } else if (chip.conditionType === 'keywords_any' || chip.conditionType === 'regex') {
      leaf.signalValue = chip.value;
    }
    if (chip.not) leaf.not = true;
    return leaf;
  });
  const conditionTree = leaves.length === 1 ? leaves[0] : { operator: op, conditions: leaves };
  return { ...rule, conditionTree };
}

function readableSummary(rule: RouterRule, displayName: (id: string) => string): string {
  const { op, chips } = getRowChips(rule);
  const parts = chips.map(c => {
    const def = QUICK_CONDITIONS.find(d => d.type === c.conditionType);
    const label = def?.shortLabel ?? c.conditionType;
    const val = !def?.boolean && c.value ? ` (${c.value})` : '';
    return (c.not ? 'not ' : '') + label.toLowerCase() + val;
  });
  const joined = parts.join(` ${op.toLowerCase()} `);
  return rule.routeTo ? `${joined} → ${displayName(rule.routeTo)}` : joined;
}


interface CandidateOptionWithInfo { id: string; info: { model_name?: string; downloaded?: boolean; cost_input_per_million?: number; recipe?: string } }


type PresetKey = 'cost_saver' | 'quality' | 'speed' | 'privacy';

interface PresetDef { key: PresetKey; emoji: string; label: string }
const PRESETS: PresetDef[] = [
  { key: 'cost_saver', emoji: '💰', label: 'Cost Saver' },
  { key: 'speed',      emoji: '⚡', label: 'Speed' },
  { key: 'privacy',    emoji: '🔒', label: 'Privacy' },
];

function buildPresetRules(key: PresetKey, _opts: CandidateOptionWithInfo[], seq: { current: number }): { rules: RouterRule[] } {
  const nextId = () => { seq.current++; return `rule-${seq.current}`; };

  const leaf = (conditionType: QuickConditionType, value: string, not = false): RouterRule['conditionTree'] => {
    const l = makeDefaultLeaf(conditionType as ConditionSignalType);
    if (conditionType === 'min_chars' || conditionType === 'max_chars') l.signalValue = parseInt(value, 10);
    else if (conditionType === 'keywords_any' || conditionType === 'regex') l.signalValue = value;
    if (not) l.not = true;
    return l;
  };

  if (key === 'cost_saver') {
    return { rules: [
      { id: nextId(), routeTo: '', conditionTree: leaf('min_chars', '500') },
      { id: nextId(), routeTo: '', conditionTree: leaf('has_images', '') },
      { id: nextId(), routeTo: '', conditionTree: leaf('keywords_any', 'translate, summarize, list') },
    ]};
  }
  if (key === 'quality') {
    return { rules: [
      { id: nextId(), routeTo: '', conditionTree: leaf('has_images', '') },
      { id: nextId(), routeTo: '', conditionTree: leaf('min_chars', '200') },
      { id: nextId(), routeTo: '', conditionTree: leaf('has_tools', '') },
    ]};
  }
  if (key === 'speed') {
    return { rules: [
      { id: nextId(), routeTo: '', conditionTree: leaf('max_chars', '300') },
      { id: nextId(), routeTo: '', conditionTree: leaf('min_chars', '300') },
    ]};
  }
  // privacy
  return { rules: [
    { id: nextId(), routeTo: '', conditionTree: leaf('min_chars', '1') },
  ]};
}


interface QuickRuleRowProps {
  rule: RouterRule;
  index: number;
  candidates: string[];
  displayName: (id: string) => string;
  dragIndex: number | null;
  onDragStart: (idx: number) => void;
  onDragOver: (idx: number) => void;
  onDrop: () => void;
  onChange: (rule: RouterRule) => void;
  onRemove: () => void;
}

const QuickRuleRow: React.FC<QuickRuleRowProps> = ({
  rule, index, candidates, displayName, dragIndex,
  onDragStart, onDragOver, onDrop, onChange, onRemove,
}) => {
  const [addOpen, setAddOpen] = useState(false);
  const { op, chips } = getRowChips(rule);
  const isDragging = dragIndex === index;

  const updateChip = (i: number, partial: Partial<QuickChip>) => {
    const next = chips.map((c, ci) => ci === i ? { ...c, ...partial } : c);
    onChange(chipsToRouterRule(rule, op, next));
  };
  const removeChip = (i: number) => {
    const next = chips.filter((_, ci) => ci !== i);
    if (next.length === 0) { onRemove(); return; }
    onChange(chipsToRouterRule(rule, op, next));
  };
  const addChip = (type: QuickConditionType) => {
    const value = type === 'min_chars' ? '500' : type === 'max_chars' ? '200' : '';
    const next = [...chips, { conditionType: type, value, not: false }];
    onChange(chipsToRouterRule(rule, op, next));
    setAddOpen(false);
  };
  const setOp = (newOp: 'AND' | 'OR') => onChange(chipsToRouterRule(rule, newOp, chips));
  const setRouteTo = (id: string) => onChange({ ...rule, routeTo: id });

  const summary = readableSummary(rule, displayName);

  return (
    <div
      className={`quick-rule-row${isDragging ? ' quick-rule-row--dragging' : ''}`}
      draggable
      onDragStart={() => onDragStart(index)}
      onDragOver={e => { e.preventDefault(); onDragOver(index); }}
      onDrop={e => { e.preventDefault(); onDrop(); }}
    >
      <span className="quick-rule-handle" title="Drag to reorder">⠿</span>
      <span className="quick-rule-index">{index + 1}</span>

      <div className="quick-rule-body">
        <div className="quick-rule-chips">
          {chips.map((chip, ci) => {
            const def = QUICK_CONDITIONS.find(d => d.type === chip.conditionType)!;
            return (
              <React.Fragment key={ci}>
                {ci > 0 && (
                  <div className="quick-rule-op-group">
                    <button type="button" className={`quick-op-btn${op === 'AND' ? ' active' : ''}`} onClick={() => setOp('AND')}>AND</button>
                    <button type="button" className={`quick-op-btn${op === 'OR' ? ' active' : ''}`} onClick={() => setOp('OR')}>OR</button>
                  </div>
                )}
                <div className={`quick-chip${chip.not ? ' quick-chip--not' : ''}`} style={{ '--chip-color': SIGNAL_COLORS[chip.conditionType as ConditionSignalType] } as React.CSSProperties}>
                  <button type="button" className="quick-chip-not" title={chip.not ? 'Remove NOT' : 'Negate (NOT)'} onClick={() => updateChip(ci, { not: !chip.not })}>
                    {chip.not ? '¬' : '+'}
                  </button>
                  <span className="quick-chip-label">{def.shortLabel}</span>
                  {!def.boolean && (
                    <input
                      className="quick-chip-input"
                      type={def.inputType ?? 'text'}
                      value={chip.value}
                      placeholder={def.placeholder}
                      min={def.inputType === 'number' ? 0 : undefined}
                      onClick={e => e.stopPropagation()}
                      onChange={e => updateChip(ci, { value: e.target.value })}
                    />
                  )}
                  <button type="button" className="quick-chip-remove" onClick={() => removeChip(ci)}>×</button>
                </div>
              </React.Fragment>
            );
          })}
          {/* Add condition */}
          <div className="quick-add-condition-wrap">
            <button type="button" className="quick-add-condition-btn" onClick={() => setAddOpen(v => !v)}>+ condition</button>
            {addOpen && (
              <>
                <div style={{ position: 'fixed', inset: 0, zIndex: 9999 }} onClick={() => setAddOpen(false)} />
                <div className="quick-add-condition-menu">
                  {QUICK_CONDITIONS.map(d => (
                    <button key={d.type} type="button" className="quick-add-condition-item" onClick={() => addChip(d.type)}>{d.label}</button>
                  ))}
                </div>
              </>
            )}
          </div>
        </div>

        <div className="quick-rule-summary">Reads: <em>{summary}</em></div>
      </div>

      {/* Arrow + model */}
      <span className="quick-rule-arrow">→</span>
      <select className="form-input form-select quick-rule-model" value={rule.routeTo} onChange={e => setRouteTo(e.target.value)}>
        <option value="">Model…</option>
        {candidates.map(id => <option key={id} value={id}>{displayName(id)}</option>)}
      </select>

      <button type="button" className="pipeline-icon-btn quick-rule-remove" title="Remove rule" onClick={onRemove}>
        <svg width="10" height="10" viewBox="0 0 12 12" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round"><path d="M1 1 L11 11 M11 1 L1 11"/></svg>
      </button>
    </div>
  );
};


interface QuickRulesEditorProps {
  rules: RouterRule[];
  candidates: string[];
  candidateOptions: CandidateOptionWithInfo[];
  displayName: (id: string) => string;
  onChangeRules: (rules: RouterRule[]) => void;
  onChangeDraft: (p: { rules: RouterRule[]; defaultModel?: string }) => void;
  onExpandToAdvanced: () => void;
  ruleSeqRef: React.MutableRefObject<number>;
}

const QuickRulesEditor: React.FC<QuickRulesEditorProps> = ({
  rules, candidates, candidateOptions, displayName,
  onChangeRules, onChangeDraft, onExpandToAdvanced, ruleSeqRef,
}) => {
  const [dragIndex, setDragIndex] = useState<number | null>(null);
  const [dragOver, setDragOver] = useState<number | null>(null);
  const [activePreset, setActivePreset] = useState<PresetKey | 'blank' | null>(null);

  const addRow = () => {
    ruleSeqRef.current += 1;
    const id = `rule-${ruleSeqRef.current}`;
    onChangeRules([...rules, { id, routeTo: '', conditionTree: makeDefaultLeaf('min_chars') }]);
  };

  const applyPreset = (key: PresetKey) => {
    const { rules: newRules } = buildPresetRules(key, candidateOptions, ruleSeqRef);
    onChangeDraft({ rules: newRules });
    setActivePreset(key);
  };

  const handleDrop = () => {
    if (dragIndex === null || dragOver === null || dragIndex === dragOver) { setDragIndex(null); setDragOver(null); return; }
    const next = [...rules];
    const [moved] = next.splice(dragIndex, 1);
    next.splice(dragOver, 0, moved);
    onChangeRules(next);
    setDragIndex(null); setDragOver(null);
  };

  return (
    <div className="quick-rules-editor">
      <div className="quick-presets-bar">
        <span className="quick-presets-label">Start from</span>
        {PRESETS.map(p => (
          <button key={p.key} type="button" className={`quick-preset-btn${activePreset === p.key ? ' quick-preset-btn--active' : ''}`} onClick={() => applyPreset(p.key)}>
            {p.emoji} {p.label}
          </button>
        ))}
        <button type="button" className={`quick-preset-btn quick-preset-btn--blank${activePreset === 'blank' ? ' quick-preset-btn--active' : ''}`} onClick={() => { onChangeDraft({ rules: [] }); setActivePreset('blank'); }}>
          Blank
        </button>
      </div>

      <div className="quick-rules-header">
        <span className="quick-rules-title">Rules</span>
        <span className="settings-description">first match wins</span>
      </div>

      {rules.length === 0 && (
        <div className="quick-rules-empty">No rules yet - pick a preset above or add one below.</div>
      )}

      {rules.map((rule, idx) => (
        <QuickRuleRow
          key={rule.id}
          rule={rule}
          index={idx}
          candidates={candidates}
          displayName={displayName}
          dragIndex={dragIndex}
          onDragStart={i => { setDragIndex(i); setDragOver(i); }}
          onDragOver={i => setDragOver(i)}
          onDrop={handleDrop}
          onChange={updated => { const next = [...rules]; next[idx] = updated; onChangeRules(next); }}
          onRemove={() => onChangeRules(rules.filter((_, i) => i !== idx))}
        />
      ))}

      <div className="quick-rules-footer">
        <button type="button" className="settings-reset-button" onClick={addRow}>+ Add rule</button>
        {rules.length > 0 && (
          <button type="button" className="settings-reset-button" onClick={onExpandToAdvanced} title="Open these rules in the Advanced canvas">
            Expand in Advanced
          </button>
        )}
      </div>
    </div>
  );
};

// ── Prompt textarea with @ mention picker ─────────────────────────────────

interface PromptTextareaProps {
  value: string;
  onChange: (v: string) => void;
  candidates: string[];
  displayName: (id: string) => string;
  placeholder?: string;
}

const PromptTextarea: React.FC<PromptTextareaProps> = ({ value, onChange, candidates, displayName, placeholder }) => {
  const [mentionOpen, setMentionOpen] = useState(false);
  const [mentionPos, setMentionPos] = useState({ top: 0, left: 0 });
  const [atIndex, setAtIndex] = useState(-1);
  const taRef = useRef<HTMLTextAreaElement>(null);
  const wrapRef = useRef<HTMLDivElement>(null);

  const handleChange = (e: React.ChangeEvent<HTMLTextAreaElement>) => {
    const val = e.target.value;
    onChange(val);
    const cursor = e.target.selectionStart ?? val.length;
    const before = val.slice(0, cursor);
    const lastAt = before.lastIndexOf('@');
    if (lastAt !== -1 && !/[\s\n]/.test(before.slice(lastAt + 1))) {
      const ta = taRef.current;
      const wrap = wrapRef.current;
      if (ta && wrap) {
        const taRect = ta.getBoundingClientRect();
        const wrapRect = wrap.getBoundingClientRect();
        setMentionPos({ top: taRect.bottom - wrapRect.top + 4, left: 0 });
      }
      setAtIndex(lastAt);
      setMentionOpen(true);
    } else {
      setMentionOpen(false);
    }
  };

  const insertCandidate = (id: string) => {
    const name = displayName(id);
    const cursorAfterAt = taRef.current?.selectionStart ?? atIndex + 1;
    const before = value.slice(0, atIndex);
    const after = value.slice(cursorAfterAt);
    const next = before + name + (after.startsWith(' ') ? '' : ' ') + after;
    onChange(next);
    setMentionOpen(false);
    setTimeout(() => {
      const ta = taRef.current;
      if (ta) { ta.focus(); ta.selectionStart = ta.selectionEnd = before.length + name.length + 1; }
    }, 0);
  };

  return (
    <div ref={wrapRef} style={{ position: 'relative' }}>
      <textarea
        ref={taRef}
        className="form-input"
        rows={5}
        value={value}
        onChange={handleChange}
        onKeyDown={e => { if (e.key === 'Escape') setMentionOpen(false); }}
        placeholder={placeholder}
        spellCheck={false}
        style={{ fontFamily: 'monospace', fontSize: '12px', resize: 'vertical', width: '100%' }}
      />
      {mentionOpen && candidates.length > 0 && (
        <>
          <div style={{ position: 'fixed', inset: 0, zIndex: 10799 }} onMouseDown={() => setMentionOpen(false)} />
          <div className="prompt-mention-menu" style={{ top: mentionPos.top, left: mentionPos.left }}>
            <div className="prompt-mention-hint">Insert candidate model</div>
            {candidates.map(id => (
              <button key={id} type="button" className="prompt-mention-item"
                onMouseDown={e => { e.preventDefault(); insertCandidate(id); }}>
                {displayName(id)}
              </button>
            ))}
          </div>
        </>
      )}
    </div>
  );
};

// ── Panel ─────────────────────────────────────────────────────────────────

const RouterCollectionPanel: React.FC<RouterCollectionPanelProps> = ({
  mode, collectionId, onClose, onSave, onExport,
}) => {
  const { modelsData } = useModels();
  const [draft, setDraft] = useState<RouterCollectionDraft>(() => emptyDraft());
  const [error, setError] = useState<string | null>(null);
  const [previewJson, setPreviewJson] = useState<string | null>(null);
  const [confirmAdvanced, setConfirmAdvanced] = useState(false);
  const [confirmQuick, setConfirmQuick] = useState(false);
  const [lossyAcknowledged, setLossyAcknowledged] = useState(false);
  const ruleSeqRef = useRef(0);
  const clfSeqRef = useRef(0);

  // Seed sequence counters from loaded rules/classifiers so newly generated IDs
  // never collide with existing ones (e.g. rule-1 already present on edit-open).
  const seedSeqRefs = (d: RouterCollectionDraft) => {
    for (const r of d.rules ?? []) {
      const m = r.id.match(/^rule-(\d+)$/);
      if (m) ruleSeqRef.current = Math.max(ruleSeqRef.current, parseInt(m[1], 10));
    }
    for (const c of d.classifiers ?? []) {
      const m = c.id.match(/^clf-(\d+)$/);
      if (m) clfSeqRef.current = Math.max(clfSeqRef.current, parseInt(m[1], 10));
    }
  };

  const candidateOptions = useMemo(() => getRouterCandidateOptions(modelsData), [modelsData]);

  const embeddingOptions = useMemo(() =>
    Object.entries(modelsData)
      .filter(([, info]) => (info?.labels ?? []).some(l => l === 'embeddings' || l === 'embedding') && !isCollectionRecipe(info?.recipe))
      .map(([id, info]) => ({ id, info }))
      .sort((a, b) => {
        const dl = Number(b.info.downloaded === true) - Number(a.info.downloaded === true);
        return dl !== 0 ? dl : (a.info.model_name ?? a.id).localeCompare(b.info.model_name ?? b.id);
      }),
  [modelsData]);

  // Models suitable for type:"classifier" - only text-classification models
  // (registry label "classification"), never chat LLMs, embeddings, or collections.
  const classifierModelOptions = useMemo(() =>
    Object.entries(modelsData)
      .filter(([, info]) => !isCollectionRecipe(info?.recipe) && (info?.labels ?? []).includes('classification'))
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
    serverFetch(`/models/${encodeURIComponent(collectionId)}`)
      .then(r => r.json())
      .then((raw: unknown) => {
        if (!raw || typeof raw !== 'object' || Array.isArray(raw)) {
          setError('Could not load router policy. Refresh and try again.'); return;
        }
        const rec = raw as Record<string, unknown>;
        if (!rec.routing || typeof rec.routing !== 'object') {
          setError('This model has no routing policy. Refresh and try again.'); return;
        }
        const loaded = routingToRouterCollectionDraft(
          collectionId,
          rec.routing as Record<string, unknown>,
          Array.isArray(rec.components) ? rec.components as string[] : [],
        );
        seedSeqRefs(loaded);
        setDraft(loaded);
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

  // Lossy markers only make sense while the flagged rule still exists - prune
  // them whenever the rule set changes so stale warnings can't block saving.
  const pruneLossyRuleIds = (lossy: string[] | undefined, rules: RouterRule[]): string[] | undefined => {
    if (!lossy?.length) return undefined;
    const ids = new Set(rules.map(r => r.id));
    const kept = lossy.filter(id => ids.has(id));
    return kept.length > 0 ? kept : undefined;
  };

  const patch = (p: Partial<RouterCollectionDraft>) => {
    setDraft(prev => {
      const next = { ...prev, ...p };
      if ('rules' in p) next.lossyRuleIds = pruneLossyRuleIds(prev.lossyRuleIds, next.rules ?? []);
      return next;
    });
    setError(null);
    setPreviewJson(null);
    if ('rules' in p || 'routingMode' in p) setLossyAcknowledged(false);
  };

  const toggleCandidate = (id: string) => {
    setDraft(prev => {
      const next = prev.candidates.includes(id)
        ? prev.candidates.filter(c => c !== id)
        : [...prev.candidates, id];
      const defaultModel = next.includes(prev.defaultModel) ? prev.defaultModel : '';
      const rules = (prev.rules ?? []).filter(r => next.includes(r.routeTo));
      return { ...prev, candidates: next, defaultModel, rules, lossyRuleIds: pruneLossyRuleIds(prev.lossyRuleIds, rules) };
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
    setDraft(prev => {
      const classifiers = (prev.classifiers ?? []).map(c => c.id === id ? { ...c, ...p } : c);
      // If the ID changed, rewrite all classifier references in rule trees
      const newId = p.id;
      const rules = newId && newId !== id
        ? (prev.rules ?? []).map(r => ({
            ...r,
            conditionTree: rewriteClassifierId(r.conditionTree, id, newId),
          }))
        : prev.rules;
      return { ...prev, classifiers, rules };
    });
    setError(null);
  };

  const addRule = (): string => {
    if (draft.candidates.length === 0) {
      setError('Select at least one candidate model before adding rules.');
      return '';
    }
    const id = nextRuleId();
    setDraft(prev => ({
      ...prev,
      rules: [...(prev.rules ?? []), { id, routeTo: prev.candidates[0] ?? '', conditionTree: null }],
    }));
    return id;
  };

  const removeRule = (id: string) => {
    setDraft(prev => {
      const rules = (prev.rules ?? []).filter(r => r.id !== id);
      return { ...prev, rules, lossyRuleIds: pruneLossyRuleIds(prev.lossyRuleIds, rules) };
    });
  };

  const patchRule = (id: string, p: Partial<RouterRule>) => {
    setDraft(prev => {
      const rules = (prev.rules ?? []).map(r => r.id === id ? { ...r, ...p } : r);
      const lossy = p.id && p.id !== id
        ? prev.lossyRuleIds?.map(l => (l === id ? p.id! : l))
        : prev.lossyRuleIds;
      return { ...prev, rules, lossyRuleIds: pruneLossyRuleIds(lossy, rules) };
    });
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
    } else if (draft.routingMode === 'quick') {
      if (!draft.rules?.length) { setError('Add at least one rule.'); return null; }
      for (let ri = 0; ri < draft.rules.length; ri++) {
        const r = draft.rules[ri];
        const label = `Rule #${ri + 1}`;
        if (!r.conditionTree) { setError(`${label} needs at least one condition.`); return null; }
        if (!r.routeTo) { setError(`${label} needs a target model.`); return null; }
        const errs = validateRuleNode(r.conditionTree, new Set(), []);
        if (errs.length) { setError(`${label}: ${errs[0]}`); return null; }
      }
    } else {
      // Duplicate classifier ID check
      const clfIds = (draft.classifiers ?? []).map(c => c.id.trim()).filter(Boolean);
      const dupClf = clfIds.find((id, i) => clfIds.indexOf(id) !== i);
      if (dupClf) { setError(`Duplicate classifier ID: "${dupClf}"`); return null; }
      // Duplicate rule ID check
      const ruleIds = (draft.rules ?? []).map(r => r.id.trim()).filter(Boolean);
      const dupRule = ruleIds.find((id, i) => ruleIds.indexOf(id) !== i);
      if (dupRule) { setError(`Duplicate rule ID: "${dupRule}"`); return null; }
      for (const c of draft.classifiers ?? []) {
        if (!c.id.trim()) { setError('Each classifier needs an id.'); return null; }
        if (!c.model) { setError(`Classifier "${c.id}": select a model.`); return null; }
        if (c.type === 'llm') {
          if (!c.prompt?.trim()) {
            setError(`Classifier "${c.id}": enter a routing prompt.`); return null;
          }
          if (!c.labels?.length) {
            setError(`Classifier "${c.id}": LLM classifiers need at least one label.`); return null;
          }
        }
        if (c.type === 'semantic_similarity') {
          const concepts = Object.keys(c.referencePhrases ?? {});
          if (!concepts.length) { setError(`Classifier "${c.id}": add at least one concept.`); return null; }
          for (const k of concepts) {
            if (!(c.referencePhrases![k]?.length)) {
              setError(`Classifier "${c.id}" concept "${k}": add at least one phrase.`); return null;
            }
          }
          if (c.defaultLabel && !concepts.includes(c.defaultLabel)) {
            setError(`Classifier "${c.id}": default label "${c.defaultLabel}" is not one of the concepts.`); return null;
          }
        } else if (c.defaultLabel && !(c.labels ?? []).includes(c.defaultLabel)) {
          setError(`Classifier "${c.id}": default label "${c.defaultLabel}" is not in the labels list.`); return null;
        }
      }
      if (!draft.rules?.length) { setError('Add at least one rule.'); return null; }
      const classifierIds = new Set((draft.classifiers ?? []).map(c => c.id));
      for (let ri = 0; ri < draft.rules.length; ri++) {
        const r = draft.rules[ri];
        const label = `Rule #${ri + 1}`;
        if (!r.routeTo) { setError(`${label}: select a target model.`); return null; }
        if (!draft.candidates.includes(r.routeTo)) {
          setError(`${label}: target model must be a candidate.`); return null;
        }
        if (!r.conditionTree) { setError(`${label}: add at least one condition.`); return null; }
        const treeErrors = validateRuleNode(r.conditionTree, classifierIds, draft.classifiers ?? []);
        if (treeErrors.length) { setError(`${label}: ${treeErrors[0]}`); return null; }
      }
    }
    // Block save when the policy loaded from the server had conditions this editor
    // cannot represent (e.g. metadata leaves). The user must acknowledge before
    // those conditions are permanently dropped.
    if ((draft.lossyRuleIds?.length ?? 0) > 0 && !lossyAcknowledged) {
      setError(
        `Rule${(draft.lossyRuleIds!.length > 1) ? 's' : ''} ${draft.lossyRuleIds!.map(id => `"${id}"`).join(', ')} ` +
        `contain conditions this editor cannot display (e.g. metadata). ` +
        `Saving will permanently remove them. Tick "I understand" below to proceed.`
      );
      return null;
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
          {draft.name.trim() && (
            <span className="settings-description" style={{ display: 'block', marginTop: 3, fontFamily: 'monospace', fontSize: '0.7rem' }}>
              ID: {makeCollectionId(draft.name)}
            </span>
          )}
        </div>

        <div className="form-section">
          <label className="form-label">
            Candidate Models *
            <span className="settings-description" style={{ marginLeft: 6 }}>- the LLMs that can answer requests</span>
          </label>
          {candidateOptions.length === 0 ? (
            <div className="collection-role-empty">No compatible models found. Pull or register LLM models first.</div>
          ) : (
            <>
              <ModelCheckboxList
                options={candidateOptions.map(({ id, info }): ModelOption => ({
                  id,
                  label: info.model_name ?? getModelDisplayName(id),
                  sublabel: info.downloaded === true ? 'local' : 'will download',
                  downloaded: info.downloaded === true,
                }))}
                selected={draft.candidates}
                onToggle={toggleCandidate}
              />
              {draft.candidates.length > 0 && (
                <span className="settings-description" style={{ display: 'block', marginTop: 4 }}>
                  {draft.candidates.map(id => displayName(id)).join(', ')}
                </span>
              )}
            </>
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
            <label className={`router-mode-option${draft.routingMode === 'llm' ? ' router-mode-option--selected' : ''}`}>
              <input type="radio" name="routingMode" value="llm" checked={draft.routingMode === 'llm'} onChange={() => patch({ routingMode: 'llm' })} />
              <strong>Natural Language</strong>
              <span className="settings-description">A small LLM reads your prompt and picks the best candidate.</span>
            </label>
            <label className={`router-mode-option${draft.routingMode === 'quick' ? ' router-mode-option--selected' : ''}`}>
              <input type="radio" name="routingMode" value="quick" checked={draft.routingMode === 'quick'}
                onChange={() => {
                  if (draft.routingMode === 'rules' && (draft.rules ?? []).length > 0) {
                    setConfirmQuick(true);
                  } else {
                    patch({ routingMode: 'quick', rules: [], classifiers: [] });
                  }
                }} />
              <strong>Quick Rules</strong>
              <span className="settings-description">Simple condition-based routing - no drag-and-drop needed.</span>
            </label>
            <label className={`router-mode-option${draft.routingMode === 'rules' ? ' router-mode-option--selected' : ''}`}>
              <input type="radio" name="routingMode" value="rules" checked={draft.routingMode === 'rules'}
                onChange={() => {
                  if (draft.routingMode === 'quick' && (draft.rules ?? []).length > 0) {
                    setConfirmAdvanced(true);
                  } else {
                    patch({ routingMode: 'rules', rules: [], classifiers: [] });
                  }
                }} />
              <strong>Advanced Rules</strong>
              <span className="settings-description">Visual boolean expression builder with gates and classifiers.</span>
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
              <PromptTextarea
                value={draft.routerPrompt ?? ''}
                onChange={v => patch({ routerPrompt: v })}
                candidates={draft.candidates}
                displayName={displayName}
                placeholder={DEFAULT_ROUTER_PROMPT}
              />
              <span className="settings-description" style={{ display: 'block', marginTop: 4 }}>
                Tell the router LLM which model to pick and when. Type <strong>@</strong> to insert a candidate model name.
              </span>
            </div>
          </>
        )}

        {draft.routingMode === 'quick' && (
          <div className="form-section">
            <QuickRulesEditor
              rules={draft.rules ?? []}
              candidates={draft.candidates}
              candidateOptions={candidateOptions}
              displayName={displayName}
              onChangeRules={rules => patch({ rules })}
              onChangeDraft={p => patch({ rules: p.rules, ...(p.defaultModel !== undefined ? { defaultModel: p.defaultModel } : {}) })}
              onExpandToAdvanced={() => patch({ routingMode: 'rules' })}
              ruleSeqRef={ruleSeqRef}
            />
          </div>
        )}

        {draft.routingMode === 'rules' && (
          <RouterPipelineCanvas
            draft={draft}
            candidateOptions={candidateOptions}
            embeddingOptions={embeddingOptions}
            classifierModelOptions={classifierModelOptions}
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
            error={error}
          />
        )}

      </div>

      {(draft.lossyRuleIds?.length ?? 0) > 0 && (
        <div className="router-panel-lossy-warning">
          <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="#f87171" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round" style={{ flexShrink: 0, marginTop: 1 }}>
            <path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/>
          </svg>
          <span>
            {draft.lossyRuleIds!.length === 1
              ? `Rule "${draft.lossyRuleIds![0]}" contains`
              : `Rules ${draft.lossyRuleIds!.map(id => `"${id}"`).join(', ')} contain`}
            {' '}conditions this editor cannot display (e.g. <code>metadata</code>).
            Saving will <strong style={{ color: '#f87171' }}>permanently remove</strong> them from the policy.
          </span>
          <label className="router-panel-lossy-ack">
            <input type="checkbox" checked={lossyAcknowledged} onChange={e => { setLossyAcknowledged(e.target.checked); setError(null); }} />
            I understand
          </label>
        </div>
      )}

      {confirmQuick && createPortal(
        <div className="confirm-overlay" onClick={() => setConfirmQuick(false)}>
          <div className="confirm-dialog" onClick={e => e.stopPropagation()}>
            <div className="confirm-title">Switch to Quick Rules?</div>
            <div className="confirm-body">
              Switching to Quick Rules starts with a blank slate - <strong style={{ color: '#f87171' }}>your Advanced Rules, classifiers, and conditions will be cleared.</strong>
              <br /><br />
              Quick Rules only supports simple flat conditions. Complex gates and classifiers cannot be migrated automatically.
            </div>
            <div className="confirm-actions">
              <button type="button" className="settings-reset-button" onClick={() => setConfirmQuick(false)}>Cancel</button>
              <button type="button" className="settings-save-button" onClick={() => { setConfirmQuick(false); patch({ routingMode: 'quick', rules: [], classifiers: [] }); }}>
                Clear and Switch
              </button>
            </div>
          </div>
        </div>,
        document.body,
      )}

      {confirmAdvanced && createPortal(
        <div className="confirm-overlay" onClick={() => setConfirmAdvanced(false)}>
          <div className="confirm-dialog" onClick={e => e.stopPropagation()}>
            <div className="confirm-title">Switch to Advanced Rules?</div>
            <div className="confirm-body">
              Switching to Advanced Rules starts with a blank canvas, <strong style={{ color: '#f87171' }}>your Quick Rules will be cleared.</strong>
              <br /><br />
              To keep editing your current rules in Advanced, use <strong style={{ color: '#d69e2e' }}>Expand in Advanced →</strong> instead.
            </div>
            <div className="confirm-actions">
              <button type="button" className="settings-reset-button" onClick={() => setConfirmAdvanced(false)}>Cancel</button>
              <button type="button" className="settings-save-button" onClick={() => { setConfirmAdvanced(false); patch({ routingMode: 'rules', rules: [], classifiers: [] }); }}>
                Clear and Switch
              </button>
            </div>
          </div>
        </div>,
        document.body,
      )}

      {previewJson !== null && (
        <div className="router-json-preview">
          <div className="router-json-preview-header">
            <span className="router-json-preview-title">Registration JSON (pull body)</span>
            <button type="button" className="router-json-preview-btn" title="Copy"
              onClick={() => navigator.clipboard.writeText(previewJson)}>
              <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                <rect x="9" y="9" width="13" height="13" rx="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/>
              </svg>
            </button>
            <button type="button" className="router-json-preview-btn" title="Download registration JSON (create or save first for a fully portable export)"
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
