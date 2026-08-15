import React, { useCallback, useEffect, useLayoutEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { Icon } from './Icon';
import { WorkspaceActionButton } from './WorkspacePanels';
import { useI18n } from '../i18n';

interface RouterSelectOption {
  value: string;
  label: string;
  disabled?: boolean;
}

interface RouterSelectProps {
  value: string;
  options: RouterSelectOption[];
  onChange: (value: string) => void;
  ariaLabel?: string;
  className?: string;
  disabled?: boolean;
}

function computeCoords(trigger: HTMLButtonElement) {
  const r = trigger.getBoundingClientRect();
  return { top: r.bottom + 4, left: r.left, width: r.width };
}

export const RouterSelect: React.FC<RouterSelectProps> = ({ value, options, onChange, ariaLabel, className, disabled }) => {
  const [open, setOpen] = useState(false);
  const [coords, setCoords] = useState<{ top: number; left: number; width: number } | null>(null);
  const triggerRef = useRef<HTMLButtonElement>(null);
  const popoverRef = useRef<HTMLDivElement>(null);
  const optionRefs = useRef<(HTMLButtonElement | null)[]>([]);
  const selected = options.find(o => o.value === value);

  const reposition = useCallback(() => {
    if (triggerRef.current) setCoords(computeCoords(triggerRef.current));
  }, []);

  useLayoutEffect(() => {
    if (!open || !triggerRef.current) return;
    setCoords(computeCoords(triggerRef.current));
  }, [open]);

  useEffect(() => {
    if (!open) return;
    const closeOnScroll = (e: Event) => {
      const t = e.target as Node;
      if (popoverRef.current?.contains(t)) return;
      setOpen(false);
    };
    window.addEventListener('scroll', closeOnScroll, { capture: true, passive: true });
    window.addEventListener('resize', reposition, { passive: true });
    return () => {
      window.removeEventListener('scroll', closeOnScroll, { capture: true });
      window.removeEventListener('resize', reposition);
    };
  }, [open, reposition]);

  useEffect(() => {
    if (!open) return;
    const handler = (e: MouseEvent) => {
      const t = e.target as Node;
      if (!triggerRef.current?.contains(t) && !popoverRef.current?.contains(t)) setOpen(false);
    };
    document.addEventListener('mousedown', handler);
    return () => document.removeEventListener('mousedown', handler);
  }, [open]);

  const close = useCallback(() => {
    setOpen(false);
    triggerRef.current?.focus();
  }, []);

  const pick = useCallback((val: string, isDisabled?: boolean) => {
    if (isDisabled) return;
    onChange(val);
    setOpen(false);
    triggerRef.current?.focus();
  }, [onChange]);

  const onTriggerKeyDown = (e: React.KeyboardEvent<HTMLButtonElement>) => {
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault();
      if (!open) {
        setOpen(true);
        // Focus first (ArrowDown) or last (ArrowUp) enabled option after open
        window.setTimeout(() => {
          const enabled = optionRefs.current.filter(Boolean) as HTMLButtonElement[];
          const target = e.key === 'ArrowDown' ? enabled[0] : enabled[enabled.length - 1];
          target?.focus();
        }, 0);
      } else {
        const enabled = optionRefs.current.filter(Boolean) as HTMLButtonElement[];
        const target = e.key === 'ArrowDown' ? enabled[0] : enabled[enabled.length - 1];
        target?.focus();
      }
    } else if (e.key === 'Escape') {
      e.preventDefault();
      e.stopPropagation();
      setOpen(false);
    }
  };

  const onOptionKeyDown = (e: React.KeyboardEvent<HTMLButtonElement>, index: number, opt: RouterSelectOption) => {
    if (e.key === 'ArrowDown') {
      e.preventDefault();
      const next = optionRefs.current.slice(index + 1).find(Boolean) as HTMLButtonElement | undefined;
      next?.focus();
    } else if (e.key === 'ArrowUp') {
      e.preventDefault();
      const prev = optionRefs.current.slice(0, index).reverse().find(Boolean) as HTMLButtonElement | undefined;
      if (prev) prev.focus(); else close();
    } else if (e.key === 'Escape') {
      e.preventDefault();
      e.stopPropagation();
      close();
    } else if (e.key === 'Enter' || e.key === ' ') {
      e.preventDefault();
      pick(opt.value, opt.disabled);
    } else if (e.key === 'Tab') {
      close();
    }
  };

  return (
    <div className={`router-select${className ? ` ${className}` : ''}`}>
      <button
        ref={triggerRef}
        type="button"
        className={`router-select__trigger${open ? ' is-open' : ''}`}
        aria-haspopup="listbox"
        aria-expanded={open}
        aria-label={ariaLabel}
        disabled={disabled}
        onClick={() => setOpen(v => !v)}
        onKeyDown={onTriggerKeyDown}
      >
        <span className={selected ? undefined : 'is-placeholder'}>{selected?.label ?? value}</span>
        <Icon name="chevron-down" size={12} />
      </button>
      {open && coords && createPortal(
        <div
          ref={popoverRef}
          className="router-select__popover"
          role="listbox"
          aria-label={ariaLabel}
          style={{ top: coords.top, left: coords.left, minWidth: coords.width }}
        >
          {options.map((opt, index) => (
            <button
              ref={el => { optionRefs.current[index] = el; }}
              type="button"
              key={opt.value}
              role="option"
              aria-selected={opt.value === value}
              aria-disabled={opt.disabled}
              tabIndex={-1}
              className={`router-select__option${opt.value === value ? ' is-selected' : ''}${opt.disabled ? ' is-disabled' : ''}`}
              onClick={() => pick(opt.value, opt.disabled)}
              onKeyDown={e => onOptionKeyDown(e, index, opt)}
            >
              {opt.label}
            </button>
          ))}
        </div>,
        document.body,
      )}
    </div>
  );
};
import {
  classifierLabels,
  createRouterGroup,
  createRouterLeaf,
  createRouterNodeId,
  normalizeRouterNode,
  routerConditionIdentity,
  type RouterClassifier,
  type RouterGroupNode,
  type RouterLeafNode,
  type RouterLeafType,
  type RouterMetadataComparator,
  type RouterNode,
} from '../features/router/routerTypes';

