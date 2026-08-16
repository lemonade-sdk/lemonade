import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Icon } from './Icon';
import RouterNodeEditor from './RouterNodeEditor';
import {
  classifierLabels,
  createRouterLeaf,
  type RouterClassifier,
  type RouterGroupOperator,
  type RouterLeafNode,
  type RouterLeafType,
  type RouterNode,
} from '../features/router/routerTypes';
import {
  appendRouterNodeAtPath,
  createEmptyRouterGraphGroup,
  isBlankRouterGraphNode,
  removeRouterNodeAtPath,
  replaceRouterNodeAtPath,
  routerDuplicateConditionConflict,
  routerNodeAtPath,
  routerReplacementConflictAtPath,
  wrapRouterNode,
  type RouterNodePath,
} from '../features/router/routerGraph';

const ROUTER_GRAPH_DRAG_MIME = 'application/x-lemonade-router-node';
const ZOOM_MIN = 0.5;
const ZOOM_MAX = 1.8;
const ZOOM_STEP = 0.15;

type RouterGraphDragData =
  | { kind: 'operator'; operator: RouterGroupOperator }
  | { kind: 'leaf'; leafType: RouterLeafType; classifierId?: string };

const LEAF_LABELS: Record<RouterLeafType, string> = {
  keywords_any: 'Keywords · any',
  keywords_all: 'Keywords · all',
  regex: 'Regex',
  min_chars: 'Min UTF-8 bytes',
  max_chars: 'Max UTF-8 bytes',
  has_tools: 'Has tools',
  has_images: 'Has images',
  classifier: 'Classifier',
  metadata: 'Metadata',
};

const TOOLBOX_LEAVES: RouterLeafType[] = [
  'keywords_any',
  'keywords_all',
  'regex',
  'min_chars',
  'max_chars',
  'has_tools',
  'has_images',
  'metadata',
];

const OPERATOR_LABELS: Record<RouterGroupOperator, string> = {
  all: 'AND',
  any: 'OR',
  not: 'NOT',
};


function exactSummary(value: unknown, fallback: string): string {
  const raw = String(value ?? '');
  if (!raw.length) return fallback;
  return raw === raw.trim() ? raw : JSON.stringify(raw);
}

const GraphGateShape: React.FC<{ operator: RouterGroupOperator; compact?: boolean }> = ({ operator, compact = false }) => {
  const size = compact ? 24 : 30;
  if (operator === 'all') {
    return (
      <svg className="router-graph__gate-shape" viewBox="0 0 56 44" width={size} height={Math.round(size * .78)} aria-hidden="true">
        <path d="M4 3 L52 3 L52 20 L28 41 L4 20 Z" />
      </svg>
    );
  }
  if (operator === 'any') {
    return (
      <svg className="router-graph__gate-shape" viewBox="0 0 56 40" width={size} height={Math.round(size * .72)} aria-hidden="true">
        <path d="M28 3 Q52 8 52 20 Q52 32 28 37 Q4 32 4 20 Q4 8 28 3 Z" />
      </svg>
    );
  }
  return (
    <svg className="router-graph__gate-shape" viewBox="0 0 44 44" width={size} height={size} aria-hidden="true">
      <circle cx="22" cy="22" r="18" />
      <line x1="9" y1="9" x2="35" y2="35" />
    </svg>
  );
};

