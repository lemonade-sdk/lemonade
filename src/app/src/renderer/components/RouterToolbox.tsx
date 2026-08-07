import React, { useState } from 'react';
import {
  DRAG_MIME,
  encodeDrag,
  OPERATOR_COLORS,
  SIGNAL_COLORS,
  SIGNAL_LABELS,
  type ConditionSignalType,
  type DragData,
  type RuleOperator,
} from '../utils/routerTree';
import { RouterClassifier } from '../utils/customCollections';

const BASE_SIGNALS: ConditionSignalType[] = [
  'keywords_any', 'keywords_all', 'regex',
  'min_chars', 'max_chars', 'has_tools', 'has_images',
];


const OPERATORS: RuleOperator[] = ['AND', 'OR', 'NOT'];

const OPERATOR_DESC: Record<RuleOperator, string> = {
  AND: 'All children must match',
  OR: 'Any child must match',
  NOT: 'Child must NOT match',
};

// Gate SVG thumbnails - shield/lens/slash shapes distinct from standard logic-gate iconography
const GateSVG: React.FC<{ op: RuleOperator; color: string }> = ({ op, color }) => {
  const fill = color + '18';
  if (op === 'AND') return (
    <svg viewBox="0 0 40 32" width="28" height="20">
      <path d="M3 2 L37 2 L37 14 L20 30 L3 14 Z"
        fill={fill} stroke={color} strokeWidth="1.8" strokeLinejoin="round" />
    </svg>
  );
  if (op === 'OR') return (
    <svg viewBox="0 0 40 28" width="28" height="20">
      <path d="M20 2 Q37 6 37 14 Q37 22 20 26 Q3 22 3 14 Q3 6 20 2 Z"
        fill={fill} stroke={color} strokeWidth="1.8" strokeLinejoin="round" />
    </svg>
  );
  return (
    <svg viewBox="0 0 32 32" width="22" height="22">
      <circle cx="16" cy="16" r="13" fill={fill} stroke={color} strokeWidth="1.8" />
      <line x1="5" y1="5" x2="27" y2="27" stroke={color} strokeWidth="1.8" strokeLinecap="round" />
    </svg>
  );
};

interface RouterToolboxProps {
  classifiers: RouterClassifier[];
  collapsed: boolean;
  onToggle: () => void;
  isFullscreen?: boolean;
  previewJson?: string | null;
  onPreviewJson?: () => void;
  error?: string | null;
  onChipClick?: (data: DragData) => void;
}