const LEAF_TYPES: Array<{ value: RouterLeafType; labelKey: string }> = [
  { value: 'keywords_any', labelKey: 'node.types.keywordsAny' },
  { value: 'keywords_all', labelKey: 'node.types.keywordsAll' },
  { value: 'regex', labelKey: 'node.types.regex' },
  { value: 'min_chars', labelKey: 'node.types.minChars' },
  { value: 'max_chars', labelKey: 'node.types.maxChars' },
  { value: 'has_tools', labelKey: 'node.types.hasTools' },
  { value: 'has_images', labelKey: 'node.types.hasImages' },
  { value: 'classifier', labelKey: 'node.types.classifier' },
  { value: 'metadata', labelKey: 'node.types.metadata' },
];

interface RouterNodeEditorProps {
  node: RouterNode;
  classifiers: RouterClassifier[];
  onChange: (next: RouterNode) => void;
  onRemoveSelf?: () => void;
  depth?: number;
}

function replaceChild(group: RouterGroupNode, index: number, child: RouterNode): RouterNode {
  return normalizeRouterNode({
    ...group,
    children: group.children.map((current, childIndex) => childIndex === index ? child : current),
  });
}

function removeChild(group: RouterGroupNode, index: number): RouterNode {
  return normalizeRouterNode({
    ...group,
    children: group.children.filter((_, childIndex) => childIndex !== index),
  });
}

function moveChild(group: RouterGroupNode, index: number, delta: number): RouterNode {
  const target = index + delta;
  if (target < 0 || target >= group.children.length) return group;
  const children = [...group.children];
  [children[index], children[target]] = [children[target], children[index]];
  return { ...group, children };
}

function unusedGroupLeafType(group: RouterGroupNode): RouterLeafType | null {
  const identities = new Set(group.children.map(routerConditionIdentity).filter((identity): identity is string => Boolean(identity)));
  for (const item of LEAF_TYPES) {
    const identity = item.value === 'classifier'
      ? 'classifier:'
      : item.value === 'metadata'
        ? 'metadata:'
        : item.value;
    if (!identities.has(identity)) return item.value;
  }
  return null;
}

const ScoreInput: React.FC<{
  label: string;
  value: number | undefined;
  onChange: (value: number | undefined) => void;
}> = ({ label, value, onChange }) => {
  const { t } = useI18n('router');
  return (
  <label className="router-node__compact-field">
    <span>{label}</span>
    <input
      className="input"
      type="number"
      min="0"
      max="1"
      step="0.05"
      value={value ?? ''}
      placeholder={t('node.any')}
      onChange={event => onChange(event.target.value === '' ? undefined : Number(event.target.value))}
    />
  </label>
  );
};

