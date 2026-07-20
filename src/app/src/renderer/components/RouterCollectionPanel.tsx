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
  makeCollectionId,
  routingToRouterCollectionDraft,
} from '../utils/customCollections';
import { isCollectionRecipe } from '../utils/recipeNames';
import { isFlatMatch, isLeaf, isOperatorNode, makeDefaultLeaf, SIGNAL_COLORS, validateRuleNode, type ConditionSignalType, type DragData, type RuleLeaf, type RuleNode } from '../utils/routerTree';
import RouterClassifiersSection, { type ClassifierTemplateKey } from './RouterClassifiersSection';
import RouterRuleCanvas from './RouterRuleCanvas';
import RouterToolbox from './RouterToolbox';
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

type QuickConditionType = 'min_chars' | 'max_chars' | 'keywords_any' | 'regex' | 'has_images' | 'has_tools' | 'classifier';

interface QCDef { type: QuickConditionType; label: string; shortLabel: string; placeholder?: string; inputType?: 'number' | 'text'; boolean?: true }

const QUICK_CONDITIONS: QCDef[] = [
  { type: 'min_chars',    label: 'Prompt is longer than…', shortLabel: 'Prompt is longer', inputType: 'number', placeholder: '500' },
  { type: 'max_chars',    label: 'Prompt is shorter than…', shortLabel: 'Prompt is shorter', inputType: 'number', placeholder: '200' },
  { type: 'keywords_any', label: 'Contains keywords',    shortLabel: 'Contains keywords', inputType: 'text',   placeholder: 'word1, word2' },
  { type: 'regex',        label: 'Matches regex',        shortLabel: 'Matches regex',     inputType: 'text',   placeholder: 'e.g. \\d{4}' },
  { type: 'has_images',   label: 'Has attached images',  shortLabel: 'Has images',        boolean: true },
  { type: 'has_tools',    label: 'Has tool calls',       shortLabel: 'Has tool calls',    boolean: true },
];

interface QuickChip { conditionType: QuickConditionType; value: string; not: boolean; classifierId?: string; label?: string }

const QUICK_CONDITION_TYPES = new Set<string>(QUICK_CONDITIONS.map(c => c.type));

// Labels a classifier can emit, used to populate the flat-form label picker.
function classifierLabels(clf: RouterClassifier): string[] {
  if (clf.type === 'semantic_similarity') return Object.keys(clf.referencePhrases ?? {});
  return (clf.labels ?? []).filter(Boolean);
}

function leafToChip(leaf: RuleLeaf): QuickChip | null {
  if (leaf.signalType === 'classifier') {
    if (!leaf.classifierId) return null;
    return { conditionType: 'classifier', value: '', not: leaf.not ?? false, classifierId: leaf.classifierId, label: leaf.label };
  }
  if (!QUICK_CONDITION_TYPES.has(leaf.signalType)) return null;
  return { conditionType: leaf.signalType as QuickConditionType, value: leaf.signalValue !== undefined ? String(leaf.signalValue) : '', not: leaf.not ?? false };
}

function getRowChips(rule: RouterRule): { op: 'AND' | 'OR'; chips: QuickChip[] } {
  const tree = rule.conditionTree;
  if (!tree) return { op: 'AND', chips: [] };
  if ('signalType' in tree) {
    const chip = leafToChip(tree);
    return { op: 'AND', chips: chip ? [chip] : [] };
  }
  if ('operator' in tree && (tree.operator === 'AND' || tree.operator === 'OR')) {
    const chips = tree.conditions
      .filter(isLeaf)
      .map(leafToChip)
      .filter((c): c is QuickChip => c !== null);
    return { op: tree.operator, chips };
  }
  return { op: 'AND', chips: [] };
}

