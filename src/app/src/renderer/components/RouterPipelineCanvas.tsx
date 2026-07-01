import React, { useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import {
  RouterClassifier,
  RouterCollectionDraft,
  RouterCondition,
  RouterConditionGroup,
  RouterRule,
  RouterRuleOperator,
} from '../utils/customCollections';

interface CandidateOption {
  id: string;
  info: { model_name?: string; downloaded?: boolean };
}

interface RouterPipelineCanvasProps {
  draft: RouterCollectionDraft;
  candidateOptions: CandidateOption[];
  embeddingOptions: CandidateOption[];
  displayName: (id: string) => string;
  displayNameWithStatus: (id: string) => string;
  onPatchClassifier: (id: string, p: Partial<RouterClassifier>) => void;
  onAddClassifier: () => void;
  onRemoveClassifier: (id: string) => void;
  onPatchRule: (id: string, p: Partial<RouterRule>) => void;
  onAddRule: () => void;
  onRemoveRule: (id: string) => void;
  highlightedClassifierId: string | null;
  onHighlightClassifier: (id: string | null) => void;
}

type ConditionType = RouterCondition['type'];

const CONDITION_LABELS: Record<ConditionType, string> = {
  keywords_any: 'Keywords (any)',
  keywords_all: 'Keywords (all)',
  regex: 'Regex',
  min_chars: 'Min chars',
  max_chars: 'Max chars',
  has_tools: 'Has tools',
  has_images: 'Has images',
  classifier: 'Classifier',
};

let _idSeq = 0;
const uid = () => `id-${++_idSeq}`;

// --------------------------------------------------------------------------
// Classifier card (swimlane)
// --------------------------------------------------------------------------

const slugify = (s: string) =>
  s.trim().toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '') || 'clf';

interface ClassifierCardProps {
  clf: RouterClassifier;
  isHighlighted: boolean;
  embeddingOptions: CandidateOption[];
  candidateOptions: CandidateOption[];
  displayNameWithStatus: (id: string) => string;
  onPatch: (p: Partial<RouterClassifier>) => void;
  onRemove: () => void;
}

const ClassifierCard: React.FC<ClassifierCardProps> = ({
  clf, isHighlighted, embeddingOptions, candidateOptions, displayNameWithStatus, onPatch, onRemove,
}) => {
  const modelOptions = clf.type === 'semantic_similarity' ? embeddingOptions : candidateOptions;
  return (
    <div className={`pipeline-clf-card${isHighlighted ? ' pipeline-clf-card--highlighted' : ''}`}>
      <div className="pipeline-clf-card-header">
        <span className="pipeline-clf-id">{clf.id || '(unnamed)'}</span>
        <button type="button" className="pipeline-icon-btn" title="Remove" onClick={onRemove}>
          <svg width="10" height="10" viewBox="0 0 12 12" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round"><path d="M1 1 L11 11 M11 1 L1 11"/></svg>
        </button>
      </div>
      <div className="pipeline-clf-fields">
        <input type="text" className="form-input pipeline-clf-input" value={clf.id}
          onChange={(e) => onPatch({ id: slugify(e.target.value) })} placeholder="id" />
        <select className="form-input form-select pipeline-clf-select" value={clf.type}
          onChange={(e) => onPatch({ type: e.target.value as RouterClassifier['type'], labels: [], defaultLabel: '', referencePhrases: {}, prompt: undefined })}>
          <option value="classifier">Classifier</option>
          <option value="semantic_similarity">Semantic Similarity</option>
          <option value="llm">LLM</option>
        </select>
        <select className="form-input form-select pipeline-clf-select" value={clf.model}
          onChange={(e) => onPatch({ model: e.target.value })}>
          <option value="">Model…</option>
          {modelOptions.map(({ id }) => <option key={id} value={id}>{displayNameWithStatus(id)}</option>)}
        </select>
      </div>
      {clf.type === 'llm' && (
        <textarea className="form-input pipeline-clf-textarea" rows={2}
          defaultValue={clf.prompt ?? ''}
          onBlur={(e) => onPatch({ prompt: e.target.value || undefined })}
          placeholder="Routing prompt…" />
      )}
      {clf.type === 'classifier' && (
        <textarea className="form-input pipeline-clf-textarea" rows={2}
          defaultValue={(clf.labels ?? []).join('\n')}
          onBlur={(e) => {
            const labels = e.target.value.split('\n').map((s) => s.trim()).filter(Boolean);
            const defaultLabel = labels.includes(clf.defaultLabel ?? '') ? clf.defaultLabel : '';
            onPatch({ labels, defaultLabel });
          }}
          placeholder={'Labels (one per line)\nPII\nNO_PII'} />
      )}
      {clf.type === 'semantic_similarity' && (
        <div className="pipeline-clf-concepts">
          {Object.entries(clf.referencePhrases ?? {}).map(([concept, phrases]) => (
            <div key={concept} className="pipeline-concept-row">
              <input type="text" className="form-input pipeline-concept-name" defaultValue={concept}
                onBlur={(e) => {
                  const newName = e.target.value.trim();
                  if (!newName || newName === concept) return;
                  const existing = { ...clf.referencePhrases };
                  const phrs = existing[concept]; delete existing[concept]; existing[newName] = phrs;
                  onPatch({ referencePhrases: existing });
                }} placeholder="concept" />
              <textarea className="form-input pipeline-concept-phrases" rows={2}
                defaultValue={phrases.join('\n')}
                onBlur={(e) => {
                  const updated = e.target.value.split('\n').map((s) => s.trim()).filter(Boolean);
                  onPatch({ referencePhrases: { ...clf.referencePhrases, [concept]: updated } });
                }} placeholder="phrases (one per line)" />
              <button type="button" className="pipeline-icon-btn" title="Remove concept"
                onClick={() => { const e = { ...clf.referencePhrases }; delete e[concept]; onPatch({ referencePhrases: e }); }}>×</button>
            </div>
          ))}
          <button type="button" className="settings-reset-button pipeline-add-concept-btn"
            onClick={() => {
              const e = clf.referencePhrases ?? {};
              onPatch({ referencePhrases: { ...e, [`concept-${Object.keys(e).length + 1}`]: [] } });
            }}>+ Concept</button>
        </div>
      )}
    </div>
  );
};

