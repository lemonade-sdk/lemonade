import React from 'react';
import { Icon } from './Icon';
import {
  classifierLabels,
  createRouterGroup,
  createRouterLeaf,
  createRouterNodeId,
  normalizeRouterNode,
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
  { value: 'min_chars', label: 'Minimum characters' },
  { value: 'max_chars', label: 'Maximum characters' },
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

const ScoreInput: React.FC<{
  label: string;
  value: number | undefined;
  onChange: (value: number | undefined) => void;
}> = ({ label, value, onChange }) => (
  <label className="router-node__compact-field">
    <span>{label}</span>
    <input
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
          <select value={node.type} onChange={event => changeType(event.target.value as RouterLeafType)}>
            {LEAF_TYPES.map(item => <option key={item.value} value={item.value}>{item.label}</option>)}
          </select>
        </label>

        {(node.type === 'keywords_any' || node.type === 'keywords_all') && (
          <input
            className="router-node__grow"
            value={node.textValue ?? ''}
            placeholder="Comma-separated keywords"
            onChange={event => update({ textValue: event.target.value })}
          />
        )}
        {node.type === 'regex' && (
          <input
            className="router-node__grow router-node__mono"
            value={node.textValue ?? ''}
            placeholder="ECMAScript regex"
            onChange={event => update({ textValue: event.target.value })}
          />
        )}
        {(node.type === 'min_chars' || node.type === 'max_chars') && (
          <input
            className="router-node__number"
            type="number"
            min="0"
            step="1"
            value={node.numberValue ?? ''}
            onChange={event => update({ numberValue: event.target.value === '' ? undefined : Number(event.target.value) })}
          />
        )}
        {(node.type === 'has_tools' || node.type === 'has_images') && (
          <select value={node.booleanValue === false ? 'false' : 'true'} onChange={event => update({ booleanValue: event.target.value === 'true' })}>
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
              value={node.classifierId ?? ''}
              onChange={event => {
                const classifier = classifiers.find(item => item.id === event.target.value);
                const nextLabels = classifierLabels(classifier);
                update({
                  classifierId: event.target.value,
                  label: classifier?.defaultLabel || nextLabels[0] || undefined,
                });
              }}
            >
              <option value="">Select classifier</option>
              {classifiers.map(item => <option key={item.id} value={item.id}>{item.id}</option>)}
            </select>
          </label>
          <label>
            <span>Label</span>
            <select value={node.label ?? ''} onChange={event => update({ label: event.target.value || undefined })}>
              <option value="">Use classifier default</option>
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
            <input value={node.metadataKey ?? ''} placeholder="task_class" onChange={event => update({ metadataKey: event.target.value })} />
          </label>
          <label>
            <span>Comparator</span>
            <select
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
              <select value={node.booleanValue === false ? 'false' : 'true'} onChange={event => update({ booleanValue: event.target.value === 'true' })}>
                <option value="true">present</option>
                <option value="false">missing</option>
              </select>
            </label>
          ) : (
            <label className="router-node__grow-field">
              <span>{node.metadataComparator === 'any' ? 'Comma-separated values' : 'Value'}</span>
              <input value={node.metadataValues ?? ''} onChange={event => update({ metadataValues: event.target.value })} />
            </label>
          )}
        </div>
      )}
      <div className="router-node__wrap-actions" aria-label="Combine condition">
        <span>Combine:</span>
        <button type="button" onClick={() => onChange({ id: createRouterNodeId('group'), kind: 'group', operator: 'all', children: [node, createRouterLeaf()] })}>AND</button>
        <button type="button" onClick={() => onChange({ id: createRouterNodeId('group'), kind: 'group', operator: 'any', children: [node, createRouterLeaf()] })}>OR</button>
        <button type="button" onClick={() => onChange({ id: createRouterNodeId('group'), kind: 'group', operator: 'not', children: [node] })}>NOT</button>
      </div>
    </div>
  );
};

export const RouterNodeEditor: React.FC<RouterNodeEditorProps> = ({ node, classifiers, onChange, depth = 0 }) => {
  if (node.kind === 'leaf') {
    return <RouterLeafEditor node={node} classifiers={classifiers} onChange={onChange} />;
  }

  const addCondition = () => onChange({ ...node, children: [...node.children, createRouterLeaf()] });
  const addGroup = () => onChange({ ...node, children: [...node.children, createRouterGroup('all')] });
  const changeOperator = (operator: RouterGroupNode['operator']) => {
    const children = operator === 'not'
      ? [node.children[0] || createRouterLeaf()]
      : node.children.length >= 2
        ? node.children
        : [node.children[0] || createRouterLeaf(), createRouterLeaf('has_tools')];
    onChange({ ...node, operator, children });
  };

  return (
    <div className="router-node router-node--group" style={{ '--router-depth': depth } as React.CSSProperties}>
      <div className="router-node__group-head">
        <div className="router-node__operator">
          <span>Match</span>
          <select value={node.operator} onChange={event => changeOperator(event.target.value as RouterGroupNode['operator'])}>
            <option value="all">ALL conditions</option>
            <option value="any">ANY condition</option>
            <option value="not">NOT condition</option>
          </select>
        </div>
        {node.operator !== 'not' && (
          <div className="router-node__group-actions">
            <button type="button" className="btn btn--ghost btn--tiny" onClick={addCondition}><Icon name="plus" size={13} /> Condition</button>
            <button type="button" className="btn btn--ghost btn--tiny" onClick={addGroup}><Icon name="plus" size={13} /> Group</button>
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