function leafSummary(node: RouterLeafNode, classifiers: RouterClassifier[]): string {
  if (node.type === 'keywords_any' || node.type === 'keywords_all') {
    return String(node.textValue || '').trim() || 'Add keywords';
  }
  if (node.type === 'regex') return exactSummary(node.textValue, 'Add regex');
  if (node.type === 'min_chars') return `≥ ${node.numberValue ?? 0}`;
  if (node.type === 'max_chars') return `≤ ${node.numberValue ?? 0}`;
  if (node.type === 'has_tools') return node.booleanValue === false ? 'is false' : 'is true';
  if (node.type === 'has_images') return node.booleanValue === false ? 'is false' : 'is true';
  if (node.type === 'metadata') {
    const key = exactSummary(node.metadataKey, 'metadata key');
    const comparator = node.metadataComparator || 'equals';
    if (comparator === 'exists') return `${key} ${node.booleanValue === false ? 'missing' : 'present'}`;
    return `${key} ${comparator === 'any' ? 'contains' : '='} ${exactSummary(node.metadataValues, '…')}`;
  }
  const classifier = classifiers.find(item => item.id === node.classifierId);
  const labels = classifierLabels(classifier);
  const label = node.label
    || classifier?.defaultLabel
    || (labels.length > 0 ? 'Select label' : 'classifier output');
  return `${node.classifierId || 'Select classifier'} · ${label}${node.minScore !== undefined ? ` · ≥ ${node.minScore}` : ''}`;
}

function createGraphLeaf(data: Extract<RouterGraphDragData, { kind: 'leaf' }>): RouterLeafNode {
  const leaf = createRouterLeaf(data.leafType);
  if (data.leafType === 'classifier' && data.classifierId) {
    leaf.classifierId = data.classifierId;
    leaf.minScore = 0.5;
  }
  return leaf;
}

function encodeDrag(data: RouterGraphDragData): string {
  return JSON.stringify(data);
}

function decodeDrag(value: string): RouterGraphDragData | null {
  try {
    const parsed = JSON.parse(value);
    if (!parsed || typeof parsed !== 'object') return null;
    if (parsed.kind === 'operator' && ['all', 'any', 'not'].includes(parsed.operator)) return parsed as RouterGraphDragData;
    if (parsed.kind === 'leaf' && typeof parsed.leafType === 'string' && parsed.leafType in LEAF_LABELS) return parsed as RouterGraphDragData;
  } catch {
    // Ignore foreign drag payloads.
  }
  return null;
}

interface GraphNodeProps {
  node: RouterNode;
  path: RouterNodePath;
  classifiers: RouterClassifier[];
  selectedPath: RouterNodePath | null;
  onSelect: (path: RouterNodePath) => void;
  onRemove: (path: RouterNodePath) => void;
  onChangeOperator: (path: RouterNodePath, operator: RouterGroupOperator) => void;
  onDropData: (path: RouterNodePath, data: RouterGraphDragData) => void;
}

const GraphDropZone: React.FC<{
  path: RouterNodePath;
  label?: string;
  onDropData: (path: RouterNodePath, data: RouterGraphDragData) => void;
}> = ({ path, label = 'Drop condition or gate here', onDropData }) => {
  const [over, setOver] = useState(false);
  return (
    <div
      className={`router-graph__drop-zone ${over ? 'is-over' : ''}`}
      onDragOver={event => { event.preventDefault(); event.stopPropagation(); event.dataTransfer.dropEffect = 'copy'; setOver(true); }}
      onDragLeave={() => setOver(false)}
      onDrop={event => {
        event.preventDefault();
        event.stopPropagation();
        setOver(false);
        const data = decodeDrag(event.dataTransfer.getData(ROUTER_GRAPH_DRAG_MIME));
        if (data) onDropData(path, data);
      }}
    >
      <Icon name="plus" size={12} />
      <span>{label}</span>
    </div>
  );
};