// --------------------------------------------------------------------------
// Condition picker portal
// --------------------------------------------------------------------------

interface ConditionPickerProps {
  classifiers: RouterClassifier[];
  anchorRef: React.RefObject<HTMLButtonElement | null>;
  onPick: (type: ConditionType, classifierId?: string) => void;
  onClose: () => void;
}

const ConditionPicker: React.FC<ConditionPickerProps> = ({ classifiers, anchorRef, onPick, onClose }) => {
  const rect = anchorRef.current?.getBoundingClientRect();
  const style: React.CSSProperties = rect
    ? { position: 'fixed', top: rect.bottom + 4, left: rect.left, zIndex: 10200 }
    : { display: 'none' };
  const base: ConditionType[] = ['keywords_any', 'keywords_all', 'regex', 'min_chars', 'max_chars', 'has_tools', 'has_images'];
  return createPortal(
    <>
      <div style={{ position: 'fixed', inset: 0, zIndex: 10199 }} onMouseDown={onClose} />
      <div className="pipeline-picker" style={style} onMouseDown={(e) => e.stopPropagation()}>
        {base.map((t) => (
          <button key={t} type="button" className="pipeline-picker-item"
            onMouseDown={() => { onPick(t); onClose(); }}>
            {CONDITION_LABELS[t]}
          </button>
        ))}
        {classifiers.length > 0 && (
          <>
            <div className="pipeline-picker-divider" />
            {classifiers.map((c) => (
              <button key={c.id} type="button" className="pipeline-picker-item pipeline-picker-item--clf"
                onMouseDown={() => { onPick('classifier', c.id); onClose(); }}>
                Classifier: {c.id}
              </button>
            ))}
          </>
        )}
      </div>
    </>,
    document.body,
  );
};

// --------------------------------------------------------------------------
// Condition chip
// --------------------------------------------------------------------------

interface ConditionChipProps {
  cond: RouterCondition;
  classifiers: RouterClassifier[];
  onPatch: (p: Partial<RouterCondition>) => void;
  onRemove: () => void;
  onHighlightClassifier: (id: string | null) => void;
}

