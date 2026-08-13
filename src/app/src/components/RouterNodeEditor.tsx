import React from 'react';
import { Icon } from './Icon';
import { WorkspaceActionButton } from './WorkspacePanels';
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

const LEAF_TYPES: Array<{ value: RouterLeafType; label: string }> = [
  { value: 'keywords_any', label: 'Keywords · any' },
  { value: 'keywords_all', label: 'Keywords · all' },
  { value: 'regex', label: 'Regex' },
  { value: 'min_chars', label: 'Minimum UTF-8 bytes' },
  { value: 'max_chars', label: 'Maximum UTF-8 bytes' },
  { value: 'has_tools', label: 'Has tools' },
  { value: 'has_images', label: 'Has images' },
  { value: 'classifier', label: 'Classifier score' },
  { value: 'metadata', label: 'Request metadata' },
];

interface RouterNodeEditorProps {
  node: RouterNode;
  classifiers: RouterClassifier[];
  onChange: (next: RouterNode) => void;
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
}> = ({ label, value, onChange }) => (
  <label className="router-node__compact-field">
    <span>{label}</span>
    <input
      className="input"
      type="number"
      min="0"
      max="1"
      step="0.05"
      value={value ?? ''}
      placeholder="Any"
      onChange={event => onChange(event.target.value === '' ? undefined : Number(event.target.value))}
    />
  </label>
);

const RouterLeafEditor: React.FC<{
  node: RouterLeafNode;
  classifiers: RouterClassifier[];
  onChange: (next: RouterNode) => void;
}> = ({ node, classifiers, onChange }) => {
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
          <span className="sr-only">Condition type</span>
          <select className="select" value={node.type} onChange={event => changeType(event.target.value as RouterLeafType)}>
            {LEAF_TYPES.map(item => <option key={item.value} value={item.value}>{item.label}</option>)}
          </select>
        </label>

        {(node.type === 'keywords_any' || node.type === 'keywords_all') && (
          <textarea
            className="textarea router-node__grow"
            rows={3}
            value={node.textValue ?? ''}
            placeholder="One keyword per line"
            onChange={event => update({ textValue: event.target.value })}
          />
        )}
        {node.type === 'regex' && (
          <input
            className="input router-node__grow router-node__mono"
            value={node.textValue ?? ''}
            placeholder="ECMAScript regex"
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
          <select className="select" value={node.booleanValue === false ? 'false' : 'true'} onChange={event => update({ booleanValue: event.target.value === 'true' })}>
            <option value="true">is true</option>
            <option value="false">is false</option>
          </select>
        )}
      </div>

      {node.type === 'classifier' && (
        <div className="router-node__details router-node__details--classifier">
          <label>
            <span>Classifier</span>
            <select
              className="select"
              value={node.classifierId ?? ''}
              onChange={event => {
                // Keep label omitted when choosing a classifier so the condition
                // continues to follow that classifier's default_label. Copying the
                // current default into the rule would silently freeze the old value.
                update({
                  classifierId: event.target.value,
                  label: undefined,
                });
              }}
            >
              <option value="">Select classifier</option>
              {classifiers.map(item => <option key={item.id} value={item.id}>{item.id}</option>)}
            </select>
          </label>
          <label>
            <span>Label</span>
            <select className="select" value={node.label ?? ''} onChange={event => update({ label: event.target.value || undefined })}>
              <option value="" disabled={labels.length > 0 && !selectedClassifier?.defaultLabel}>
                {selectedClassifier?.defaultLabel
                  ? `Use classifier default (${selectedClassifier.defaultLabel})`
                  : labels.length > 0 ? 'Select label' : 'Use classifier output'}
              </option>
              {labels.map(label => <option key={label} value={label}>{label}</option>)}
            </select>
          </label>
          <ScoreInput label="Min score" value={node.minScore} onChange={minScore => update({ minScore })} />
          <ScoreInput label="Max score" value={node.maxScore} onChange={maxScore => update({ maxScore })} />
        </div>
      )}

      {node.type === 'metadata' && (
        <div className="router-node__details router-node__details--metadata">
          <label>
            <span>Metadata key</span>
            <input className="input" value={node.metadataKey ?? ''} placeholder="task_class" onChange={event => update({ metadataKey: event.target.value })} />
          </label>
          <label>
            <span>Comparator</span>
            <select
              className="select"
              value={node.metadataComparator ?? 'equals'}
              onChange={event => update({ metadataComparator: event.target.value as RouterMetadataComparator })}
            >
              <option value="equals">equals</option>
              <option value="any">contains any token</option>
              <option value="exists">exists</option>
            </select>
          </label>
          {(node.metadataComparator ?? 'equals') === 'exists' ? (
            <label>
              <span>Expected</span>
              <select className="select" value={node.booleanValue === false ? 'false' : 'true'} onChange={event => update({ booleanValue: event.target.value === 'true' })}>
                <option value="true">present</option>
                <option value="false">missing</option>
              </select>
            </label>
          ) : node.metadataComparator === 'any' ? (
            <label className="router-node__grow-field">
              <span>Values <small>one per line</small></span>
              <textarea className="textarea" rows={3} value={node.metadataValues ?? ''} onChange={event => update({ metadataValues: event.target.value })} />
            </label>
          ) : (
            <label className="router-node__grow-field">
              <span>Value</span>
              <input className="input" value={node.metadataValues ?? ''} onChange={event => update({ metadataValues: event.target.value })} />
            </label>
          )}
        </div>
      )}
      <div className="router-node__wrap-actions" aria-label="Combine condition">
        <span>Combine:</span>
        <button type="button" onClick={() => onChange({ id: createRouterNodeId('group'), kind: 'group', operator: 'all', children: [node, createRouterLeaf(node.type === 'keywords_any' ? 'keywords_all' : 'keywords_any')] })}>AND</button>
        <button type="button" onClick={() => onChange({ id: createRouterNodeId('group'), kind: 'group', operator: 'any', children: [node, createRouterLeaf(node.type === 'keywords_any' ? 'keywords_all' : 'keywords_any')] })}>OR</button>
        <button type="button" onClick={() => onChange({ id: createRouterNodeId('group'), kind: 'group', operator: 'not', children: [node] })}>NOT</button>
      </div>
    </div>
  );
};