const GraphNode: React.FC<GraphNodeProps> = ({
  node,
  path,
  classifiers,
  selectedPath,
  onSelect,
  onRemove,
  onChangeOperator,
  onDropData,
}) => {
  const selected = selectedPath != null
    && path.length === selectedPath.length
    && path.every((part, index) => part === selectedPath[index]);

  if (node.kind === 'leaf') {
    return (
      <div className="router-graph__branch">
        <button
          type="button"
          className={`router-graph__leaf router-graph__leaf--${node.type} ${selected ? 'is-selected' : ''}`}
          onClick={() => onSelect(path)}
          title="Click to edit this condition"
        >
          <span className="router-graph__leaf-type">{LEAF_LABELS[node.type]}</span>
          <span className="router-graph__leaf-summary">{leafSummary(node, classifiers)}</span>
        </button>
        <button type="button" className="router-graph__remove" aria-label="Remove condition" title="Remove condition" onClick={() => onRemove(path)}>
          <Icon name="x" size={11} />
        </button>
      </div>
    );
  }

  return (
    <div className={`router-graph__group router-graph__group--${node.operator} ${selected ? 'is-selected' : ''}`}>
      <div className="router-graph__group-head">
        <button
          type="button"
          className={`router-graph__gate router-graph__gate--${node.operator}`}
          onClick={() => onSelect(path)}
          aria-pressed={selected}
          title={`Select ${OPERATOR_LABELS[node.operator]} gate`}
        >
          <GraphGateShape operator={node.operator} />
          {OPERATOR_LABELS[node.operator]}
        </button>
        <span className="router-graph__group-copy">
          {node.operator === 'all' ? 'All child conditions must match' : node.operator === 'any' ? 'Any child condition may match' : 'Negate the child condition'}
        </span>
        <div className="router-graph__operator-actions" onClick={event => event.stopPropagation()}>
          {node.operator !== 'not' && (
            <>
              <button type="button" className={node.operator === 'all' ? 'is-active' : ''} onClick={() => onChangeOperator(path, 'all')}>AND</button>
              <button type="button" className={node.operator === 'any' ? 'is-active' : ''} onClick={() => onChangeOperator(path, 'any')}>OR</button>
            </>
          )}
          <button type="button" aria-label="Remove gate" title="Remove gate" onClick={() => onRemove(path)}><Icon name="x" size={11} /></button>
        </div>
      </div>
      <div className="router-graph__children">
        {node.children.map((child, index) => (
          <GraphNode
            key={child.id}
            node={child}
            path={[...path, index]}
            classifiers={classifiers}
            selectedPath={selectedPath}
            onSelect={onSelect}
            onRemove={onRemove}
            onChangeOperator={onChangeOperator}
            onDropData={onDropData}
          />
        ))}
        {(node.operator !== 'not' || node.children.length === 0) && (
          <GraphDropZone path={path} onDropData={onDropData} />
        )}
      </div>
    </div>
  );
};

interface RouterRuleGraphProps {
  node: RouterNode;
  classifiers: RouterClassifier[];
  onChange: (node: RouterNode) => void;
  onExpand?: () => void;
  // Full-modal layout: workspace and inspector sit side by side and the
  // divider between them becomes draggable.
  expanded?: boolean;
  // Allows a freshly-mounted expanded instance to inherit committed state from
  // the inline instance. Without this, a blank keywords_any node (textValue='')
  // looks identical to the initial empty graph and would be hidden on expand.
  initialCommitted?: boolean;
}

const INSPECTOR_MIN_WIDTH = 320;
const WORKSPACE_MIN_WIDTH = 360;