const ConditionChip: React.FC<ConditionChipProps> = ({
  cond, classifiers, onPatch, onRemove, onHighlightClassifier,
}) => {
  const [expanded, setExpanded] = useState(false);

  const summary = (() => {
    if (cond.type === 'keywords_any' || cond.type === 'keywords_all') {
      const kws = cond.keywords ?? [];
      return kws.length ? kws.slice(0, 3).join(', ') + (kws.length > 3 ? '…' : '') : '(empty)';
    }
    if (cond.type === 'regex') return cond.pattern || '(empty)';
    if (cond.type === 'min_chars') return `≥ ${cond.value ?? '?'}`;
    if (cond.type === 'max_chars') return `≤ ${cond.value ?? '?'}`;
    if (cond.type === 'has_tools') return 'request carries tools[]';
    if (cond.type === 'has_images') return 'request carries images';
    if (cond.type === 'classifier') {
      const score = cond.minScore ?? 0.5;
      return `${cond.classifierId ?? '?'} ≥ ${score}`;
    }
    return '';
  })();

  const renderEditor = () => {
    if (cond.type === 'keywords_any' || cond.type === 'keywords_all') {
      return (
        <input type="text" className="form-input pipeline-chip-input"
          defaultValue={(cond.keywords ?? []).join(', ')}
          onBlur={(e) => onPatch({ keywords: e.target.value.split(',').map((s) => s.trim()).filter(Boolean) })}
          placeholder="word1, word2" autoFocus />
      );
    }
    if (cond.type === 'regex') {
      return (
        <input type="text" className="form-input pipeline-chip-input"
          value={cond.pattern ?? ''}
          onChange={(e) => onPatch({ pattern: e.target.value })}
          placeholder="e.g. ```[a-z]*" autoFocus />
      );
    }
    if (cond.type === 'min_chars' || cond.type === 'max_chars') {
      return (
        <input type="number" className="form-input pipeline-chip-input" min={0}
          value={cond.value ?? ''}
          onChange={(e) => onPatch({ value: e.target.value ? parseInt(e.target.value) : undefined })}
          placeholder="chars" autoFocus />
      );
    }
    if (cond.type === 'has_tools' || cond.type === 'has_images') {
      return <span className="pipeline-chip-static">{summary}</span>;
    }
    if (cond.type === 'classifier') {
      const activeCLF = classifiers.find((c) => c.id === cond.classifierId);
      const clfLabels = activeCLF?.type === 'semantic_similarity'
        ? Object.keys(activeCLF.referencePhrases ?? {})
        : activeCLF?.labels ?? [];
      return (
        <div className="pipeline-chip-clf-editor">
          <select className="form-input form-select pipeline-chip-select"
            value={cond.classifierId ?? ''}
            onChange={(e) => {
              onPatch({ classifierId: e.target.value || undefined });
              onHighlightClassifier(e.target.value || null);
            }} autoFocus>
            <option value="">Select classifier…</option>
            {classifiers.map((c) => <option key={c.id} value={c.id}>{c.id}</option>)}
          </select>
          {cond.classifierId && clfLabels.length > 0 && (
            <select className="form-input form-select pipeline-chip-select"
              value={cond.label ?? ''}
              onChange={(e) => onPatch({ label: e.target.value || undefined })}>
              <option value="">default label</option>
              {clfLabels.map((l) => <option key={l} value={l}>{l}</option>)}
            </select>
          )}
          {cond.classifierId && (
            <div className="pipeline-chip-score-row">
              <input type="number" className="form-input pipeline-chip-score"
                min={0} max={1} step={0.05} value={cond.minScore ?? 0.5}
                onChange={(e) => onPatch({ minScore: parseFloat(e.target.value) })}
                title="Min score" />
              <span className="pipeline-chip-score-sep">–</span>
              <input type="number" className="form-input pipeline-chip-score"
                min={0} max={1} step={0.05} value={cond.maxScore ?? ''}
                onChange={(e) => onPatch({ maxScore: e.target.value ? parseFloat(e.target.value) : undefined })}
                placeholder="max" title="Max score (optional)" />
            </div>
          )}
        </div>
      );
    }
    return null;
  };

  return (
    <div
      className={`pipeline-condition-chip${cond.not ? ' pipeline-condition-chip--negated' : ''}`}
      onMouseEnter={() => cond.type === 'classifier' && cond.classifierId ? onHighlightClassifier(cond.classifierId) : undefined}
      onMouseLeave={() => cond.type === 'classifier' ? onHighlightClassifier(null) : undefined}
    >
      <div className="pipeline-chip-header">
        <span className="pipeline-chip-label">
          {cond.not && <span className="pipeline-chip-not">¬</span>}
          {CONDITION_LABELS[cond.type]}
        </span>
        <div className="pipeline-chip-actions">
          <button type="button"
            className={`pipeline-chip-not-btn${cond.not ? ' active' : ''}`}
            title={cond.not ? 'Remove NOT' : 'Negate condition'}
            onClick={() => onPatch({ not: cond.not ? undefined : true })}>NOT</button>
          <button type="button" className="pipeline-icon-btn" title="Expand / collapse"
            onClick={() => setExpanded((v) => !v)}>
            <svg width="9" height="9" viewBox="0 0 10 10" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round">
              {expanded ? <path d="M2 7 L5 4 L8 7"/> : <path d="M2 3 L5 6 L8 3"/>}
            </svg>
          </button>
          <button type="button" className="pipeline-icon-btn" title="Remove condition" onClick={onRemove}>
            <svg width="9" height="9" viewBox="0 0 10 10" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round">
              <path d="M1 1 L9 9 M9 1 L1 9"/>
            </svg>
          </button>
        </div>
      </div>
      <div className="pipeline-chip-summary">{summary}</div>
      {expanded && <div className="pipeline-chip-editor">{renderEditor()}</div>}
    </div>
  );
};

