import React, { useEffect, useMemo, useState } from 'react';
import type { LoadedModel, ModelInfo } from '../api';
import { capabilityFromModelInfo } from '../modelCapabilities';
import { Icon } from './Icon';
import {
  DEFAULT_GLOBAL_MODEL_SETTINGS,
  estimatedLoadedSizeGb,
  loadGlobalModelSettings,
  saveGlobalModelSettings,
  type GlobalModelSettings,
  type ModelEvictionPolicy,
  type ModelLoadingPolicy,
  type ResourceBudgetMode,
} from '../features/modelSettings/globalModelSettings';
import {
  loadTtsPlaybackSettings,
  saveActiveTtsModel,
  saveTtsReadMode,
  ttsReadModeFromSettings,
  type TtsReadMode,
} from '../features/audio/ttsSettings';

export interface UpdateAllModelsResult {
  started: number;
  skipped: number;
  errors: string[];
}

interface GlobalModelSettingsPanelProps {
  scope: string;
  models: ModelInfo[];
  loadedModels: LoadedModel[];
  pinnedModels: string[];
  onTogglePin: (modelName: string) => void;
  onUpdateAllModels: () => Promise<UpdateAllModelsResult>;
  onClose: () => void;
}

function modelName(model: ModelInfo): string {
  return String((model as any).model_name || model.name || model.id || '').trim();
}

function modelDisplayName(model: ModelInfo): string {
  return String(model.display_name || modelName(model));
}

function modelRecipe(model: ModelInfo): string {
  return String((model as any).recipe || '').trim().toLowerCase();
}

function formatGb(value: number): string {
  if (!Number.isFinite(value) || value <= 0) return 'Unknown';
  return `${value.toFixed(value >= 10 ? 0 : 1)} GB`;
}

const READ_MODES: Array<{ value: TtsReadMode; title: string; description: string }> = [
  { value: 'on-demand', title: 'Agent read on demand', description: 'Play speech only when the speaker action is used.' },
  { value: 'agent', title: 'Read agent', description: 'Automatically read every assistant response.' },
  { value: 'agent-and-user', title: 'Read agent and user', description: 'Read assistant responses and submitted user text.' },
];