function chipsToRouterRule(rule: RouterRule, op: 'AND' | 'OR', chips: QuickChip[]): RouterRule {
  if (chips.length === 0) return { ...rule, conditionTree: null };
  const leaves = chips.map<RuleLeaf>(chip => {
    if (chip.conditionType === 'classifier') {
      const leaf: RuleLeaf = { signalType: 'classifier', classifierId: chip.classifierId ?? '' };
      if (chip.label) leaf.label = chip.label;
      if (chip.not) leaf.not = true;
      return leaf;
    }
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
    if (c.conditionType === 'classifier') {
      const base = `classifier ${c.classifierId ?? '?'}`;
      return (c.not ? 'not ' : '') + (c.label ? `${base} is ${c.label}` : base);
    }
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
  classifiers: RouterClassifier[];
  displayName: (id: string) => string;
  dragIndex: number | null;
  onDragStart: (idx: number) => void;
  onDragOver: (idx: number) => void;
  onDrop: () => void;
  onChange: (rule: RouterRule) => void;
  onRemove: () => void;
  onEditAsGraph: () => void;
}

const QuickRuleRow: React.FC<QuickRuleRowProps> = ({
  rule, index, candidates, classifiers, displayName, dragIndex,
  onDragStart, onDragOver, onDrop, onChange, onRemove, onEditAsGraph,
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
  const addClassifierChip = (classifierId: string) => {
    const next = [...chips, { conditionType: 'classifier' as QuickConditionType, value: '', not: false, classifierId, label: '' }];
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
            const opSelector = ci > 0 && (
              <div className="quick-rule-op-group">
                <button type="button" className={`quick-op-btn${op === 'AND' ? ' active' : ''}`} onClick={() => setOp('AND')}>AND</button>
                <button type="button" className={`quick-op-btn${op === 'OR' ? ' active' : ''}`} onClick={() => setOp('OR')}>OR</button>
              </div>
            );
            if (chip.conditionType === 'classifier') {
              const clf = classifiers.find(c => c.id === chip.classifierId);
              const labelOptions = clf ? classifierLabels(clf) : [];
              return (
                <React.Fragment key={ci}>
                  {opSelector}
                  <div className={`quick-chip${chip.not ? ' quick-chip--not' : ''}`} style={{ '--chip-color': SIGNAL_COLORS.classifier } as React.CSSProperties}>
                    <button type="button" className="quick-chip-not" title={chip.not ? 'Remove NOT' : 'Negate (NOT)'} onClick={() => updateChip(ci, { not: !chip.not })}>
                      {chip.not ? '¬' : '+'}
                    </button>
                    <span className="quick-chip-label">{chip.classifierId || '(classifier)'} is</span>
                    {labelOptions.length > 0 ? (
                      <select
                        className="form-input form-select quick-chip-input"
                        value={chip.label ?? ''}
                        onClick={e => e.stopPropagation()}
                        onChange={e => updateChip(ci, { label: e.target.value })}
                      >
                        <option value="">(any)</option>
                        {labelOptions.map(l => <option key={l} value={l}>{l}</option>)}
                      </select>
                    ) : (
                      <input
                        className="quick-chip-input"
                        type="text"
                        value={chip.label ?? ''}
                        placeholder="label (any)"
                        onClick={e => e.stopPropagation()}
                        onChange={e => updateChip(ci, { label: e.target.value })}
                      />
                    )}
                    <button type="button" className="quick-chip-remove" onClick={() => removeChip(ci)}>×</button>
                  </div>
                </React.Fragment>
              );
            }
            const def = QUICK_CONDITIONS.find(d => d.type === chip.conditionType)!;
            return (
              <React.Fragment key={ci}>
                {opSelector}
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
                  {classifiers.length > 0 && <div className="quick-add-condition-divider" />}
                  {classifiers.map(c => (
                    <button key={c.id} type="button" className="quick-add-condition-item" onClick={() => addClassifierChip(c.id)}>Classifier: {c.id}</button>
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

      <button type="button" className="pipeline-icon-btn quick-rule-graph" title="Edit as graph (mix AND/OR, add classifiers)" onClick={onEditAsGraph}>
        <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"><circle cx="6" cy="6" r="3"/><circle cx="6" cy="18" r="3"/><circle cx="18" cy="12" r="3"/><path d="M9 6h6M9 18h6M15 12h.01"/><path d="M8.5 7.5 15 11M8.5 16.5 15 13"/></svg>
      </button>
      <button type="button" className="pipeline-icon-btn quick-rule-remove" title="Remove rule" onClick={onRemove}>
        <svg width="10" height="10" viewBox="0 0 12 12" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round"><path d="M1 1 L11 11 M11 1 L1 11"/></svg>
      </button>
    </div>
  );
};


// A rule that has escalated to the graph canvas: it either mixes AND/OR,
// nests gates, or references a classifier — things the flat form can't hold.
interface RuleGraphRowProps {
  rule: RouterRule;
  index: number;
  candidates: string[];
  classifiers: RouterClassifier[];
  displayName: (id: string) => string;
  canCollapse: boolean;
  dragIndex: number | null;
  onDragStart: (idx: number) => void;
  onDragOver: (idx: number) => void;
  onDrop: () => void;
  onChange: (rule: RouterRule) => void;
  onRemove: () => void;
  onCollapse: () => void;
}

const RuleGraphRow: React.FC<RuleGraphRowProps> = ({
  rule, index, candidates, classifiers, displayName, canCollapse, dragIndex,
  onDragStart, onDragOver, onDrop, onChange, onRemove, onCollapse,
}) => {
  const [toolboxCollapsed, setToolboxCollapsed] = useState(false);
  const chipClickRef = useRef<((data: DragData) => void) | null>(null);
  const isDragging = dragIndex === index;

  return (
    <div
      className={`rule-graph-row${isDragging ? ' rule-graph-row--dragging' : ''}`}
      onDragOver={e => { e.preventDefault(); onDragOver(index); }}
      onDrop={e => { e.preventDefault(); onDrop(); }}
    >
      <div className="rule-graph-row-header">
        <span className="quick-rule-handle" title="Drag to reorder"
          draggable onDragStart={() => onDragStart(index)}>⠿</span>
        <span className="quick-rule-index">{index + 1}</span>
        <input
          type="text"
          className="form-input rpc-rule-id-input"
          defaultValue={rule.id}
          onBlur={e => onChange({ ...rule, id: e.target.value.trim().toLowerCase().replace(/[^a-z0-9-]+/g, '-') || rule.id })}
          title="Rule ID"
        />
        <span className="quick-rule-arrow">→</span>
        <select className="form-input form-select quick-rule-model" value={rule.routeTo} onChange={e => onChange({ ...rule, routeTo: e.target.value })}>
          <option value="">Model…</option>
          {candidates.map(id => <option key={id} value={id}>{displayName(id)}</option>)}
        </select>
        {canCollapse && (
          <button type="button" className="settings-reset-button rule-graph-collapse" title="This rule is simple enough for the form editor" onClick={onCollapse}>
            Collapse to simple
          </button>
        )}
        <button type="button" className="pipeline-icon-btn quick-rule-remove" title="Remove rule" onClick={onRemove}>
          <svg width="10" height="10" viewBox="0 0 12 12" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round"><path d="M1 1 L11 11 M11 1 L1 11"/></svg>
        </button>
      </div>
      <div className="rule-graph-row-body rpc-canvas-body">
        <RouterRuleCanvas
          tree={rule.conditionTree ?? null}
          classifiers={classifiers}
          onChange={(tree: RuleNode | null) => onChange({ ...rule, conditionTree: tree })}
          onChipClick={handler => { chipClickRef.current = handler; }}
        />
        <RouterToolbox
          classifiers={classifiers}
          collapsed={toolboxCollapsed}
          onToggle={() => setToolboxCollapsed(v => !v)}
          onChipClick={data => chipClickRef.current?.(data)}
        />
      </div>
    </div>
  );
};


interface RulesEditorProps {
  rules: RouterRule[];
  candidates: string[];
  candidateOptions: CandidateOptionWithInfo[];
  classifiers: RouterClassifier[];
  displayName: (id: string) => string;
  onChangeRules: (rules: RouterRule[]) => void;
  onChangeDraft: (p: { rules: RouterRule[]; defaultModel?: string }) => void;
  ruleSeqRef: React.MutableRefObject<number>;
}

// Unified rules editor. Each rule renders in the simple form while its
// condition stays flat (a lone signal or a single-level AND/OR); the moment it
// mixes operators, nests, or references a classifier it escalates — on its own,
// independently of the other rules — to the inline graph canvas.
const RulesEditor: React.FC<RulesEditorProps> = ({
  rules, candidates, candidateOptions, classifiers, displayName,
  onChangeRules, onChangeDraft, ruleSeqRef,
}) => {
  const [dragIndex, setDragIndex] = useState<number | null>(null);
  const [dragOver, setDragOver] = useState<number | null>(null);
  const [activePreset, setActivePreset] = useState<PresetKey | 'blank' | null>(null);
  // Flat rules the user chose to hand-author as a graph. A rule that is not
  // flat is always a graph regardless of this set.
  const [forcedGraph, setForcedGraph] = useState<Set<string>>(new Set());

  const forceGraph = (id: string) => setForcedGraph(prev => new Set(prev).add(id));
  const unforceGraph = (id: string) => setForcedGraph(prev => { const n = new Set(prev); n.delete(id); return n; });

  const addRow = () => {
    const existing = new Set(rules.map(r => r.id));
    let n = ruleSeqRef.current + 1;
    while (existing.has(`rule-${n}`)) n++;
    ruleSeqRef.current = n;
    onChangeRules([...rules, { id: `rule-${n}`, routeTo: '', conditionTree: makeDefaultLeaf('min_chars') }]);
  };

  const applyPreset = (key: PresetKey) => {
    const { rules: newRules } = buildPresetRules(key, candidateOptions, ruleSeqRef);
    onChangeDraft({ rules: newRules });
    setActivePreset(key);
    setForcedGraph(new Set());
  };

  const handleDrop = () => {
    if (dragIndex === null || dragOver === null || dragIndex === dragOver) { setDragIndex(null); setDragOver(null); return; }
    const next = [...rules];
    const [moved] = next.splice(dragIndex, 1);
    next.splice(dragOver, 0, moved);
    onChangeRules(next);
    setDragIndex(null); setDragOver(null);
  };

  const changeRuleAt = (idx: number, updated: RouterRule) => {
    const next = [...rules]; next[idx] = updated; onChangeRules(next);
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
        <button type="button" className={`quick-preset-btn quick-preset-btn--blank${activePreset === 'blank' ? ' quick-preset-btn--active' : ''}`} onClick={() => { onChangeDraft({ rules: [] }); setActivePreset('blank'); setForcedGraph(new Set()); }}>
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

      {rules.map((rule, idx) => {
        const flat = isFlatMatch(rule.conditionTree);
        const showGraph = !flat || forcedGraph.has(rule.id);
        return showGraph ? (
          <RuleGraphRow
            key={rule.id}
            rule={rule}
            index={idx}
            candidates={candidates}
            classifiers={classifiers}
            displayName={displayName}
            canCollapse={flat}
            dragIndex={dragIndex}
            onDragStart={i => { setDragIndex(i); setDragOver(i); }}
            onDragOver={i => setDragOver(i)}
            onDrop={handleDrop}
            onChange={updated => changeRuleAt(idx, updated)}
            onRemove={() => { unforceGraph(rule.id); onChangeRules(rules.filter((_, i) => i !== idx)); }}
            onCollapse={() => unforceGraph(rule.id)}
          />
        ) : (
          <QuickRuleRow
            key={rule.id}
            rule={rule}
            index={idx}
            candidates={candidates}
            classifiers={classifiers}
            displayName={displayName}
            dragIndex={dragIndex}
            onDragStart={i => { setDragIndex(i); setDragOver(i); }}
            onDragOver={i => setDragOver(i)}
            onDrop={handleDrop}
            onChange={updated => changeRuleAt(idx, updated)}
            onRemove={() => onChangeRules(rules.filter((_, i) => i !== idx))}
            onEditAsGraph={() => forceGraph(rule.id)}
          />
        );
      })}

      <div className="quick-rules-footer">
        <button type="button" className="settings-reset-button" onClick={addRow}>+ Add rule</button>
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
  const ruleSeqRef = useRef(0);
  const clfSeqRef = useRef(0);

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

  const uniqueClassifierId = (base: string) => {
    const existing = new Set((draft.classifiers ?? []).map(c => c.id));
    if (!existing.has(base)) return base;
    let n = 2;
    while (existing.has(`${base}-${n}`)) n++;
    return `${base}-${n}`;
  };

  const addClassifierTemplate = (key: ClassifierTemplateKey) => {
    const pickLlm = () =>
      candidateOptions.find(o => o.info.downloaded)?.id ?? candidateOptions[0]?.id ?? 'Qwen3-0.6B-GGUF';
    const pickEmbed = () =>
      embeddingOptions.find(o => o.info.downloaded)?.id ?? embeddingOptions[0]?.id ?? 'nomic-embed-text-v1-GGUF';

    const clf: RouterClassifier = key === 'pii'
      ? {
          id: uniqueClassifierId('pii'),
          type: 'llm',
          model: pickLlm(),
          prompt: [
            'You are a privacy filter. Decide whether the user\'s message contains personally',
            'identifiable information (real names, emails, phone numbers, street addresses,',
            'government IDs, or credit-card numbers).',
            'Reply with ONLY one word: PII if it does, or CLEAN if it does not.',
          ].join('\n'),
          onError: 'match_false',
        }
      : {
          id: uniqueClassifierId('topic'),
          type: 'semantic_similarity',
          model: pickEmbed(),
          referencePhrases: {
            code: ['write a function', 'fix this bug', 'refactor this code', 'what does this error mean'],
            creative: ['write a short story', 'compose a poem', 'brainstorm ideas', 'suggest a tagline'],
            general: ['what is', 'explain how', 'summarize this', 'help me understand'],
          },
          onError: 'match_false',
        };

    setDraft(prev => ({ ...prev, classifiers: [...(prev.classifiers ?? []), clf] }));
    setError(null);
    setPreviewJson(null);
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
      for (let ri = 0; ri < draft.rules.length; ri++) {
        const r = draft.rules[ri];
        const label = `Rule #${ri + 1}`;
        if (!r.routeTo) { setError(`${label}: select a target model.`); return null; }
        if (!draft.candidates.includes(r.routeTo)) {
          setError(`${label}: target model must be a candidate.`); return null;
        }
        if (!r.conditionTree) { setError(`${label}: add at least one condition.`); return null; }
        const treeErrors = validateRuleNode(r.conditionTree, classifierIds);
        if (treeErrors.length) { setError(`${label}: ${treeErrors[0]}`); return null; }
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
                options={candidateOptions.map(({ id, info }) => ({
                  id,
                  label: info.model_name ?? getModelDisplayName(id),
                  sublabel: info.downloaded === true ? 'local' : 'will download',
                  downloaded: info.downloaded === true,
                }) satisfies ModelOption)}
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
              <strong>NL Router</strong>
              <span className="settings-description">A small LLM reads your prompt and picks the best candidate.</span>
            </label>
            <label className={`router-mode-option${draft.routingMode === 'rules' ? ' router-mode-option--selected' : ''}`}>
              <input type="radio" name="routingMode" value="rules" checked={draft.routingMode === 'rules'} onChange={() => patch({ routingMode: 'rules' })} />
              <strong>Rule-based Router</strong>
              <span className="settings-description">Deterministic condition rules. Each rule stays a simple form until it needs a graph.</span>
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

        {draft.routingMode === 'rules' && (
          <>
            <RouterClassifiersSection
              classifiers={draft.classifiers ?? []}
              candidateOptions={candidateOptions}
              embeddingOptions={embeddingOptions}
              displayNameWithStatus={displayNameWithStatus}
              onAddClassifier={addClassifier}
              onAddTemplate={addClassifierTemplate}
              onPatchClassifier={patchClassifier}
              onRemoveClassifier={removeClassifier}
            />
            <div className="form-section">
              <RulesEditor
                rules={draft.rules ?? []}
                candidates={draft.candidates}
                candidateOptions={candidateOptions}
                classifiers={draft.classifiers ?? []}
                displayName={displayName}
                onChangeRules={rules => patch({ rules })}
                onChangeDraft={p => patch({ rules: p.rules, ...(p.defaultModel !== undefined ? { defaultModel: p.defaultModel } : {}) })}
                ruleSeqRef={ruleSeqRef}
              />
            </div>
          </>
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