// --------------------------------------------------------------------------
// Condition group
// --------------------------------------------------------------------------

interface ConditionGroupProps {
  group: RouterConditionGroup;
  isOnly: boolean;
  classifiers: RouterClassifier[];
  onPatchCond: (condId: string, p: Partial<RouterCondition>) => void;
  onRemoveCond: (condId: string) => void;
  onAddCond: (type: ConditionType, classifierId?: string) => void;
  onPatchGroup: (p: Partial<RouterConditionGroup>) => void;
  onRemoveGroup: () => void;
  onHighlightClassifier: (id: string | null) => void;
}

const ConditionGroup: React.FC<ConditionGroupProps> = ({
  group, isOnly, classifiers,
  onPatchCond, onRemoveCond, onAddCond, onPatchGroup, onRemoveGroup, onHighlightClassifier,
}) => {
  const [showPicker, setShowPicker] = useState(false);
  const addBtnRef = useRef<HTMLButtonElement>(null);

  return (
    <div className="pipeline-group">
      {/* Group header */}
      <div className="pipeline-group-header">
        <select className="form-input form-select pipeline-operator-select"
          value={group.operator}
          onChange={(e) => onPatchGroup({ operator: e.target.value as RouterRuleOperator })}>
          <option value="any">OR (any match)</option>
          <option value="all">AND (all match)</option>
        </select>
        <button type="button" className="settings-reset-button pipeline-remove-group-btn"
          disabled={isOnly} title={isOnly ? 'Cannot remove the only group' : 'Remove this group'}
          onClick={onRemoveGroup}>
          Remove group
        </button>
      </div>

      {/* Conditions */}
      <div className="pipeline-chips-area">
        {group.conditions.length === 0 && (
          <div className="collection-role-empty pipeline-empty-group">
            Click "+ Add Condition" to add a condition.
          </div>
        )}
        {group.conditions.map((cond) => (
          <ConditionChip
            key={cond.id}
            cond={cond}
            classifiers={classifiers}
            onPatch={(p) => onPatchCond(cond.id, p)}
            onRemove={() => onRemoveCond(cond.id)}
            onHighlightClassifier={onHighlightClassifier}
          />
        ))}
      </div>

      {/* Add condition */}
      <div className="pipeline-add-condition-row">
        <button ref={addBtnRef} type="button" className="settings-reset-button pipeline-add-btn"
          onClick={() => setShowPicker((v) => !v)}>
          + Add Condition
        </button>
        {showPicker && (
          <ConditionPicker
            anchorRef={addBtnRef}
            classifiers={classifiers}
            onPick={(type, classifierId) => { onAddCond(type, classifierId); }}
            onClose={() => setShowPicker(false)}
          />
        )}
      </div>
    </div>
  );
};

// --------------------------------------------------------------------------
// Rule card
// --------------------------------------------------------------------------

interface RuleCardProps {
  rule: RouterRule;
  index: number;
  candidates: string[];
  classifiers: RouterClassifier[];
  displayName: (id: string) => string;
  onPatch: (p: Partial<RouterRule>) => void;
  onRemove: () => void;
  onHighlightClassifier: (id: string | null) => void;
}