const RouterLeafEditor: React.FC<{
  node: RouterLeafNode;
  classifiers: RouterClassifier[];
  onChange: (next: RouterNode) => void;
}> = ({ node, classifiers, onChange }) => {
  const { t } = useI18n('router');
  const update = (patch: Partial<RouterLeafNode>) => onChange({ ...node, ...patch });
  const selectedClassifier = classifiers.find(item => item.id === node.classifierId);
  const labels = classifierLabels(selectedClassifier);

  const changeType = (type: RouterLeafType) => {
    const replacement = createRouterLeaf(type);
    onChange({ ...replacement, id: node.id });
  };

  return (
    <div className="router-node router-node--leaf">
      <div className="router-node__leaf-row">
        <label className="router-node__type-field">
          <span className="sr-only">{t('node.conditionType')}</span>
          <RouterSelect
            value={node.type}
            options={LEAF_TYPES.map(item => ({ value: item.value, label: t(item.labelKey) }))}
            onChange={val => changeType(val as RouterLeafType)}
            ariaLabel={t('node.conditionType')}
          />
        </label>

        {(node.type === 'keywords_any' || node.type === 'keywords_all') && (
          <textarea
            className="textarea router-node__grow"
            rows={3}
            value={node.textValue ?? ''}
            placeholder={t('node.oneKeywordPerLine')}
            onChange={event => update({ textValue: event.target.value })}
          />
        )}
        {node.type === 'regex' && (
          <input
            className="input router-node__grow router-node__mono"
            value={node.textValue ?? ''}
            placeholder={t('node.regexPlaceholder')}
            onChange={event => update({ textValue: event.target.value })}
          />
        )}
        {(node.type === 'min_chars' || node.type === 'max_chars') && (
          <input
            className="input router-node__number"
            type="number"
            min="0"
            step="1"
            value={node.numberValue ?? ''}
            onChange={event => update({ numberValue: event.target.value === '' ? undefined : Number(event.target.value) })}
          />
        )}
        {(node.type === 'has_tools' || node.type === 'has_images') && (
          <RouterSelect
            value={node.booleanValue === false ? 'false' : 'true'}
            options={[{ value: 'true', label: t('node.isTrue') }, { value: 'false', label: t('node.isFalse') }]}
            onChange={val => update({ booleanValue: val === 'true' })}
          />
        )}
      </div>

      {node.type === 'classifier' && (
        <div className="router-node__details router-node__details--classifier">
          <label>
            <span>{t('node.classifier')}</span>
            <RouterSelect
              value={node.classifierId ?? ''}
              options={[
                { value: '', label: t('node.selectClassifier') },
                ...classifiers.map(item => ({ value: item.id, label: item.id })),
              ]}
              onChange={val => update({ classifierId: val, label: undefined })}
              ariaLabel={t('node.classifier')}
            />
          </label>
          <label>
            <span>{t('node.label')}</span>
            <RouterSelect
              value={node.label ?? ''}
              options={[
                {
                  value: '',
                  label: selectedClassifier?.defaultLabel
                    ? t('node.useClassifierDefaultNamed', { label: selectedClassifier.defaultLabel })
                    : labels.length > 0 ? t('node.selectLabel') : t('node.useClassifierOutput'),
                  disabled: labels.length > 0 && !selectedClassifier?.defaultLabel,
                },
                ...labels.map(label => ({ value: label, label })),
              ]}
              onChange={val => update({ label: val || undefined })}
              ariaLabel={t('node.label')}
            />
          </label>
          <ScoreInput label={t('node.minScore')} value={node.minScore} onChange={minScore => update({ minScore })} />
          <ScoreInput label={t('node.maxScore')} value={node.maxScore} onChange={maxScore => update({ maxScore })} />
        </div>
      )}

      {node.type === 'metadata' && (
        <div className="router-node__details router-node__details--metadata">
          <label>
            <span>{t('node.metadataKey')}</span>
            <input className="input" value={node.metadataKey ?? ''} placeholder="task_class" onChange={event => update({ metadataKey: event.target.value })} />
          </label>
          <label>
            <span>{t('node.comparator')}</span>
            <RouterSelect
              value={node.metadataComparator ?? 'equals'}
              options={[
                { value: 'equals', label: t('node.equals') },
                { value: 'any', label: t('node.containsAny') },
                { value: 'exists', label: t('node.exists') },
              ]}
              onChange={val => update({ metadataComparator: val as RouterMetadataComparator })}
              ariaLabel={t('node.comparator')}
            />
          </label>
          {(node.metadataComparator ?? 'equals') === 'exists' ? (
            <label>
              <span>{t('node.expected')}</span>
              <RouterSelect
                value={node.booleanValue === false ? 'false' : 'true'}
                options={[{ value: 'true', label: t('node.present') }, { value: 'false', label: t('node.missing') }]}
                onChange={val => update({ booleanValue: val === 'true' })}
                ariaLabel={t('node.expected')}
              />
            </label>
          ) : node.metadataComparator === 'any' ? (
            <label className="router-node__grow-field">
              <span>{t('node.values')} <small>{t('node.onePerLine')}</small></span>
              <textarea className="textarea" rows={3} value={node.metadataValues ?? ''} onChange={event => update({ metadataValues: event.target.value })} />
            </label>
          ) : (
            <label className="router-node__grow-field">
              <span>{t('node.value')}</span>
              <input className="input" value={node.metadataValues ?? ''} onChange={event => update({ metadataValues: event.target.value })} />
            </label>
          )}
        </div>
      )}
      <div className="router-node__wrap-actions" aria-label={t('node.combineCondition')}>
        <span>{t('node.combine')}:</span>
        <button type="button" onClick={() => onChange({ id: createRouterNodeId('group'), kind: 'group', operator: 'all', children: [node, createRouterLeaf(node.type === 'keywords_any' ? 'keywords_all' : 'keywords_any')] })}>{t('node.and')}</button>
        <button type="button" onClick={() => onChange({ id: createRouterNodeId('group'), kind: 'group', operator: 'any', children: [node, createRouterLeaf(node.type === 'keywords_any' ? 'keywords_all' : 'keywords_any')] })}>{t('node.or')}</button>
        <button type="button" onClick={() => onChange({ id: createRouterNodeId('group'), kind: 'group', operator: 'not', children: [node] })}>{t('node.not')}</button>
      </div>
    </div>
  );
};