export const RouterRuleGraph: React.FC<RouterRuleGraphProps> = ({ node, classifiers, onChange, onExpand, expanded = false, initialCommitted }) => {
  const [toolboxCollapsed, setToolboxCollapsed] = useState(false);
  const [toolboxSearch, setToolboxSearch] = useState('');
  const [selectedPath, setSelectedPath] = useState<RouterNodePath | null>(null);
  const [zoom, setZoom] = useState(1);
  const [pan, setPan] = useState({ x: 18, y: 18 });
  const [panning, setPanning] = useState(false);
  const [rejection, setRejection] = useState<string | null>(null);
  const [inspectorWidth, setInspectorWidth] = useState<number | null>(null);
  const [resizing, setResizing] = useState(false);
  const graphRef = useRef<HTMLDivElement>(null);
  const resizeStateRef = useRef<{ pointerId: number } | null>(null);
  const canvasRef = useRef<HTMLDivElement>(null);
  const panStateRef = useRef<{ pointerId: number; x: number; y: number } | null>(null);
  const historyRef = useRef<RouterNode[]>([]);
  const futureRef = useRef<RouterNode[]>([]);
  const lastEmittedNodeRef = useRef<RouterNode | null>(null);
  // True once the user has explicitly dropped or committed a node. Prevents a
  // freshly-dropped blank keywords_any (textValue='') from being hidden by the
  // isBlankRouterGraphNode check, which also matches the initial default state.
  // initialCommitted lets the expanded modal inherit committed state from the
  // inline instance for nodes that look blank but were explicitly placed.
  const hasCommittedRef = useRef(initialCommitted ?? !isBlankRouterGraphNode(node));
  const isMountedRef = useRef(false);

  const selectedNode = selectedPath ? routerNodeAtPath(node, selectedPath) : null;
  const blank = isBlankRouterGraphNode(node) && !hasCommittedRef.current;

  useEffect(() => {
    if (selectedPath && !routerNodeAtPath(node, selectedPath)) setSelectedPath(null);
  }, [node, selectedPath]);

  useEffect(() => {
    // Skip on initial mount — hasCommittedRef is correctly seeded by useRef
    // (using initialCommitted when provided, otherwise derived from the node).
    // Running on mount would overwrite that with a stale isBlankRouterGraphNode
    // check and drop a committed-but-empty keywords_any node on the canvas.
    if (!isMountedRef.current) {
      isMountedRef.current = true;
      return;
    }
    // Inline and expanded builders may edit the same rule. If this instance
    // receives a node it did not emit itself, its undo/redo snapshots belong to
    // an older tree and must not be allowed to overwrite the external change.
    if (lastEmittedNodeRef.current === node) {
      lastEmittedNodeRef.current = null;
      return;
    }
    historyRef.current = [];
    futureRef.current = [];
    hasCommittedRef.current = !isBlankRouterGraphNode(node);
    setSelectedPath(null);
  }, [node]);

  const emitChange = useCallback((next: RouterNode) => {
    lastEmittedNodeRef.current = next;
    onChange(next);
  }, [onChange]);

  const commit = useCallback((next: RouterNode) => {
    historyRef.current = [...historyRef.current.slice(-29), node];
    futureRef.current = [];
    hasCommittedRef.current = true;
    emitChange(next);
  }, [emitChange, node]);

  const reject = useCallback((message: string) => {
    setRejection(message);
    window.setTimeout(() => setRejection(current => current === message ? null : current), 1800);
  }, []);

  const addDataAtPath = useCallback((targetPath: RouterNodePath, data: RouterGraphDragData) => {
    const incoming = data.kind === 'operator' ? createEmptyRouterGraphGroup(data.operator) : createGraphLeaf(data);
    try {
      if (targetPath.length === 0 && blank) {
        commit(incoming);
        setSelectedPath([]);
        return;
      }
      const target = routerNodeAtPath(node, targetPath);
      if (!target) return;
      if (targetPath.length === 0 && target.kind === 'leaf' && data.kind === 'operator') {
        commit(wrapRouterNode(node, data.operator));
        setSelectedPath([]);
        return;
      }
      commit(appendRouterNodeAtPath(node, targetPath, incoming));
    } catch (error) {
      reject(error instanceof Error ? error.message : 'This item cannot be dropped here.');
    }
  }, [blank, commit, node, reject]);

  const removeAtPath = useCallback((path: RouterNodePath) => {
    const next = removeRouterNodeAtPath(node, path);
    if (isBlankRouterGraphNode(next)) {
      // Removal produced the canonical blank leaf — treat as a full reset so
      // the canvas returns to the empty-state placeholder.
      historyRef.current = [...historyRef.current.slice(-29), node];
      futureRef.current = [];
      hasCommittedRef.current = false;
      setSelectedPath(null);
      emitChange(next);
      return;
    }
    commit(next);
    setSelectedPath(null);
  }, [commit, emitChange, node]);

  const changeOperator = useCallback((path: RouterNodePath, operator: RouterGroupOperator) => {
    const current = routerNodeAtPath(node, path);
    if (!current || current.kind !== 'group' || current.operator === operator) return;
    if (operator === 'not' && current.children.length > 1) {
      // Negate the complete gate rather than dropping every child except the
      // first. The wrapper preserves the exact AND/OR subtree and all leaves.
      commit(replaceRouterNodeAtPath(node, path, wrapRouterNode(current, 'not')));
      return;
    }
    commit(replaceRouterNodeAtPath(node, path, { ...current, operator }));
  }, [commit, node]);

  const undo = () => {
    const previous = historyRef.current.at(-1);
    if (!previous) return;
    historyRef.current = historyRef.current.slice(0, -1);
    futureRef.current = [node, ...futureRef.current.slice(0, 29)];
    setSelectedPath(null);
    emitChange(previous);
  };

  const redo = () => {
    const next = futureRef.current[0];
    if (!next) return;
    futureRef.current = futureRef.current.slice(1);
    historyRef.current = [...historyRef.current.slice(-29), node];
    setSelectedPath(null);
    emitChange(next);
  };

  const normalizedSearch = toolboxSearch.trim().toLowerCase();
  const filteredLeaves = useMemo(() => TOOLBOX_LEAVES.filter(type =>
    !normalizedSearch || LEAF_LABELS[type].toLowerCase().includes(normalizedSearch)
  ), [normalizedSearch]);
  const filteredClassifiers = useMemo(() => classifiers.filter(classifier =>
    !normalizedSearch || `${classifier.id} classifier ${classifier.type}`.toLowerCase().includes(normalizedSearch)
  ), [classifiers, normalizedSearch]);
  const toolboxTargetPath = selectedNode?.kind === 'group' && selectedPath ? selectedPath : [];

  const beginDrag = (event: React.DragEvent, data: RouterGraphDragData) => {
    event.dataTransfer.setData(ROUTER_GRAPH_DRAG_MIME, encodeDrag(data));
    event.dataTransfer.effectAllowed = 'copy';
  };

  const clampZoom = (value: number) => Math.max(ZOOM_MIN, Math.min(ZOOM_MAX, Number(value.toFixed(2))));

  const onWheel = (event: React.WheelEvent<HTMLDivElement>) => {
    if (!canvasRef.current) return;
    // Keep the surrounding rule list scrollable. Zoom only when the user
    // explicitly holds the platform zoom modifier, while the +/- controls
    // remain available for pointer-only use.
    if (!event.ctrlKey && !event.metaKey) return;
    event.preventDefault();
    const rect = canvasRef.current.getBoundingClientRect();
    const nextZoom = clampZoom(zoom + (event.deltaY < 0 ? ZOOM_STEP : -ZOOM_STEP));
    if (nextZoom === zoom) return;
    const cursorX = event.clientX - rect.left;
    const cursorY = event.clientY - rect.top;
    const ratio = nextZoom / zoom;
    setPan(current => ({
      x: cursorX - (cursorX - current.x) * ratio,
      y: cursorY - (cursorY - current.y) * ratio,
    }));
    setZoom(nextZoom);
  };

  const onPointerDown = (event: React.PointerEvent<HTMLDivElement>) => {
    if (event.button !== 0) return;
    const target = event.target as HTMLElement;
    if (target.closest('button, input, select, textarea, .router-graph__group, .router-graph__drop-zone')) return;
    panStateRef.current = { pointerId: event.pointerId, x: event.clientX, y: event.clientY };
    setPanning(true);
    event.currentTarget.setPointerCapture(event.pointerId);
  };

  const onPointerMove = (event: React.PointerEvent<HTMLDivElement>) => {
    const state = panStateRef.current;
    if (!state || state.pointerId !== event.pointerId) return;
    const dx = event.clientX - state.x;
    const dy = event.clientY - state.y;
    panStateRef.current = { ...state, x: event.clientX, y: event.clientY };
    setPan(current => ({ x: current.x + dx, y: current.y + dy }));
  };

  const stopPanning = (event?: React.PointerEvent<HTMLDivElement>) => {
    if (event && panStateRef.current?.pointerId === event.pointerId && event.currentTarget.hasPointerCapture(event.pointerId)) {
      event.currentTarget.releasePointerCapture(event.pointerId);
    }
    panStateRef.current = null;
    setPanning(false);
  };

  const startResize = (event: React.PointerEvent<HTMLDivElement>) => {
    if (event.button !== 0) return;
    event.preventDefault();
    resizeStateRef.current = { pointerId: event.pointerId };
    event.currentTarget.setPointerCapture(event.pointerId);
    setResizing(true);
  };

  const onResizeMove = (event: React.PointerEvent<HTMLDivElement>) => {
    const state = resizeStateRef.current;
    if (!state || state.pointerId !== event.pointerId || !graphRef.current) return;
    const rect = graphRef.current.getBoundingClientRect();
    const max = Math.max(INSPECTOR_MIN_WIDTH, rect.width - WORKSPACE_MIN_WIDTH);
    const next = rect.right - event.clientX;
    setInspectorWidth(Math.round(Math.min(max, Math.max(INSPECTOR_MIN_WIDTH, next))));
  };

  const stopResize = (event: React.PointerEvent<HTMLDivElement>) => {
    if (resizeStateRef.current?.pointerId === event.pointerId && event.currentTarget.hasPointerCapture(event.pointerId)) {
      event.currentTarget.releasePointerCapture(event.pointerId);
    }
    resizeStateRef.current = null;
    setResizing(false);
  };

  return (
    <div className={`router-graph ${resizing ? 'is-resizing' : ''}`} ref={graphRef}>
      <div className={`router-graph__workspace ${toolboxCollapsed ? 'is-toolbox-collapsed' : ''}`}>
        <aside className={`router-graph__toolbox ${toolboxCollapsed ? 'is-collapsed' : ''}`} aria-label="Router condition toolbox">
          <button type="button" className="router-graph__toolbox-toggle" onClick={() => setToolboxCollapsed(current => !current)} aria-expanded={!toolboxCollapsed}>
            <Icon name={toolboxCollapsed ? 'panel-left-open' : 'panel-left-close'} size={14} />
            {!toolboxCollapsed && <span>Toolbox</span>}
          </button>
          {!toolboxCollapsed && (
            <div className="router-graph__toolbox-content">
              <div className="router-graph__toolbox-title">Logic Gates</div>
              <div className="router-graph__gates">
                {(['all', 'any', 'not'] as RouterGroupOperator[]).map(operator => {
                  const data: RouterGraphDragData = { kind: 'operator', operator };
                  return (
                    <button
                      type="button"
                      draggable
                      key={operator}
                      className={`router-graph__tool router-graph__tool--gate router-graph__tool--${operator}`}
                      onClick={() => addDataAtPath(toolboxTargetPath, data)}
                      onDragStart={event => beginDrag(event, data)}
                      title={`Drag ${OPERATOR_LABELS[operator]} onto the graph`}
                    >
                      <GraphGateShape operator={operator} compact />
                      <strong>{OPERATOR_LABELS[operator]}</strong>
                      <small>{operator === 'all' ? 'All match' : operator === 'any' ? 'Any match' : 'Negate'}</small>
                    </button>
                  );
                })}
              </div>

              <div className="router-graph__toolbox-title">Conditions</div>
              <div className="router-graph__toolbox-search">
                <Icon name="search" size={13} />
                <input value={toolboxSearch} placeholder="Search conditions" onChange={event => setToolboxSearch(event.target.value)} />
              </div>
              <div className="router-graph__tools">
                {filteredLeaves.map(type => {
                  const data: RouterGraphDragData = { kind: 'leaf', leafType: type };
                  return (
                    <button
                      type="button"
                      draggable
                      key={type}
                      className={`router-graph__tool router-graph__tool--condition router-graph__tool--${type}`}
                      onClick={() => addDataAtPath(toolboxTargetPath, data)}
                      onDragStart={event => beginDrag(event, data)}
                      title={`Drag or click to add ${LEAF_LABELS[type]}`}
                    >
                      {LEAF_LABELS[type]}
                    </button>
                  );
                })}
              </div>

              {classifiers.length > 0 && (
                <>
                  <div className="router-graph__toolbox-title">Classifiers</div>
                  <div className="router-graph__tools">
                    {filteredClassifiers.map(classifier => {
                      const data: RouterGraphDragData = { kind: 'leaf', leafType: 'classifier', classifierId: classifier.id };
                      return (
                        <button
                          type="button"
                          draggable
                          key={classifier.id}
                          className="router-graph__tool router-graph__tool--condition router-graph__tool--classifier"
                          onClick={() => addDataAtPath(toolboxTargetPath, data)}
                          onDragStart={event => beginDrag(event, data)}
                          title={`Drag or click to add classifier ${classifier.id}`}
                        >
                          <span>clf:</span> {classifier.id}
                        </button>
                      );
                    })}
                    {filteredClassifiers.length === 0 && normalizedSearch && <div className="router-graph__toolbox-empty">No matching classifiers.</div>}
                  </div>
                </>
              )}
            </div>
          )}
        </aside>

        <div className="router-graph__canvas-shell">
          <div className="router-graph__toolbar">
            <button type="button" disabled={historyRef.current.length === 0} onClick={undo}>Undo</button>
            <button type="button" disabled={futureRef.current.length === 0} onClick={redo}>Redo</button>
            {!blank && <button type="button" className="is-danger" onClick={() => {
              historyRef.current = [...historyRef.current.slice(-29), node];
              futureRef.current = [];
              hasCommittedRef.current = false;
              setSelectedPath(null);
              emitChange(createRouterLeaf());
            }}>Clear</button>}
            {rejection && <span className="router-graph__rejection" role="status"><Icon name="alert" size={12} /> {rejection}</span>}
            <span className="router-graph__toolbar-hint">Drag items from the toolbox. Select a gate and click a toolbox item to add there. Ctrl/⌘ + wheel zooms.</span>
            <span className="router-graph__toolbar-spacer" />
            {onExpand && (
              <button type="button" className="router-graph__expand" onClick={onExpand} title="Expand graph builder" aria-label="Expand graph builder">
                <Icon name="maximize-2" size={12} />
              </button>
            )}
            <button type="button" disabled={zoom <= ZOOM_MIN} onClick={() => setZoom(current => clampZoom(current - ZOOM_STEP))}>−</button>
            <button type="button" className="router-graph__zoom-label" onClick={() => { setZoom(1); setPan({ x: 18, y: 18 }); }}>{Math.round(zoom * 100)}%</button>
            <button type="button" disabled={zoom >= ZOOM_MAX} onClick={() => setZoom(current => clampZoom(current + ZOOM_STEP))}>+</button>
          </div>
          <div
            ref={canvasRef}
            className={`router-graph__canvas ${panning ? 'is-panning' : ''}`}
            onWheel={onWheel}
            onPointerDown={onPointerDown}
            onPointerMove={onPointerMove}
            onPointerUp={stopPanning}
            onPointerCancel={stopPanning}
            onPointerLeave={event => { if (panStateRef.current) stopPanning(event); }}
            onDragOver={event => { event.preventDefault(); event.dataTransfer.dropEffect = 'copy'; }}
            onDrop={event => {
              event.preventDefault();
              const data = decodeDrag(event.dataTransfer.getData(ROUTER_GRAPH_DRAG_MIME));
              if (data) addDataAtPath([], data);
            }}
          >
            {blank ? (
              <div className="router-graph__empty">
                <span className="router-graph__empty-icon"><Icon name="router" size={22} /></span>
                <strong>Build the first condition</strong>
                <span>Drag a logic gate or condition here, or click an item in the Toolbox.</span>
                <GraphDropZone path={[]} label="Drop first node here" onDropData={addDataAtPath} />
              </div>
            ) : (
              <div className="router-graph__canvas-transform" style={{ transform: `translate(${pan.x}px, ${pan.y}px) scale(${zoom})` }}>
                <GraphNode
                  node={node}
                  path={[]}
                  classifiers={classifiers}
                  selectedPath={selectedPath}
                  onSelect={setSelectedPath}
                  onRemove={removeAtPath}
                  onChangeOperator={changeOperator}
                  onDropData={addDataAtPath}
                />
              </div>
            )}
          </div>
        </div>
      </div>

      {selectedNode && expanded && (
        <div
          className="router-graph__resizer"
          role="separator"
          aria-orientation="vertical"
          aria-label="Resize node inspector"
          aria-valuemin={INSPECTOR_MIN_WIDTH}
          aria-valuemax={INSPECTOR_MIN_WIDTH + (graphRef.current ? Math.max(0, graphRef.current.offsetWidth - WORKSPACE_MIN_WIDTH - INSPECTOR_MIN_WIDTH) : 400)}
          aria-valuenow={inspectorWidth ?? INSPECTOR_MIN_WIDTH}
          tabIndex={0}
          title="Drag to resize · double-click to reset"
          onPointerDown={startResize}
          onPointerMove={onResizeMove}
          onPointerUp={stopResize}
          onPointerCancel={stopResize}
          onDoubleClick={() => setInspectorWidth(null)}
          onKeyDown={e => {
            const STEP = 20;
            if (e.key === 'ArrowLeft') {
              e.preventDefault();
              if (!graphRef.current) return;
              const rect = graphRef.current.getBoundingClientRect();
              const max = Math.max(INSPECTOR_MIN_WIDTH, rect.width - WORKSPACE_MIN_WIDTH);
              setInspectorWidth(w => Math.round(Math.min(max, Math.max(INSPECTOR_MIN_WIDTH, (w ?? INSPECTOR_MIN_WIDTH) + STEP))));
            } else if (e.key === 'ArrowRight') {
              e.preventDefault();
              setInspectorWidth(w => Math.round(Math.max(INSPECTOR_MIN_WIDTH, (w ?? INSPECTOR_MIN_WIDTH) - STEP)));
            } else if (e.key === 'Home') {
              e.preventDefault();
              setInspectorWidth(null);
            }
          }}
        >
          <span className="router-graph__resizer-grip" aria-hidden="true">
            <svg width="14" height="14" viewBox="0 0 24 24" fill="currentColor">
              <circle cx="9" cy="5" r="1.4" /><circle cx="9" cy="12" r="1.4" /><circle cx="9" cy="19" r="1.4" />
              <circle cx="15" cy="5" r="1.4" /><circle cx="15" cy="12" r="1.4" /><circle cx="15" cy="19" r="1.4" />
            </svg>
          </span>
        </div>
      )}

      {selectedNode && (
        <div
          className="router-graph__inspector"
          style={expanded && inspectorWidth != null ? { flex: `0 0 ${inspectorWidth}px` } : undefined}
        >
          <div className="router-graph__inspector-head">
            <div>
              <strong>Node Inspector</strong>
              <small>Edit the selected condition or gate without leaving the graph.</small>
            </div>
            <button type="button" onClick={() => setSelectedPath(null)} aria-label="Close node inspector"><Icon name="x" size={13} /></button>
          </div>
          <div className="router-graph__inspector-body">
            <RouterNodeEditor
              node={selectedNode}
              classifiers={classifiers}
              onChange={next => {
                if (!selectedPath) return;
                const conflict = routerReplacementConflictAtPath(node, selectedPath, next);
                if (conflict) {
                  reject(conflict);
                  return;
                }
                commit(replaceRouterNodeAtPath(node, selectedPath, next));
              }}
            />
          </div>
        </div>
      )}
    </div>
  );
};

export default RouterRuleGraph;