export const RouterNodeEditor: React.FC<RouterNodeEditorProps> = ({ node, classifiers, onChange, depth = 0 }) => {
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

  return (
    <div className="router-node router-node--group" style={{ '--router-depth': depth } as React.CSSProperties}>
      <div className="router-node__group-head">
        <div className="router-node__operator">
          <span>Match</span>
          <select className="select" value={node.operator} onChange={event => changeOperator(event.target.value as RouterGroupNode['operator'])}>
            <option value="all">ALL conditions</option>
            <option value="any">ANY condition</option>
            <option value="not">NOT condition</option>
          </select>
        </div>
        {node.operator !== 'not' && (
          <div className="router-node__group-actions">
            <WorkspaceActionButton size="small" icon="plus" disabled={!unusedConditionType} title={unusedConditionType ? "Add condition" : "Every available condition identity is already used in this gate"} onClick={addCondition}>Condition</WorkspaceActionButton>
            <WorkspaceActionButton size="small" icon="plus" onClick={addGroup}>Group</WorkspaceActionButton>
          </div>
        )}
      </div>
      <div className="router-node__children">
        {node.children.map((child, index) => (
          <div className="router-node__child" key={child.id}>
            <div className="router-node__child-actions" aria-label={`Condition ${index + 1} controls`}>
              <button type="button" disabled={index === 0} title="Move up" aria-label="Move condition up" onClick={() => onChange(moveChild(node, index, -1))}><Icon name="chevron-up" size={13} /></button>
              <button type="button" disabled={index === node.children.length - 1} title="Move down" aria-label="Move condition down" onClick={() => onChange(moveChild(node, index, 1))}><Icon name="chevron-down" size={13} /></button>
              <button type="button" title="Remove condition" aria-label="Remove condition" onClick={() => onChange(removeChild(node, index))}><Icon name="trash" size={13} /></button>
            </div>
            <RouterNodeEditor
              node={child}
              classifiers={classifiers}
              depth={depth + 1}
              onChange={next => onChange(replaceChild(node, index, next))}
            />
          </div>
        ))}
      </div>
    </div>
  );
};

export default RouterNodeEditor;
