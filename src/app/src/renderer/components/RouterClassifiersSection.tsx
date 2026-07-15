import React, { useState } from 'react';
import { createPortal } from 'react-dom';
import { RouterClassifier } from '../utils/customCollections';
import { ModelSelect } from './ModelSearchPicker';

interface CandidateOption {
  id: string;
  info: { model_name?: string; downloaded?: boolean };
}

const slugify = (s: string) =>
  s.trim().toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '') || 'clf';

const TYPE_BADGE: Record<RouterClassifier['type'], string> = {
  classifier: 'clf',
  semantic_similarity: 'sem',
  llm: 'llm',
};

export type ClassifierTemplateKey = 'pii' | 'topic';

interface ClassifierTemplateDef {
  key: ClassifierTemplateKey;
  emoji: string;
  label: string;
  title: string;
}

export const CLASSIFIER_TEMPLATES: ClassifierTemplateDef[] = [
  { key: 'pii', emoji: '🔒', label: 'PII Detector',
    title: 'LLM classifier that flags personally identifiable information (labels: PII / CLEAN)' },
  { key: 'topic', emoji: '🎯', label: 'Topic Router',
    title: 'Semantic-similarity classifier that sorts prompts into code / creative / general' },
];

export const ClassifierCard: React.FC<{
  clf: RouterClassifier;
  isHighlighted: boolean;
  embeddingOptions: CandidateOption[];
  candidateOptions: CandidateOption[];
  displayNameWithStatus: (id: string) => string;
  onPatch: (p: Partial<RouterClassifier>) => void;
  onRemove: () => void;
}> = ({ clf, isHighlighted, embeddingOptions, candidateOptions, displayNameWithStatus, onPatch, onRemove }) => {
  const [expanded, setExpanded] = useState(false);
  const [fullscreen, setFullscreen] = useState(false);
  const modelOptions = clf.type === 'semantic_similarity' ? embeddingOptions : candidateOptions;

  const modelShort = clf.model ? clf.model.split(/[/:-]/).pop() ?? clf.model : '-';
  const parsedLabels = (clf.labels ?? []).filter(Boolean);

  return (
    <div className={`pipeline-clf-card${isHighlighted ? ' pipeline-clf-card--highlighted' : ''}${expanded ? ' pipeline-clf-card--expanded' : ''}`}>
      <div className="pipeline-clf-chip-header">
        <span className="pipeline-clf-type-badge">{TYPE_BADGE[clf.type]}</span>
        <span className="pipeline-clf-id">{clf.id || '(unnamed)'}</span>
        <div className="pipeline-clf-chip-actions">
          <button type="button" className="pipeline-icon-btn" title={expanded ? 'Collapse' : 'Expand'}
            onClick={() => setExpanded(v => !v)}>
            {expanded
              ? <svg width="10" height="10" viewBox="0 0 12 12" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round"><path d="M2 8 L6 4 L10 8"/></svg>
              : <svg width="10" height="10" viewBox="0 0 12 12" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round"><path d="M2 4 L6 8 L10 4"/></svg>}
          </button>
          <button type="button" className="pipeline-icon-btn" title="Open in full view" onClick={() => setFullscreen(true)}>
            <svg width="10" height="10" viewBox="0 0 12 12" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round">
              <path d="M1 4V1H4M8 1H11V4M11 8V11H8M4 11H1V8"/>
            </svg>
          </button>
          <button type="button" className="pipeline-icon-btn" title="Remove" onClick={onRemove}>
            <svg width="10" height="10" viewBox="0 0 12 12" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round"><path d="M1 1 L11 11 M11 1 L1 11"/></svg>
          </button>
        </div>
      </div>

      {!expanded && (
        <div className="pipeline-clf-chip-summary">
          <span className="pipeline-clf-chip-model">{modelShort}</span>
          {clf.type === 'classifier' && parsedLabels.length > 0 && (
            <span className="pipeline-clf-chip-detail">{parsedLabels.slice(0, 3).join(', ')}{parsedLabels.length > 3 ? '…' : ''}</span>
          )}
          {clf.type === 'semantic_similarity' && (() => {
            const concepts = Object.keys(clf.referencePhrases ?? {});
            return concepts.length > 0
              ? <span className="pipeline-clf-chip-detail">{concepts.slice(0, 3).join(', ')}</span>
              : null;
          })()}
          {clf.type === 'llm' && clf.prompt && (
            <span className="pipeline-clf-chip-detail">{clf.prompt.slice(0, 30)}{clf.prompt.length > 30 ? '…' : ''}</span>
          )}
        </div>
      )}

      {fullscreen && createPortal(
        <div className="clf-fullscreen-overlay" onClick={e => { if (e.target === e.currentTarget) setFullscreen(false); }}>
          <div className="clf-fullscreen-panel">
            <div className="clf-fullscreen-header">
              <span className="pipeline-clf-type-badge">{TYPE_BADGE[clf.type]}</span>
              <span style={{ fontWeight: 600, fontSize: '0.85rem' }}>{clf.id || '(unnamed)'}</span>
              <div style={{ flex: 1 }} />
              <button type="button" className="pipeline-icon-btn" title="Close" onClick={() => setFullscreen(false)}>
                <svg width="12" height="12" viewBox="0 0 12 12" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round"><path d="M1 1 L11 11 M11 1 L1 11"/></svg>
              </button>
            </div>
            <div className="clf-fullscreen-body pipeline-clf-expanded">
              <div className="pipeline-clf-field">
                <label className="pipeline-clf-label">ID *</label>
                <input type="text" className="form-input pipeline-clf-input" defaultValue={clf.id}
                  onBlur={e => onPatch({ id: slugify(e.target.value) || clf.id })} placeholder="e.g. pii" />
              </div>
              <div className="pipeline-clf-field">
                <label className="pipeline-clf-label">Type *</label>
                <select className="form-input form-select pipeline-clf-select" value={clf.type}
                  onChange={e => onPatch({ type: e.target.value as RouterClassifier['type'], labels: [], defaultLabel: '', referencePhrases: {}, prompt: undefined })}>
                  <option value="classifier">Classifier</option>
                  <option value="semantic_similarity">Semantic Similarity</option>
                  <option value="llm">LLM</option>
                </select>
              </div>
              <div className="pipeline-clf-field">
                <label className="pipeline-clf-label">Model *</label>
                <ModelSelect
                  options={modelOptions.map(({ id }) => ({ id, label: displayNameWithStatus(id) }))}
                  value={clf.model}
                  onChange={id => onPatch({ model: id })}
                  placeholder="Select a model…"
                  searchPlaceholder="Search models…"
                />
              </div>
              {clf.type === 'classifier' && (
                <>
                  <div className="pipeline-clf-field">
                    <label className="pipeline-clf-label">Labels <span className="settings-description" style={{ marginLeft: 4 }}>(one per line)</span></label>
                    <textarea className="form-input pipeline-clf-textarea" rows={4}
                      defaultValue={(clf.labels ?? []).join('\n')}
                      onBlur={e => { const labels = e.target.value.split('\n').map(s => s.trim()).filter(Boolean); onPatch({ labels, defaultLabel: labels.includes(clf.defaultLabel ?? '') ? clf.defaultLabel : '' }); }}
                      placeholder={'PII\nNO_PII'} />
                  </div>
                  <div className="pipeline-clf-field">
                    <label className="pipeline-clf-label">On Error</label>
                    <select className="form-input form-select pipeline-clf-select" value={clf.onError ?? 'match_false'}
                      onChange={e => onPatch({ onError: e.target.value as RouterClassifier['onError'] })}>
                      <option value="match_false">match_false — fail-open (default)</option>
                      <option value="match_true">match_true — fail-closed (safer)</option>
                    </select>
                  </div>
                </>
              )}
              {clf.type === 'llm' && (
                <div className="pipeline-clf-field">
                  <label className="pipeline-clf-label">Prompt *</label>
                  <textarea className="form-input pipeline-clf-textarea" rows={6}
                    defaultValue={clf.prompt ?? ''}
                    onBlur={e => onPatch({ prompt: e.target.value || undefined })}
                    placeholder={'Classify this request.\nReply with ONLY one of: SAFE, RISKY'} />
                </div>
              )}
              {clf.type === 'semantic_similarity' && (
                <div className="pipeline-clf-field">
                  <div className="pipeline-clf-concepts-header">
                    <label className="pipeline-clf-label" style={{ margin: 0 }}>Concepts * <span className="settings-description" style={{ marginLeft: 4 }}>each key becomes an output label</span></label>
                    <button type="button" className="settings-reset-button" style={{ fontSize: '0.65rem', padding: '1px 7px' }}
                      onClick={() => { const ex = clf.referencePhrases ?? {}; onPatch({ referencePhrases: { ...ex, [`concept-${Object.keys(ex).length + 1}`]: [] } }); }}>+ Concept</button>
                  </div>
                  {Object.entries(clf.referencePhrases ?? {}).map(([concept, phrases]) => (
                    <div key={concept} className="pipeline-concept-row">
                      <div style={{ display: 'flex', gap: 4, alignItems: 'center' }}>
                        <input type="text" className="form-input pipeline-concept-name" defaultValue={concept}
                          onBlur={e => { const n = e.target.value.trim(); if (!n || n === concept) return; const ex = { ...clf.referencePhrases }; const p = ex[concept]; delete ex[concept]; ex[n] = p; onPatch({ referencePhrases: ex }); }}
                          placeholder="concept name" />
                        <button type="button" className="pipeline-icon-btn" onClick={() => { const ex = { ...clf.referencePhrases }; delete ex[concept]; onPatch({ referencePhrases: ex }); }}>×</button>
                      </div>
                      <textarea className="form-input pipeline-concept-phrases" rows={3}
                        defaultValue={phrases.join('\n')}
                        onBlur={e => onPatch({ referencePhrases: { ...clf.referencePhrases, [concept]: e.target.value.split('\n').map(s => s.trim()).filter(Boolean) } })}
                        placeholder="phrases (one per line)" />
                    </div>
                  ))}
                </div>
              )}
            </div>
            <div className="clf-fullscreen-footer">
              <button type="button" className="settings-save-button" onClick={() => setFullscreen(false)}>Done</button>
            </div>
          </div>
        </div>,
        document.body,
      )}

      {expanded && (
        <div className="pipeline-clf-expanded">
          <div className="pipeline-clf-field">
            <label className="pipeline-clf-label">ID *</label>
            <input type="text" className="form-input pipeline-clf-input" defaultValue={clf.id}
              onBlur={e => onPatch({ id: slugify(e.target.value) || clf.id })} placeholder="e.g. pii" />
          </div>

          <div className="pipeline-clf-field">
            <label className="pipeline-clf-label">Type *</label>
            <select className="form-input form-select pipeline-clf-select" value={clf.type}
              onChange={e => onPatch({ type: e.target.value as RouterClassifier['type'], labels: [], defaultLabel: '', referencePhrases: {}, prompt: undefined })}>
              <option value="classifier">Classifier</option>
              <option value="semantic_similarity">Semantic Similarity</option>
              <option value="llm">LLM</option>
            </select>
          </div>

          <div className="pipeline-clf-field">
            <label className="pipeline-clf-label">Model *</label>
            <ModelSelect
              options={modelOptions.map(({ id }) => ({ id, label: displayNameWithStatus(id) }))}
              value={clf.model}
              onChange={id => onPatch({ model: id })}
              placeholder="Select a model…"
              searchPlaceholder="Search models…"
            />
            {clf.type === 'semantic_similarity' && embeddingOptions.length === 0 && (
              <span className="settings-description" style={{ fontSize: '0.65rem', marginTop: 2, display: 'block' }}>
                No embedding models found. Pull one first (e.g. nomic-embed-text-v1.5-GGUF).
              </span>
            )}
          </div>

          {clf.type === 'classifier' && (
            <>
              <div className="pipeline-clf-field">
                <label className="pipeline-clf-label">
                  Labels
                  <span className="settings-description" style={{ marginLeft: 4 }}>(one per line)</span>
                </label>
                <textarea className="form-input pipeline-clf-textarea" rows={3}
                  defaultValue={(clf.labels ?? []).join('\n')}
                  onBlur={e => {
                    const labels = e.target.value.split('\n').map(s => s.trim()).filter(Boolean);
                    const defaultLabel = labels.includes(clf.defaultLabel ?? '') ? clf.defaultLabel : '';
                    onPatch({ labels, defaultLabel });
                  }}
                  placeholder={'PII\nNO_PII'} />
              </div>

              {parsedLabels.length > 0 && (
                <div className="pipeline-clf-field">
                  <label className="pipeline-clf-label">
                    Default Label
                    <span className="settings-description" style={{ marginLeft: 4 }}>used when condition omits label</span>
                  </label>
                  <select className="form-input form-select pipeline-clf-select"
                    value={clf.defaultLabel ?? ''}
                    onChange={e => onPatch({ defaultLabel: e.target.value })}>
                    <option value="">None</option>
                    {parsedLabels.map(l => <option key={l} value={l}>{l}</option>)}
                  </select>
                </div>
              )}

              <div className="pipeline-clf-field">
                <label className="pipeline-clf-label">On Error</label>
                <select className="form-input form-select pipeline-clf-select"
                  value={clf.onError ?? 'match_false'}
                  onChange={e => onPatch({ onError: e.target.value as RouterClassifier['onError'] })}>
                  <option value="match_false">match_false - fail-open (default)</option>
                  <option value="match_true">match_true - fail-closed (safer)</option>
                </select>
              </div>
            </>
          )}

          {clf.type === 'llm' && (
            <div className="pipeline-clf-field">
              <label className="pipeline-clf-label">Prompt *</label>
              <textarea className="form-input pipeline-clf-textarea" rows={4}
                defaultValue={clf.prompt ?? ''}
                onBlur={e => onPatch({ prompt: e.target.value || undefined })}
                placeholder={'Classify this request.\nReply with ONLY one of: SAFE, RISKY'} />
            </div>
          )}

          {clf.type === 'semantic_similarity' && (
            <div className="pipeline-clf-field">
              <div className="pipeline-clf-concepts-header">
                <label className="pipeline-clf-label" style={{ margin: 0 }}>
                  Concepts *
                  <span className="settings-description" style={{ marginLeft: 4 }}>each key becomes an output label</span>
                </label>
                <button type="button" className="settings-reset-button" style={{ fontSize: '0.65rem', padding: '1px 7px' }}
                  onClick={() => {
                    const ex = clf.referencePhrases ?? {};
                    onPatch({ referencePhrases: { ...ex, [`concept-${Object.keys(ex).length + 1}`]: [] } });
                  }}>+ Concept</button>
              </div>
              {Object.keys(clf.referencePhrases ?? {}).length === 0 && (
                <div className="collection-role-empty" style={{ fontSize: '0.65rem', marginTop: 2 }}>
                  No concepts yet. Click "+ Concept" to add one.
                </div>
              )}
              {Object.entries(clf.referencePhrases ?? {}).map(([concept, phrases]) => (
                <div key={concept} className="pipeline-concept-row">
                  <div style={{ display: 'flex', gap: 4, alignItems: 'center' }}>
                    <input type="text" className="form-input pipeline-concept-name" defaultValue={concept}
                      onBlur={e => {
                        const newName = e.target.value.trim();
                        if (!newName || newName === concept) return;
                        const ex = { ...clf.referencePhrases };
                        const p = ex[concept]; delete ex[concept]; ex[newName] = p;
                        onPatch({ referencePhrases: ex });
                      }} placeholder="concept name (= output label)" />
                    <button type="button" className="pipeline-icon-btn" title="Remove concept"
                      onClick={() => { const ex = { ...clf.referencePhrases }; delete ex[concept]; onPatch({ referencePhrases: ex }); }}>×</button>
                  </div>
                  <textarea className="form-input pipeline-concept-phrases" rows={2}
                    defaultValue={phrases.join('\n')}
                    onBlur={e => onPatch({ referencePhrases: { ...clf.referencePhrases, [concept]: e.target.value.split('\n').map(s => s.trim()).filter(Boolean) } })}
                    placeholder="phrases (one per line)" />
                </div>
              ))}
            </div>
          )}
        </div>
      )}
    </div>
  );
};

interface RouterClassifiersSectionProps {
  classifiers: RouterClassifier[];
  candidateOptions: CandidateOption[];
  embeddingOptions: CandidateOption[];
  displayNameWithStatus: (id: string) => string;
  onAddClassifier: () => void;
  onAddTemplate: (key: ClassifierTemplateKey) => void;
  onPatchClassifier: (id: string, p: Partial<RouterClassifier>) => void;
  onRemoveClassifier: (id: string) => void;
  highlightedClassifierId?: string | null;
}

// Standalone authoring surface for the reusable ML detectors that rules
// reference by id. Decoupled from rule building: declaring a classifier and
// wiring up routing logic are separate jobs.
const RouterClassifiersSection: React.FC<RouterClassifiersSectionProps> = ({
  classifiers, candidateOptions, embeddingOptions, displayNameWithStatus,
  onAddClassifier, onAddTemplate, onPatchClassifier, onRemoveClassifier, highlightedClassifierId = null,
}) => {
  return (
    <div className="form-section">
      <div className="rpc-section-header">
        <span className="form-label" style={{ margin: 0 }}>Classifiers</span>
        <span className="settings-description" style={{ marginLeft: 6 }}>- reusable detectors that rules can reference</span>
        <button type="button" className="settings-reset-button pipeline-add-btn"
          style={{ marginLeft: 'auto' }} onClick={onAddClassifier}>
          + Add
        </button>
      </div>
      <div className="quick-presets-bar">
        <span className="quick-presets-label">Templates</span>
        {CLASSIFIER_TEMPLATES.map(t => (
          <button key={t.key} type="button" className="quick-preset-btn" title={t.title}
            onClick={() => onAddTemplate(t.key)}>
            {t.emoji} {t.label}
          </button>
        ))}
      </div>
      <div className="pipeline-clf-lane rpc-clf-lane">
        {classifiers.length === 0 && (
          <div className="collection-role-empty">No classifiers - add one to use as a condition signal in a rule graph.</div>
        )}
        {classifiers.map((clf, ci) => (
          <ClassifierCard key={ci} clf={clf}
            isHighlighted={highlightedClassifierId === clf.id}
            embeddingOptions={embeddingOptions}
            candidateOptions={candidateOptions}
            displayNameWithStatus={displayNameWithStatus}
            onPatch={p => onPatchClassifier(clf.id, p)}
            onRemove={() => onRemoveClassifier(clf.id)} />
        ))}
      </div>
    </div>
  );
};

export default RouterClassifiersSection;