const RuleCard: React.FC<RuleCardProps> = ({
  rule, index, candidates, classifiers, displayName, onPatch, onRemove, onHighlightClassifier,
}) => {
  const dragFromRef = useRef<number | null>(null);
  const [dragTargetIndex, setDragTargetIndex] = useState<number | null>(null);

  const patchGroups = (groups: RouterConditionGroup[]) => onPatch({ groups });

  const patchGroup = (groupId: string, p: Partial<RouterConditionGroup>) =>
    patchGroups(rule.groups.map((g) => g.id === groupId ? { ...g, ...p } : g));

  const removeGroup = (groupId: string) => {
    const next = rule.groups.filter((g) => g.id !== groupId);
    patchGroups(next.map((g, i) => i === 0 ? { ...g, joinOperator: undefined } : g));
  };

  const addGroup = () =>
    patchGroups([...rule.groups, { id: uid(), operator: 'any', conditions: [], joinOperator: 'any' }]);

  const patchCond = (groupId: string, condId: string, p: Partial<RouterCondition>) =>
    patchGroups(rule.groups.map((g) =>
      g.id !== groupId ? g : { ...g, conditions: g.conditions.map((c) => c.id === condId ? { ...c, ...p } : c) }
    ));

  const removeCond = (groupId: string, condId: string) =>
    patchGroups(rule.groups.map((g) =>
      g.id !== groupId ? g : { ...g, conditions: g.conditions.filter((c) => c.id !== condId) }
    ));

  const addCond = (groupId: string, type: ConditionType, classifierId?: string) => {
    const newCond: RouterCondition = { id: uid(), type };
    if (type === 'keywords_any' || type === 'keywords_all') newCond.keywords = [];
    else if (type === 'regex') newCond.pattern = '';
    else if (type === 'min_chars') newCond.value = 500;
    else if (type === 'max_chars') newCond.value = 2000;
    else if (type === 'classifier') { newCond.classifierId = classifierId ?? classifiers[0]?.id; newCond.minScore = 0.5; }
    patchGroups(rule.groups.map((g) =>
      g.id !== groupId ? g : { ...g, conditions: [...g.conditions, newCond] }
    ));
  };

  // Reorder groups by drag — ensure first group has no joinOperator, rest inherit or default to 'any'.
  const reorderGroups = (from: number, to: number) => {
    const next = [...rule.groups];
    const [moved] = next.splice(from, 1);
    next.splice(to, 0, moved);
    patchGroups(next.map((g, i) =>
      i === 0 ? { ...g, joinOperator: undefined } : { ...g, joinOperator: g.joinOperator ?? 'any' }
    ));
  };

  return (
    <div className="pipeline-rule-card">
      {/* Header */}
      <div className="pipeline-rule-header">
        <span className="pipeline-rule-badge">{index + 1}</span>
        <input type="text" className="form-input pipeline-rule-id-input"
          value={rule.id}
          onChange={(e) => onPatch({ id: e.target.value.trim().toLowerCase().replace(/[^a-z0-9-]+/g, '-') || rule.id })}
          title="Rule ID" />
        <div className="pipeline-rule-route-header">
          <span className="form-label" style={{ fontSize: '11px', margin: 0, flexShrink: 0 }}>→</span>
          <select className="form-input form-select" style={{ fontSize: '12px' }}
            value={rule.routeTo}
            onChange={(e) => onPatch({ routeTo: e.target.value })}>
            <option value="">Select model…</option>
            {candidates.map((id) => <option key={id} value={id}>{displayName(id)}</option>)}
          </select>
        </div>
        <button type="button" className="pipeline-icon-btn" title="Remove rule" onClick={onRemove}>
          <svg width="10" height="10" viewBox="0 0 12 12" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round"><path d="M1 1 L11 11 M11 1 L1 11"/></svg>
        </button>
      </div>

      {/* Groups */}
      <div className="pipeline-rule-body">
        {rule.groups.map((group, gi) => (
          <React.Fragment key={group.id}>
            {gi > 0 && (
              <div className="pipeline-group-divider">
                <div className="pipeline-group-divider-line" />
                <button
                  type="button"
                  className="pipeline-group-op-toggle"
                  title="Click to switch between OR / AND"
                  onClick={() => patchGroup(group.id, { joinOperator: (group.joinOperator ?? 'any') === 'any' ? 'all' : 'any' })}
                >
                  {(group.joinOperator ?? 'any') === 'any' ? 'OR' : 'AND'}
                </button>
                <div className="pipeline-group-divider-line" />
              </div>
            )}
            <div
              className={`pipeline-group-wrapper${dragTargetIndex === gi ? ' pipeline-group-wrapper--drag-over' : ''}`}
              onDragOver={(e) => { e.preventDefault(); setDragTargetIndex(gi); }}
              onDragLeave={() => setDragTargetIndex(null)}
              onDrop={(e) => {
                e.preventDefault();
                setDragTargetIndex(null);
                if (dragFromRef.current !== null && dragFromRef.current !== gi) {
                  reorderGroups(dragFromRef.current, gi);
                }
                dragFromRef.current = null;
              }}
            >
              <span
                className="pipeline-group-drag-handle"
                title="Drag to reorder groups"
                draggable
                onDragStart={() => { dragFromRef.current = gi; }}
              >⠿</span>
              <ConditionGroup
                group={group}
                isOnly={rule.groups.length === 1}
                classifiers={classifiers}
                onPatchCond={(condId, p) => patchCond(group.id, condId, p)}
                onRemoveCond={(condId) => removeCond(group.id, condId)}
                onAddCond={(type, classifierId) => addCond(group.id, type, classifierId)}
                onPatchGroup={(p) => patchGroup(group.id, p)}
                onRemoveGroup={() => removeGroup(group.id)}
                onHighlightClassifier={onHighlightClassifier}
              />
            </div>
          </React.Fragment>
        ))}
        <button type="button" className="settings-reset-button pipeline-add-group-btn" onClick={addGroup}>
          + Add Group
        </button>
      </div>
    </div>
  );
};