export const RouterNodeEditor: React.FC<RouterNodeEditorProps> = ({ node, classifiers, onChange, onRemoveSelf, depth = 0 }) => {
  const { t } = useI18n('router');
  if (node.kind === 'leaf') {
    return <RouterLeafEditor node={node} classifiers={classifiers} onChange={onChange} />;
  }

  const unusedConditionType = unusedGroupLeafType(node);
  const addCondition = () => {
    if (unusedConditionType) onChange({ ...node, children: [...node.children, createRouterLeaf(unusedConditionType)] });
  };
  const addGroup = () => onChange({ ...node, children: [...node.children, createRouterGroup('all')] });
  const changeOperator = (operator: RouterGroupNode['operator']) => {
    if (operator === node.operator) return;
    if (operator === 'not' && node.children.length > 1) {
      // Negate the complete existing expression. Truncating to children[0]
      // would silently delete conditions when AND/OR is changed to NOT.
      const inner: RouterGroupNode = { ...node, id: createRouterNodeId('group') };
      onChange({ ...node, operator: 'not', children: [inner] });
      return;
    }
    onChange({ ...node, operator, children: node.children.length ? node.children : [createRouterLeaf()] });
  };

  const handleRemoveChild = (index: number) => {
    if (node.children.length === 1 && onRemoveSelf) {
      onRemoveSelf();
    } else {
      onChange(removeChild(node, index));
    }
  };

  return (
    <div className="router-node router-node--group" style={{ '--router-depth': depth } as React.CSSProperties}>
      <div className="router-node__group-head">
        <div className="router-node__operator">
          <span>{t('node.match')}</span>
          <RouterSelect
            value={node.operator}
            options={[
              { value: 'all', label: t('node.allConditions') },
              { value: 'any', label: t('node.anyCondition') },
              { value: 'not', label: t('node.notCondition') },
            ]}
            onChange={val => changeOperator(val as RouterGroupNode['operator'])}
            ariaLabel={t('node.groupOperator')}
          />
        </div>
        {node.operator !== 'not' && (
          <div className="router-node__group-actions">
            <WorkspaceActionButton size="small" icon="plus" disabled={!unusedConditionType} title={unusedConditionType ? t('node.addCondition') : t('node.allConditionIdentitiesUsed')} onClick={addCondition}>{t('node.condition')}</WorkspaceActionButton>
            <WorkspaceActionButton size="small" icon="plus" onClick={addGroup}>{t('node.group')}</WorkspaceActionButton>
          </div>
        )}
      </div>
      <div className="router-node__children">
        {node.children.map((child, index) => (
          <div className="router-node__child" key={child.id}>
            <div className="router-node__child-actions" aria-label={t('node.conditionControls', { index: index + 1 })}>
              <button type="button" disabled={index === 0} title={t('node.moveUp')} aria-label={t('node.moveConditionUp')} onClick={() => onChange(moveChild(node, index, -1))}><Icon name="chevron-up" size={13} /></button>
              <button type="button" disabled={index === node.children.length - 1} title={t('node.moveDown')} aria-label={t('node.moveConditionDown')} onClick={() => onChange(moveChild(node, index, 1))}><Icon name="chevron-down" size={13} /></button>
              <button type="button" title={t('node.removeCondition')} aria-label={t('node.removeCondition')} onClick={() => handleRemoveChild(index)}><Icon name="trash" size={13} /></button>
            </div>
            <RouterNodeEditor
              node={child}
              classifiers={classifiers}
              depth={depth + 1}
              onChange={next => onChange(replaceChild(node, index, next))}
              onRemoveSelf={() => handleRemoveChild(index)}
            />
          </div>
        ))}
      </div>
    </div>
  );
};

export default RouterNodeEditor;