const RouterToolbox: React.FC<RouterToolboxProps> = ({ classifiers, collapsed, onToggle, isFullscreen, previewJson, onPreviewJson, error, onChipClick }) => {
  const [search, setSearch] = useState('');
  const q = search.toLowerCase();

  const filteredSignals = BASE_SIGNALS.filter(t =>
    !q || SIGNAL_LABELS[t].toLowerCase().includes(q)
  );
  // Only show classifiers that are ready to use (model set + concepts for semantic_similarity)
  const readyClassifiers = classifiers.filter(c => {
    if (!c.model) return false;
    if (c.type === 'semantic_similarity') return Object.keys(c.referencePhrases ?? {}).length > 0;
    return true;
  });
  const filteredClassifiers = readyClassifiers.filter(c =>
    !q || c.id.toLowerCase().includes(q) || 'classifier'.includes(q)
  );

  const wide = isFullscreen && previewJson != null;

  return (
    <div className={`rtb-toolbox${collapsed ? ' rtb-toolbox--collapsed' : ''}${wide ? ' rtb-toolbox--wide' : ''}`}>
      <div className="rtb-header" onClick={onToggle} title={collapsed ? 'ExpandUtilities' : 'CollapseUtilities'}>
        <span className="rtb-header-title">{collapsed ? '◀' : '▶'}Utilities</span>
        {!collapsed && <span className="rtb-header-count">{BASE_SIGNALS.length + readyClassifiers.length} signals</span>}
      </div>

      {!collapsed && (
        <div className="rtb-content">
          <div className="rtb-section-title">Logic Gates</div>
          <div className="rtb-gates">
            {OPERATORS.map(op => {
              const color = OPERATOR_COLORS[op];
              return (
                <div
                  key={op}
                  className="rtb-gate"
                  style={{ '--gate-color': color } as React.CSSProperties}
                  draggable
                  title={`${op}: ${OPERATOR_DESC[op]}\nDrag onto canvas`}
                  onDragStart={e => {
                    e.dataTransfer.setData(DRAG_MIME, encodeDrag({ kind: 'operator', operator: op }));
                    e.dataTransfer.effectAllowed = 'copy';
                  }}
                >
                  <GateSVG op={op} color={color} />
                  <span className="rtb-gate-label" style={{ color }}>{op}</span>
                  <span className="rtb-gate-desc">{OPERATOR_DESC[op]}</span>
                </div>
              );
            })}
          </div>

          <div className="rtb-section-title" style={{ marginTop: 12 }}>Conditions</div>
          <div className="rtb-search-row">
            <input
              className="form-input rtb-search"
              value={search}
              onChange={e => setSearch(e.target.value)}
              placeholder="Search…"
            />
            {search && (
              <button type="button" className="rtb-search-clear" onClick={() => setSearch('')}>×</button>
            )}
          </div>

          <div className="rtb-signals">
            {filteredSignals.map(t => {
              const color = SIGNAL_COLORS[t];
              const data: DragData = { kind: 'leaf', leaf: { signalType: t, signalValue: t === 'min_chars' ? 500 : t === 'max_chars' ? 2000 : '' } };
              return (
                <div
                  key={t}
                  className="rtb-chip"
                  style={{ '--chip-color': color } as React.CSSProperties}
                  draggable
                  title={`${SIGNAL_LABELS[t]}\nDrag or click to add`}
                  onClick={() => onChipClick?.(data)}
                  onDragStart={e => {
                    e.dataTransfer.setData(DRAG_MIME, encodeDrag(data));
                    e.dataTransfer.effectAllowed = 'copy';
                  }}
                >
                  {SIGNAL_LABELS[t]}
                </div>
              );
            })}
          </div>

          {classifiers.length > 0 && (
            <>
              <div className="rtb-section-title" style={{ marginTop: 10 }}>Classifiers</div>
              <div className="rtb-signals">
                {filteredClassifiers.map(clf => {
                  const color = SIGNAL_COLORS.classifier;
                  return (
                    <div
                      key={clf.id}
                      className="rtb-chip rtb-chip--classifier"
                      style={{ '--chip-color': color } as React.CSSProperties}
                      draggable
                      title={`Classifier: ${clf.id}\nDrag or click to add`}
                      onClick={() => onChipClick?.({ kind: 'leaf', leaf: { signalType: 'classifier', classifierId: clf.id, minScore: 0.5 } })}
                      onDragStart={e => {
                        e.dataTransfer.setData(DRAG_MIME, encodeDrag({
                          kind: 'leaf',
                          leaf: { signalType: 'classifier', classifierId: clf.id, minScore: 0.5 },
                        }));
                        e.dataTransfer.effectAllowed = 'copy';
                      }}
                    >
                      <span className="rtb-chip-prefix">clf:</span> {clf.id}
                    </div>
                  );
                })}
                {filteredClassifiers.length === 0 && q && (
                  <span className="rtb-empty">No matching classifiers</span>
                )}
              </div>
            </>
          )}

          {filteredSignals.length === 0 && filteredClassifiers.length === 0 && (
            <span className="rtb-empty">No matching signals</span>
          )}

          {isFullscreen && onPreviewJson && (
            <div className="rtb-preview-section">
              <button type="button" className="settings-reset-button rtb-preview-btn" onClick={onPreviewJson}>
                {previewJson != null ? 'Hide JSON' : 'Preview JSON'}
              </button>
              {error && <div className="router-panel-error">{error}</div>}
              {previewJson != null && (
                <div className="router-json-preview rtb-preview-panel">
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
                        a.href = url; a.download = 'router.json'; a.click();
                        URL.revokeObjectURL(url);
                      }}>
                      <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                        <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/>
                      </svg>
                    </button>
                    <button type="button" className="router-json-preview-btn" title="Close" onClick={onPreviewJson}>
                      <svg width="12" height="12" viewBox="0 0 12 12" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round">
                        <path d="M1 1 L11 11 M11 1 L1 11"/>
                      </svg>
                    </button>
                  </div>
                  <pre className="router-json-preview-body">{previewJson}</pre>
                </div>
              )}
            </div>
          )}
        </div>
      )}
    </div>
  );
};

export default RouterToolbox;