// --------------------------------------------------------------------------
// Main canvas
// --------------------------------------------------------------------------

const RouterPipelineCanvas: React.FC<RouterPipelineCanvasProps> = ({
  draft, candidateOptions, embeddingOptions, displayName, displayNameWithStatus,
  onPatchClassifier, onAddClassifier, onRemoveClassifier,
  onPatchRule, onAddRule, onRemoveRule,
  highlightedClassifierId, onHighlightClassifier,
}) => {
  const classifiers = draft.classifiers ?? [];
  const rules = draft.rules ?? [];

  return (
    <div className="pipeline-canvas">
      {/* Classifier swimlane */}
      <div className="pipeline-swimlane-header">
        <span className="pipeline-swimlane-title">Classifiers</span>
        <span className="settings-description" style={{ fontSize: '0.68rem' }}>— optional ML models referenced by conditions</span>
        <button type="button" className="settings-reset-button pipeline-add-btn" style={{ marginLeft: 'auto' }} onClick={onAddClassifier}>
          + Add Classifier
        </button>
      </div>
      <div className="pipeline-clf-lane">
        {classifiers.length === 0 && (
          <div className="collection-role-empty" style={{ alignSelf: 'center' }}>
            No classifiers. Rules can use keyword and length conditions without any.
          </div>
        )}
        {classifiers.map((clf) => (
          <ClassifierCard key={clf.id} clf={clf}
            isHighlighted={highlightedClassifierId === clf.id}
            embeddingOptions={embeddingOptions} candidateOptions={candidateOptions}
            displayNameWithStatus={displayNameWithStatus}
            onPatch={(p) => onPatchClassifier(clf.id, p)}
            onRemove={() => onRemoveClassifier(clf.id)} />
        ))}
      </div>

      {/* Rules lane */}
      <div className="pipeline-swimlane-header" style={{ marginTop: 16 }}>
        <span className="pipeline-swimlane-title">Rules</span>
        <span className="settings-description" style={{ fontSize: '0.68rem' }}>— evaluated top-to-bottom, first match wins</span>
      </div>
      <div className="pipeline-rules-lane">
        {rules.length === 0 && (
          <div className="collection-role-empty" style={{ textAlign: 'center', padding: '12px 0' }}>
            No rules yet. Add a rule to start routing requests.
          </div>
        )}
        {rules.map((rule, i) => (
          <RuleCard key={rule.id} rule={rule} index={i}
            candidates={draft.candidates} classifiers={classifiers}
            displayName={displayName}
            onPatch={(p) => onPatchRule(rule.id, p)}
            onRemove={() => onRemoveRule(rule.id)}
            onHighlightClassifier={onHighlightClassifier} />
        ))}
        {draft.defaultModel && (
          <div className="pipeline-default-row">
            <span className="pipeline-rule-badge pipeline-rule-badge--default">↩</span>
            <span className="pipeline-default-label">Default fallback</span>
            <div className="pipeline-rule-arrow">
              <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round"><path d="M5 12h14M14 6l6 6-6 6"/></svg>
            </div>
            <span className="pipeline-default-model">{displayName(draft.defaultModel)}</span>
          </div>
        )}
        <button type="button" className="settings-reset-button pipeline-add-btn" onClick={onAddRule}>
          + Add Rule
        </button>
      </div>
    </div>
  );
};

export default RouterPipelineCanvas;
