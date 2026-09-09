import React, { useCallback, useEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import {
  addChildAtPath,
  decodeDrag,
  DRAG_MIME,
  getNodeAtPath,
  isLeaf,
  isOperatorNode,
  leafSummary,
  makeDefaultLeaf,
  OPERATOR_COLORS,
  removeAtPath,
  replaceAtPath,
  RuleLeaf,
  RuleNode,
  RuleOperator,
  SIGNAL_COLORS,
  SIGNAL_LABELS,
  SINGLETON_SIGNALS,
  validateRuleNode,
  type ConditionSignalType,
  type DragData,
  type NodePath,
} from '../utils/routerTree';
import { RouterClassifier } from '../utils/customCollections';


interface LeafEditorProps {
  leaf: RuleLeaf;
  classifiers: RouterClassifier[];
  anchorEl: HTMLElement;
  onSave: (updated: RuleLeaf) => void;
  onClose: () => void;
}

const LeafEditor: React.FC<LeafEditorProps> = ({ leaf, classifiers, anchorEl, onSave, onClose }) => {
  const [val, setVal] = useState<RuleLeaf>({ ...leaf });
  const rect = anchorEl.getBoundingClientRect();
  const style: React.CSSProperties = {
    position: 'fixed', top: rect.bottom + 6, left: rect.left,
    zIndex: 10600, minWidth: 220,
  };

  const save = () => { onSave(val); onClose(); };

  return createPortal(
    <>
      <div style={{ position: 'fixed', inset: 0, zIndex: 10599 }} onMouseDown={onClose} />
      <div className="rtc-leaf-editor" style={style} onMouseDown={e => e.stopPropagation()}>
        {(val.signalType === 'keywords_any' || val.signalType === 'keywords_all') && (
          <>
            <label className="rtc-editor-label">Keywords <span className="rtc-editor-hint">(comma-separated)</span></label>
            <input className="form-input rtc-editor-input" autoFocus
              value={String(val.signalValue ?? '')}
              onChange={e => setVal(v => ({ ...v, signalValue: e.target.value }))}
              placeholder="word1, word2" />
          </>
        )}
        {val.signalType === 'regex' && (
          <>
            <label className="rtc-editor-label">Pattern</label>
            <input className="form-input rtc-editor-input" autoFocus
              value={String(val.signalValue ?? '')}
              onChange={e => setVal(v => ({ ...v, signalValue: e.target.value }))}
              placeholder="e.g. ```[a-z]*" />
          </>
        )}
        {(val.signalType === 'min_chars' || val.signalType === 'max_chars') && (
          <>
            <label className="rtc-editor-label">{val.signalType === 'min_chars' ? 'Min' : 'Max'} chars</label>
            <input className="form-input rtc-editor-input" autoFocus type="number" min={0} step={1}
              value={val.signalValue ?? ''}
              onChange={e => {
                if (!e.target.value) { setVal(v => ({ ...v, signalValue: undefined })); return; }
                const n = parseInt(e.target.value, 10);
                if (!isNaN(n)) setVal(v => ({ ...v, signalValue: Math.max(0, n) }));
              }} />
          </>
        )}
        {(val.signalType === 'has_tools' || val.signalType === 'has_images') && (
          <p className="rtc-editor-hint" style={{ margin: 0 }}>
            Matches when the request {val.signalType === 'has_tools' ? 'includes tools[]' : 'includes images'}.
          </p>
        )}
        {val.signalType === 'classifier' && (
          <>
            <label className="rtc-editor-label">Classifier</label>
            <select className="form-input form-select rtc-editor-input" autoFocus
              value={val.classifierId ?? ''}
              onChange={e => setVal(v => ({ ...v, classifierId: e.target.value }))}>
              <option value="">Select…</option>
              {classifiers.map(c => <option key={c.id} value={c.id}>{c.id}</option>)}
            </select>
            {val.classifierId && (() => {
              const clf = classifiers.find(c => c.id === val.classifierId);
              const labels = clf?.type === 'semantic_similarity'
                ? Object.keys(clf.referencePhrases ?? {})
                : clf?.labels ?? [];
              if (labels.length === 0) return null;
              // Only offer "default" option if the classifier has an explicit defaultLabel
              const hasDefault = !!clf?.defaultLabel;
              return (
                <>
                  <label className="rtc-editor-label" style={{ marginTop: 6 }}>Label</label>
                  <select className="form-input form-select rtc-editor-input"
                    value={val.label ?? ''}
                    onChange={e => setVal(v => ({ ...v, label: e.target.value || undefined }))}>
                    {hasDefault && <option value="">default ({clf!.defaultLabel})</option>}
                    {labels.map(l => <option key={l} value={l}>{l}</option>)}
                  </select>
                </>
              );
            })()}
            <div style={{ display: 'flex', gap: 6, marginTop: 6 }}>
              <div style={{ flex: 1 }}>
                <label className="rtc-editor-label">Min score</label>
                <input className="form-input rtc-editor-input" type="number" min={0} max={val.maxScore ?? 1} step={0.05}
                  value={val.minScore ?? 0.5}
                  onChange={e => {
                    const n = parseFloat(e.target.value);
                    if (isNaN(n)) return;
                    setVal(v => ({ ...v, minScore: Math.min(Math.max(0, n), v.maxScore ?? 1) }));
                  }} />
              </div>
              <div style={{ flex: 1 }}>
                <label className="rtc-editor-label">Max score</label>
                <input className="form-input rtc-editor-input" type="number" min={val.minScore ?? 0} max={1} step={0.05}
                  value={val.maxScore ?? ''}
                  onChange={e => {
                    if (!e.target.value) { setVal(v => ({ ...v, maxScore: undefined })); return; }
                    const n = parseFloat(e.target.value);
                    if (isNaN(n)) return;
                    setVal(v => ({ ...v, maxScore: Math.min(Math.max(v.minScore ?? 0, n), 1) }));
                  }}
                  placeholder="none" />
              </div>
            </div>
          </>
        )}
        <div style={{ display: 'flex', gap: 6, marginTop: 8 }}>
          <label className="rtc-editor-label" style={{ display: 'flex', alignItems: 'center', gap: 4, margin: 0 }}>
            <input type="checkbox" checked={val.not === true}
              onChange={e => setVal(v => ({ ...v, not: e.target.checked || undefined }))} />
            NOT (negate)
          </label>
          <div style={{ flex: 1 }} />
          <button type="button" className="settings-save-button" style={{ fontSize: '0.72rem', padding: '2px 10px' }}
            onClick={save}>Done</button>
        </div>
      </div>
    </>,
    document.body,
  );
};


interface TreeNodeProps {
  node: RuleNode;
  path: NodePath;
  classifiers: RouterClassifier[];
  onDrop: (targetPath: NodePath, data: ReturnType<typeof decodeDrag>) => string | null;
  onReject: (reason: string) => void;
  onEdit: (path: NodePath, el: HTMLElement) => void;
  onDelete: (path: NodePath) => void;
  onChangeOp: (path: NodePath, op: RuleOperator) => void;
}

const TreeNode: React.FC<TreeNodeProps> = ({
  node, path, classifiers, onDrop, onReject, onEdit, onDelete, onChangeOp,
}) => {
  const elRef = useRef<HTMLDivElement>(null);

  if (isLeaf(node)) {
    const color = SIGNAL_COLORS[node.signalType];
    const label = SIGNAL_LABELS[node.signalType];
    const summary = leafSummary(node);
    return (
      <div
        ref={elRef}
        className={`rtc-leaf${node.not ? ' rtc-leaf--negated' : ''}`}
        style={{ '--node-color': color } as React.CSSProperties}
        onDoubleClick={e => { e.stopPropagation(); if (elRef.current) onEdit(path, elRef.current); }}
      >
        <div className="rtc-leaf-type">{node.not ? <span className="rtc-not-badge">¬</span> : null}{label}</div>
        <div className="rtc-leaf-summary">{summary}</div>
        <button type="button" className="rtc-delete-btn" title="Remove" onClick={e => { e.stopPropagation(); onDelete(path); }}>×</button>
      </div>
    );
  }

  // Operator node
  const color = OPERATOR_COLORS[node.operator];
  const canAddChild = node.operator !== 'NOT' || node.conditions.length === 0;

  return (
    <div
      className="rtc-operator"
      style={{ '--node-color': color } as React.CSSProperties}
    >
      <div className="rtc-gate-header">
        <GateShape op={node.operator} color={color} />
        <div className="rtc-gate-controls">
          {node.operator !== 'NOT' && (
            <>
              <button type="button"
                className={`rtc-op-pill${node.operator === 'AND' ? ' active' : ''}`}
                style={node.operator === 'AND' ? { background: color + '22', color } : {}}
                onClick={e => { e.stopPropagation(); onChangeOp(path, 'AND'); }}>AND</button>
              <button type="button"
                className={`rtc-op-pill${node.operator === 'OR' ? ' active' : ''}`}
                style={node.operator === 'OR' ? { background: color + '22', color } : {}}
                onClick={e => { e.stopPropagation(); onChangeOp(path, 'OR'); }}>OR</button>
            </>
          )}
          {node.operator === 'NOT' && <span className="rtc-gate-label" style={{ color }}>NOT</span>}
          {path.length > 0 && (
            <button type="button" className="rtc-gate-delete-btn" title="Remove gate and all its children"
              onClick={e => { e.stopPropagation(); onDelete(path); }}>×</button>
          )}
        </div>
      </div>

      <div className="rtc-children">
        {node.conditions.length === 0 && canAddChild && (
          <DropZone path={path} onDrop={onDrop} onReject={onReject} />
        )}
        {node.conditions.map((child, i) => (
          <TreeNode
            key={i}
            node={child}
            path={[...path, i]}
            classifiers={classifiers}
            onDrop={onDrop}
            onReject={onReject}
            onEdit={onEdit}
            onDelete={onDelete}
            onChangeOp={onChangeOp}
          />
        ))}
        {node.conditions.length > 0 && canAddChild && (
          <DropZone path={path} onDrop={onDrop} onReject={onReject} />
        )}
      </div>
    </div>
  );
};


const GateShape: React.FC<{ op: RuleOperator; color: string }> = ({ op, color }) => {
  const fill = color.startsWith('#') ? color + '15' : 'transparent';
  if (op === 'AND') return (
    <svg viewBox="0 0 56 44" width="36" height="28" className="rtc-gate-svg">
      <path d="M4 3 L52 3 L52 20 L28 41 L4 20 Z"
        fill={fill} stroke={color} strokeWidth="2" strokeLinejoin="round" />
    </svg>
  );
  if (op === 'OR') return (
    <svg viewBox="0 0 56 40" width="36" height="24" className="rtc-gate-svg">
      <path d="M28 3 Q52 8 52 20 Q52 32 28 37 Q4 32 4 20 Q4 8 28 3 Z"
        fill={fill} stroke={color} strokeWidth="2" strokeLinejoin="round" />
    </svg>
  );
  return (
    <svg viewBox="0 0 44 44" width="28" height="28" className="rtc-gate-svg">
      <circle cx="22" cy="22" r="18" fill={fill} stroke={color} strokeWidth="2" />
      <line x1="8" y1="8" x2="36" y2="36" stroke={color} strokeWidth="2" strokeLinecap="round" />
    </svg>
  );
};


const DropZone: React.FC<{ path: NodePath; onDrop: TreeNodeProps['onDrop']; onReject: (reason: string) => void }> = ({ path, onDrop, onReject }) => {
  const [over, setOver] = useState(false);
  return (
    <div
      className={`rtc-dropzone${over ? ' rtc-dropzone--over' : ''}`}
      onDragOver={e => { e.preventDefault(); e.stopPropagation(); setOver(true); }}
      onDragLeave={() => setOver(false)}
      onDrop={e => {
        e.preventDefault(); e.stopPropagation(); setOver(false);
        const raw = e.dataTransfer.getData(DRAG_MIME);
        if (!raw) return;
        const reason = onDrop(path, decodeDrag(raw));
        if (reason) onReject(reason);
      }}
    >
      drop here
    </div>
  );
};


// ── Warning badge with click-to-open popover ─────────────────────────────

const WarnBadge: React.FC<{ warnings: string[] }> = ({ warnings }) => {
  const [open, setOpen] = useState(false);
  return (
    <div className="rtc-warn-anchor">
      <span className="rtc-warn-badge" onClick={() => setOpen(v => !v)} style={{ cursor: 'pointer' }}>
        ⚠ {warnings.length}
      </span>
      {open && (
        <>
          <div style={{ position: 'fixed', inset: 0, zIndex: 10698 }} onClick={() => setOpen(false)} />
          <div className="rtc-warn-popover rtc-warn-popover--open">
            {warnings.map((w, i) => <div key={i} className="rtc-warn-popover-item">⚠ {w}</div>)}
          </div>
        </>
      )}
    </div>
  );
};

const ZOOM_STEP = 0.15;
const ZOOM_MIN = 0.3;
const ZOOM_MAX = 2.5;

interface RouterRuleCanvasProps {
  tree: RuleNode | null;
  classifiers: RouterClassifier[];
  onChange: (tree: RuleNode | null) => void;
  onExpand?: () => void;
  onChipClick?: (handler: (data: DragData) => void) => void;
}

const RouterRuleCanvas: React.FC<RouterRuleCanvasProps> = ({ tree, classifiers, onChange, onExpand, onChipClick }) => {
  const [editState, setEditState] = useState<{ path: NodePath; el: HTMLElement } | null>(null);
  const [rejected, setRejected] = useState<string | null>(null);
  const [zoom, setZoom] = useState(1);
  const [pan, setPan] = useState({ x: 0, y: 0 });
  const [panning, setPanning] = useState(false);
  const canvasRef = useRef<HTMLDivElement>(null);
  const isPanningRef = useRef(false);
  const lastPointerRef = useRef({ x: 0, y: 0 });
  const viewRef = useRef({ zoom: 1, pan: { x: 0, y: 0 } });

  // Keep viewRef in sync so wheel handler can read latest without stale closure
  viewRef.current = { zoom, pan };

  const clampZoom = (z: number) => Math.min(ZOOM_MAX, Math.max(ZOOM_MIN, parseFloat(z.toFixed(2))));
  const zoomIn  = () => setZoom(z => clampZoom(z + ZOOM_STEP));
  const zoomOut = () => setZoom(z => clampZoom(z - ZOOM_STEP));
  const zoomReset = () => { setZoom(1); setPan({ x: 0, y: 0 }); };

  // Attach wheel listener as non-passive (required for preventDefault) via ref callback
  const canvasRefCallback = useCallback((el: HTMLDivElement | null) => {
    if (canvasRef.current) {
      canvasRef.current.removeEventListener('wheel', onWheel);
    }
    canvasRef.current = el;
    if (el) {
      el.addEventListener('wheel', onWheel, { passive: false });
    }
  }, []);

  function onWheel(e: WheelEvent) {
    e.preventDefault();
    const canvas = canvasRef.current;
    if (!canvas) return;
    const rect = canvas.getBoundingClientRect();
    const { zoom: z, pan: p } = viewRef.current;
    const factor = e.deltaY < 0 ? 1 + ZOOM_STEP : 1 - ZOOM_STEP;
    const nextZoom = clampZoom(z * factor);
    const cursorX = e.clientX - rect.left;
    const cursorY = e.clientY - rect.top;
    // Keep the point under the cursor fixed while zooming
    const nextPanX = cursorX - (cursorX - p.x) * (nextZoom / z);
    const nextPanY = cursorY - (cursorY - p.y) * (nextZoom / z);
    setZoom(nextZoom);
    setPan({ x: nextPanX, y: nextPanY });
  }

  // Pan: left-drag on canvas background (not on nodes/buttons)
  const onPointerDown = useCallback((e: React.PointerEvent<HTMLDivElement>) => {
    if (e.button !== 0) return;
    const target = e.target as HTMLElement;
    if (target.closest('.rtc-operator, .rtc-leaf, .rtc-dropzone, .rtc-leaf-editor, button')) return;
    isPanningRef.current = true;
    setPanning(true);
    lastPointerRef.current = { x: e.clientX, y: e.clientY };
    e.currentTarget.setPointerCapture(e.pointerId);
  }, []);

  const onPointerMove = useCallback((e: React.PointerEvent<HTMLDivElement>) => {
    if (!isPanningRef.current) return;
    const dx = e.clientX - lastPointerRef.current.x;
    const dy = e.clientY - lastPointerRef.current.y;
    lastPointerRef.current = { x: e.clientX, y: e.clientY };
    setPan(p => ({ x: p.x + dx, y: p.y + dy }));
  }, []);

  const onPointerUp = useCallback(() => {
    isPanningRef.current = false;
    setPanning(false);
  }, []);

  const historyRef = useRef<(RuleNode | null)[]>([]);
  const futureRef = useRef<(RuleNode | null)[]>([]);

  const push = (prev: RuleNode | null) => {
    historyRef.current = [...historyRef.current.slice(-30), prev];
    futureRef.current = [];
  };
  const undo = () => {
    const stack = historyRef.current;
    if (!stack.length) return;
    const prev = stack[stack.length - 1];
    historyRef.current = stack.slice(0, -1);
    futureRef.current = [tree, ...futureRef.current.slice(0, 29)];
    onChange(prev);
  };
  const redo = () => {
    const future = futureRef.current;
    if (!future.length) return;
    const next = future[0];
    futureRef.current = future.slice(1);
    historyRef.current = [...historyRef.current.slice(-30), tree];
    onChange(next);
  };

  const classifierIds = new Set(classifiers.map(c => c.id));

  // Classifier definition warnings — surface independently of the rule tree
  const classifierDefWarnings: string[] = classifiers.flatMap(c => {
    if (c.type === 'semantic_similarity') {
      const concepts = Object.keys(c.referencePhrases ?? {});
      if (concepts.length === 0) return [`Classifier "${c.id}" needs at least one concept before it can be used`];
    }
    if (c.type === 'llm' && (c.labels ?? []).length === 0) {
      return [`Classifier "${c.id}" needs at least one label before it can be used`];
    }
    if (!c.model) return [`Classifier "${c.id}" has no model selected`];
    return [];
  });

  const warnings = [
    ...classifierDefWarnings,
    ...(tree ? validateRuleNode(tree, classifierIds, classifiers) : []),
  ];

  const handleDrop = useCallback((targetPath: NodePath, data: ReturnType<typeof decodeDrag>): string | null => {
    if (!data || data.kind === 'tree-node') return null;

    let newNode: RuleNode;
    if (data.kind === 'operator') {
      newNode = data.operator === 'NOT'
        ? { operator: 'NOT', conditions: [] }
        : { operator: data.operator, conditions: [] };
    } else {
      newNode = makeDefaultLeaf(data.leaf.signalType as ConditionSignalType, data.leaf.classifierId);
      Object.assign(newNode, data.leaf);
    }

    if (!tree) {
      push(tree);
      onChange(newNode);
      return null;
    }

    if (targetPath.length === 0) {
      if (data.kind === 'leaf' && SINGLETON_SIGNALS.has(data.leaf.signalType as ConditionSignalType) &&
          isOperatorNode(tree) && tree.conditions.some(c => isLeaf(c) && c.signalType === data.leaf.signalType)) {
        return `${SIGNAL_LABELS[data.leaf.signalType as ConditionSignalType]} is already in this gate`;
      }
      push(tree);
      if (isOperatorNode(tree) && tree.operator !== 'NOT') {
        onChange({ ...tree, conditions: [...tree.conditions, newNode] });
      } else if (data.kind === 'operator' && data.operator !== 'NOT') {
        onChange({ operator: data.operator, conditions: [tree] });
      } else {
        onChange({ operator: 'AND', conditions: [tree, newNode] });
      }
      return null;
    }

    if (data.kind === 'leaf' && SINGLETON_SIGNALS.has(data.leaf.signalType as ConditionSignalType)) {
      const targetNode = getNodeAtPath(tree, targetPath);
      if (targetNode && isOperatorNode(targetNode) &&
          targetNode.conditions.some(c => isLeaf(c) && c.signalType === data.leaf.signalType)) {
        return `${SIGNAL_LABELS[data.leaf.signalType as ConditionSignalType]} is already in this gate`;
      }
    }

    push(tree);
    onChange(addChildAtPath(tree, targetPath, newNode));
    return null;
  }, [tree, onChange]);

  const triggerRejection = useCallback((reason: string) => {
    setRejected(reason);
    setTimeout(() => setRejected(null), 400);
  }, []);

  const handleEdit = useCallback((path: NodePath, el: HTMLElement) => {
    setEditState({ path, el });
  }, []);

  const handleEditSave = useCallback((updated: RuleLeaf) => {
    if (!editState || !tree) return;
    push(tree);
    onChange(replaceAtPath(tree, editState.path, updated));
    setEditState(null);
  }, [editState, tree, onChange]);

  const handleDelete = useCallback((path: NodePath) => {
    if (!tree) return;
    push(tree);
    onChange(removeAtPath(tree, path));
  }, [tree, onChange]);

  const handleChangeOp = useCallback((path: NodePath, op: RuleOperator) => {
    if (!tree) return;
    const node = getNodeAtPath(tree, path);
    if (!node || !isOperatorNode(node)) return;
    push(tree);
    onChange(replaceAtPath(tree, path, { ...node, operator: op }));
  }, [tree, onChange]);

  // Root canvas drop
  const handleCanvasDrop = useCallback((e: React.DragEvent) => {
    e.preventDefault();
    const raw = e.dataTransfer.getData(DRAG_MIME);
    if (!raw) return;
    const reason = handleDrop([], decodeDrag(raw));
    if (reason) triggerRejection(reason);
  }, [handleDrop, triggerRejection]);

  // Chip click: drop onto root canvas (used by toolbox onClick)
  const handleChipClick = useCallback((data: DragData) => {
    const reason = handleDrop([], data);
    if (reason) triggerRejection(reason);
  }, [handleDrop, triggerRejection]);

  // Register click handler with parent toolbox via callback prop
  const onChipClickRef = useRef(handleChipClick);
  onChipClickRef.current = handleChipClick;
  useEffect(() => {
    if (onChipClick) onChipClick((data) => onChipClickRef.current(data));
  }, [onChipClick]);

  return (
    <div className="rtc-canvas-root">
      <div className="rtc-toolbar">
        <button type="button" className="rtc-toolbar-btn" onClick={undo} disabled={!historyRef.current.length} title="Undo">↩ Undo</button>
        <button type="button" className="rtc-toolbar-btn" onClick={redo} disabled={!futureRef.current.length} title="Redo">↻ Redo</button>
        {tree && (
          <button type="button" className="rtc-toolbar-btn rtc-toolbar-btn--danger"
            onClick={() => { push(tree); onChange(null); }} title="Clear expression">Clear</button>
        )}
        {warnings.length > 0 && (
          <WarnBadge warnings={warnings} />
        )}
        <span className="rtc-toolbar-hint">Double-click a leaf to edit</span>
        <div style={{ flex: 1 }} />
        {onExpand && (
          <button type="button" className="rtc-toolbar-btn rtc-toolbar-btn--expand" onClick={onExpand} title="Expand builder">
            <svg width="11" height="11" viewBox="0 0 12 12" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round">
              <path d="M1 4.5V1H4.5M7.5 1H11V4.5M11 7.5V11H7.5M4.5 11H1V7.5" />
            </svg>
          </button>
        )}
      </div>

      <div
        ref={canvasRefCallback}
        className={`rtc-canvas${panning ? ' rtc-canvas--panning' : ''}${rejected ? ' rtc-canvas--rejected' : ''}`}
        title={rejected || undefined}
        onDragOver={e => { e.preventDefault(); e.dataTransfer.dropEffect = 'copy'; }}
        onDrop={handleCanvasDrop}
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={onPointerUp}
        onPointerLeave={onPointerUp}
      >
        {tree ? (
          <div className="rtc-canvas-scaler" style={{ transform: `translate(${pan.x}px, ${pan.y}px) scale(${zoom})`, transformOrigin: '0 0' }}>
            <TreeNode
              node={tree}
              path={[]}
              classifiers={classifiers}
              onDrop={handleDrop}
              onReject={triggerRejection}
              onEdit={handleEdit}
              onDelete={handleDelete}
              onChangeOp={handleChangeOp}
            />
          </div>
        ) : (
          <div className="rtc-empty-state">
            <div className="rtc-empty-icon">⊕</div>
            <div className="rtc-empty-text">Drag AND / OR / NOT gates and condition signals here</div>
            <div className="rtc-empty-hint">or drag directly from the Toolbox on the right</div>
          </div>
        )}
      </div>

      <div className="rtc-zoom-controls">
        <button type="button" className="rtc-zoom-btn" onClick={zoomOut} title="Zoom out" disabled={zoom <= ZOOM_MIN}>−</button>
        <button type="button" className="rtc-zoom-label" onClick={zoomReset} title="Reset zoom">{Math.round(zoom * 100)}%</button>
        <button type="button" className="rtc-zoom-btn" onClick={zoomIn} title="Zoom in" disabled={zoom >= ZOOM_MAX}>+</button>
      </div>

      {editState && tree && (() => {
        const node = getNodeAtPath(tree, editState.path);
        if (!node || !isLeaf(node)) return null;
        return (
          <LeafEditor
            leaf={node}
            classifiers={classifiers}
            anchorEl={editState.el}
            onSave={handleEditSave}
            onClose={() => setEditState(null)}
          />
        );
      })()}
    </div>
  );
};

export default RouterRuleCanvas;