const GlobalModelSettingsPanel: React.FC<GlobalModelSettingsPanelProps> = ({
  scope,
  models,
  loadedModels,
  pinnedModels,
  onTogglePin,
  onUpdateAllModels,
  onClose,
}) => {
  const [draft, setDraft] = useState<GlobalModelSettings>(() => loadGlobalModelSettings(scope));
  const [ttsModel, setTtsModel] = useState<string | null>(() => loadTtsPlaybackSettings(scope).modelName);
  const [ttsReadMode, setTtsReadMode] = useState<TtsReadMode>(() => ttsReadModeFromSettings(loadTtsPlaybackSettings(scope)));
  const [pinCandidate, setPinCandidate] = useState('');
  const [saved, setSaved] = useState(false);
  const [updating, setUpdating] = useState(false);
  const [updateNotice, setUpdateNotice] = useState<string | null>(null);

  useEffect(() => {
    setDraft(loadGlobalModelSettings(scope));
    const speech = loadTtsPlaybackSettings(scope);
    setTtsModel(speech.modelName);
    setTtsReadMode(ttsReadModeFromSettings(speech));
    setSaved(false);
    setUpdateNotice(null);
  }, [scope]);

  const pinnedSet = useMemo(() => new Set(pinnedModels.map(name => name.toLowerCase())), [pinnedModels]);
  const sortedModels = useMemo(() => [...models]
    .filter(model => modelName(model))
    .sort((a, b) => modelDisplayName(a).localeCompare(modelDisplayName(b))), [models]);
  const pinOptions = useMemo(() => sortedModels.filter(model => !pinnedSet.has(modelName(model).toLowerCase())), [sortedModels, pinnedSet]);
  const pinnedRows = useMemo(() => pinnedModels.map(name => {
    const info = models.find(model => modelName(model).toLowerCase() === name.toLowerCase()) || null;
    return { name, label: info ? modelDisplayName(info) : name, size: info ? Number((info as any).size || 0) : 0 };
  }), [models, pinnedModels]);

  const ttsModels = useMemo(() => sortedModels.filter(model => capabilityFromModelInfo(model) === 'tts'), [sortedModels]);
  const kokoroModels = ttsModels.filter(model => modelRecipe(model).includes('kokoro'));
  const openMossModels = ttsModels.filter(model => modelRecipe(model).includes('openmoss') && !/voicegen/i.test(modelName(model)));
  const otherTtsModels = ttsModels.filter(model => !kokoroModels.includes(model) && !openMossModels.includes(model));
  const loadedEstimate = estimatedLoadedSizeGb(loadedModels, models);

  const patchDraft = <K extends keyof GlobalModelSettings>(key: K, value: GlobalModelSettings[K]) => {
    setDraft(current => ({ ...current, [key]: value }));
    setSaved(false);
  };

  const handleSave = () => {
    saveGlobalModelSettings(scope, draft);
    saveActiveTtsModel(scope, ttsModel);
    saveTtsReadMode(scope, ttsReadMode);
    setSaved(true);
    window.setTimeout(() => setSaved(false), 1600);
  };

  const handleReset = () => {
    setDraft({ ...DEFAULT_GLOBAL_MODEL_SETTINGS });
    setTtsModel(null);
    setTtsReadMode('on-demand');
    setSaved(false);
  };

  const handleAddPin = () => {
    if (!pinCandidate) return;
    onTogglePin(pinCandidate);
    setPinCandidate('');
  };

  const handleUpdateAll = async () => {
    if (updating) return;
    setUpdating(true);
    setUpdateNotice(null);
    try {
      const result = await onUpdateAllModels();
      const parts = [`Started ${result.started} model update${result.started === 1 ? '' : 's'}.`];
      if (result.skipped) parts.push(`${result.skipped} skipped.`);
      if (result.errors.length) parts.push(`${result.errors.length} failed to start.`);
      setUpdateNotice(parts.join(' '));
    } catch (error) {
      setUpdateNotice(error instanceof Error ? error.message : String(error));
    } finally {
      setUpdating(false);
    }
  };

  return (
    <section className="global-model-settings" aria-label="Global model settings">
      <header className="global-model-settings__header">
        <div>
          <span className="global-model-settings__eyebrow">Models</span>
          <h2><Icon name="settings" size={20} aria-hidden="true" /> Global model settings</h2>
          <p>Defaults for model memory, loading, chat reasoning, speech and updates.</p>
        </div>
        <button type="button" className="global-model-settings__close" onClick={onClose} aria-label="Close global model settings">
          <Icon name="x" size={18} />
        </button>
      </header>

      <div className="global-model-settings__body">
        <section className="global-settings-card">
          <div className="global-settings-card__head">
            <div><Icon name="gauge" size={18} /><h3>Memory budget</h3></div>
            <span>{loadedModels.length} loaded · {formatGb(loadedEstimate)} estimated</span>
          </div>
          <p className="global-settings-card__description">The client uses known model sizes to pre-evict before loading. Server-managed mode leaves memory decisions entirely to Lemonade.</p>
          <div className="global-settings-grid global-settings-grid--two">
            <label className="global-settings-field">
              <span>Budget source</span>
              <select value={draft.resourceBudgetMode} onChange={event => patchDraft('resourceBudgetMode', event.target.value as ResourceBudgetMode)}>
                <option value="server">Automatic / server managed</option>
                <option value="vram">Custom VRAM budget</option>
                <option value="memory">Custom system memory budget</option>
              </select>
            </label>
            <label className="global-settings-field">
              <span>Budget</span>
              <div className="global-settings-number">
                <input
                  type="number"
                  min={1}
                  max={1024}
                  step={0.5}
                  disabled={draft.resourceBudgetMode === 'server'}
                  value={draft.resourceBudgetGb}
                  onChange={event => patchDraft('resourceBudgetGb', Number(event.target.value))}
                />
                <strong>GB</strong>
              </div>
            </label>
          </div>
          <label className="global-settings-toggle">
            <input type="checkbox" checked={draft.autoEvictOnPressure} disabled={draft.evictionPolicy === 'manual'} onChange={event => patchDraft('autoEvictOnPressure', event.target.checked)} />
            <span><strong>Auto-evict on memory or VRAM pressure</strong><small>On an OOM-style load failure, evict eligible models and retry once.</small></span>
          </label>
        </section>

        <section className="global-settings-card">
          <div className="global-settings-card__head"><div><Icon name="layers" size={18} /><h3>Loading and eviction</h3></div></div>
          <div className="global-settings-grid global-settings-grid--two">
            <label className="global-settings-field">
              <span>Loading policy</span>
              <select value={draft.loadingPolicy} onChange={event => patchDraft('loadingPolicy', event.target.value as ModelLoadingPolicy)}>
                <option value="keep-loaded">Keep loaded models</option>
                <option value="single-active">Single active model</option>
                <option value="budget-aware">Stay within budget</option>
              </select>
            </label>
            <label className="global-settings-field">
              <span>Eviction order</span>
              <select value={draft.evictionPolicy} onChange={event => patchDraft('evictionPolicy', event.target.value as ModelEvictionPolicy)}>
                <option value="lru">Least recently used</option>
                <option value="largest">Largest first</option>
                <option value="oldest-process">Oldest process first</option>
                <option value="manual">Manual only</option>
              </select>
            </label>
          </div>
          <label className="global-settings-toggle">
            <input type="checkbox" checked={draft.protectPinnedModels} onChange={event => patchDraft('protectPinnedModels', event.target.checked)} />
            <span><strong>Protect pinned models from automatic eviction</strong><small>Pinned models can still be unloaded manually.</small></span>
          </label>
        </section>

        <section className="global-settings-card">
          <div className="global-settings-card__head"><div><Icon name="pin" size={18} /><h3>Pinned models</h3></div><span>{pinnedRows.length} pinned</span></div>
          <div className="global-settings-pin-add">
            <select value={pinCandidate} onChange={event => setPinCandidate(event.target.value)}>
              <option value="">Select a model to pin…</option>
              {pinOptions.map(model => <option key={modelName(model)} value={modelName(model)}>{modelDisplayName(model)}</option>)}
            </select>
            <button type="button" className="btn btn--ghost" disabled={!pinCandidate} onClick={handleAddPin}>Pin model</button>
          </div>
          {pinnedRows.length ? (
            <div className="global-settings-pinned-list">
              {pinnedRows.map(row => (
                <div key={row.name} className="global-settings-pinned-row">
                  <Icon name="pin" size={14} />
                  <span><strong>{row.label}</strong><small>{row.name}{row.size > 0 ? ` · ${formatGb(row.size)}` : ''}</small></span>
                  <button type="button" onClick={() => onTogglePin(row.name)} aria-label={`Unpin ${row.label}`} title={`Unpin ${row.label}`}><Icon name="x" size={14} /></button>
                </div>
              ))}
            </div>
          ) : <p className="global-settings-empty">No pinned models. Pinning keeps important models at the top and can protect them from eviction.</p>}
        </section>

        <section className="global-settings-card">
          <div className="global-settings-card__head"><div><Icon name="brain" size={18} /><h3>Chat behavior</h3></div></div>
          <label className="global-settings-toggle">
            <input type="checkbox" checked={draft.collapseThinkingByDefault} onChange={event => patchDraft('collapseThinkingByDefault', event.target.checked)} />
            <span><strong>Collapse thinking by default</strong><small>Reasoning remains available in an expandable section on every assistant message.</small></span>
          </label>
        </section>

        <section className="global-settings-card">
          <div className="global-settings-card__head"><div><Icon name="tts" size={18} /><h3>Chat speech</h3></div></div>
          <label className="global-settings-field">
            <span>Default TTS model</span>
            <select value={ttsModel || ''} onChange={event => setTtsModel(event.target.value || null)}>
              <option value="">No default speech model</option>
              <optgroup label="Kokoro · English">
                {kokoroModels.length
                  ? kokoroModels.map(model => <option key={modelName(model)} value={modelName(model)}>{modelDisplayName(model)}</option>)
                  : <option disabled value="__kokoro_missing">Kokoro English · install kokoro-v1</option>}
              </optgroup>
              <optgroup label="OpenMOSS · Multilingual">
                {openMossModels.length
                  ? openMossModels.map(model => <option key={modelName(model)} value={modelName(model)}>{modelDisplayName(model)}</option>)
                  : <option disabled value="__openmoss_missing">OpenMOSS multilingual · install OpenMOSS-TTS</option>}
              </optgroup>
              {otherTtsModels.length > 0 && <optgroup label="Other TTS models">
                {otherTtsModels.map(model => <option key={modelName(model)} value={modelName(model)}>{modelDisplayName(model)}</option>)}
              </optgroup>}
            </select>
          </label>
          <div className="global-settings-read-modes" role="radiogroup" aria-label="Global TTS playback mode">
            {READ_MODES.map(mode => (
              <button key={mode.value} type="button" role="radio" aria-checked={ttsReadMode === mode.value} className={ttsReadMode === mode.value ? 'is-active' : ''} onClick={() => setTtsReadMode(mode.value)}>
                <strong>{mode.title}</strong><small>{mode.description}</small>
              </button>
            ))}
          </div>
        </section>

        <section className="global-settings-card">
          <div className="global-settings-card__head"><div><Icon name="rotate-ccw" size={18} /><h3>Model updates</h3></div></div>
          <label className="global-settings-toggle">
            <input type="checkbox" checked={draft.automaticModelUpdates} onChange={event => patchDraft('automaticModelUpdates', event.target.checked)} />
            <span><strong>Automatic model updates</strong><small>Off by default. When enabled, GUI3 checks downloaded models at most once per day.</small></span>
          </label>
          <div className="global-settings-update-action">
            <div><strong>Update all models now</strong><small>Starts a pull/update for every downloaded or currently loaded model.</small></div>
            <button type="button" className="btn btn--ghost" disabled={updating} onClick={handleUpdateAll}>{updating ? 'Starting…' : 'Update all'}</button>
          </div>
          {updateNotice && <p className="global-settings-notice" role="status">{updateNotice}</p>}
        </section>
      </div>

      <footer className="global-model-settings__footer">
        <button type="button" className="btn btn--ghost" onClick={handleReset}>Reset defaults</button>
        <div>
          <button type="button" className="btn btn--ghost" onClick={onClose}>Cancel</button>
          <button type="button" className={`btn btn--primary${saved ? ' btn--saved' : ''}`} onClick={handleSave}>{saved ? '✓ Saved' : 'Save settings'}</button>
        </div>
      </footer>
    </section>
  );
};

export default GlobalModelSettingsPanel;
