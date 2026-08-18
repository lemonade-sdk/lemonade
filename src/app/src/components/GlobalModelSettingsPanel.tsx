import React, { useEffect, useMemo, useState } from 'react';
import type { LoadedModel, ModelInfo } from '../api';
import { capabilityFromModelInfo } from '../modelCapabilities';
import { loadChatHistoryPreference, saveChatHistoryPreference } from '../features/chatHistory/historySettings';
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
import { Icon } from './Icon';
import { WorkspaceActionButton, WorkspaceActionGroup } from './WorkspacePanels';

export type GlobalSettingsSection = 'chat' | 'memory' | 'updates';

interface GlobalModelSettingsPanelProps {
  section: GlobalSettingsSection;
  models: ModelInfo[];
  loadedModels: LoadedModel[];
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

const GlobalModelSettingsPanel: React.FC<GlobalModelSettingsPanelProps> = ({ section, models, loadedModels }) => {
  const [draft, setDraft] = useState<GlobalModelSettings>(() => loadGlobalModelSettings());
  const [persistHistory, setPersistHistory] = useState(() => loadChatHistoryPreference());
  const [ttsModel, setTtsModel] = useState<string | null>(() => loadTtsPlaybackSettings().modelName);
  const [ttsReadMode, setTtsReadMode] = useState<TtsReadMode>(() => ttsReadModeFromSettings(loadTtsPlaybackSettings()));
  const [saved, setSaved] = useState(false);

  useEffect(() => {
    setDraft(loadGlobalModelSettings());
    setPersistHistory(loadChatHistoryPreference());
    const speech = loadTtsPlaybackSettings();
    setTtsModel(speech.modelName);
    setTtsReadMode(ttsReadModeFromSettings(speech));
    setSaved(false);
  }, [section]);

  const sortedModels = useMemo(() => [...models]
    .filter(model => modelName(model))
    .sort((a, b) => modelDisplayName(a).localeCompare(modelDisplayName(b))), [models]);
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
    const current = loadGlobalModelSettings();
    const next = section === 'memory'
      ? {
          ...current,
          resourceBudgetMode: draft.resourceBudgetMode,
          resourceBudgetGb: draft.resourceBudgetGb,
          autoEvictOnPressure: draft.autoEvictOnPressure,
          loadingPolicy: draft.loadingPolicy,
          evictionPolicy: draft.evictionPolicy,
          protectPinnedModels: draft.protectPinnedModels,
        }
      : section === 'chat'
        ? { ...current, collapseThinkingByDefault: draft.collapseThinkingByDefault }
        : {
            ...current,
            automaticModelUpdates: draft.automaticModelUpdates,
            lastAutomaticUpdateAt: draft.lastAutomaticUpdateAt,
          };

    const savedSettings = saveGlobalModelSettings(next);
    setDraft(savedSettings);
    if (section === 'chat') {
      saveChatHistoryPreference(persistHistory);
      saveActiveTtsModel(ttsModel);
      saveTtsReadMode(ttsReadMode);
    }
    setSaved(true);
    window.setTimeout(() => setSaved(false), 1600);
  };

  const handleReset = () => {
    if (section === 'memory') {
      setDraft(current => ({
        ...current,
        resourceBudgetMode: DEFAULT_GLOBAL_MODEL_SETTINGS.resourceBudgetMode,
        resourceBudgetGb: DEFAULT_GLOBAL_MODEL_SETTINGS.resourceBudgetGb,
        autoEvictOnPressure: DEFAULT_GLOBAL_MODEL_SETTINGS.autoEvictOnPressure,
        loadingPolicy: DEFAULT_GLOBAL_MODEL_SETTINGS.loadingPolicy,
        evictionPolicy: DEFAULT_GLOBAL_MODEL_SETTINGS.evictionPolicy,
        protectPinnedModels: DEFAULT_GLOBAL_MODEL_SETTINGS.protectPinnedModels,
      }));
    } else if (section === 'chat') {
      setDraft(current => ({ ...current, collapseThinkingByDefault: DEFAULT_GLOBAL_MODEL_SETTINGS.collapseThinkingByDefault }));
      setPersistHistory(false);
      setTtsModel(null);
      setTtsReadMode('on-demand');
    } else {
      setDraft(current => ({
        ...current,
        automaticModelUpdates: DEFAULT_GLOBAL_MODEL_SETTINGS.automaticModelUpdates,
        lastAutomaticUpdateAt: DEFAULT_GLOBAL_MODEL_SETTINGS.lastAutomaticUpdateAt,
      }));
    }
    setSaved(false);
  };

  return (
    <div className="global-model-settings__body">
      {section === 'chat' && (
        <>
          <section className="global-settings-card">
            <div className="global-settings-card__head"><div><Icon name="chat" size={18} /><h3>Chat history</h3></div></div>
            <label className="global-settings-toggle">
              <input type="checkbox" checked={persistHistory} onChange={event => { setPersistHistory(event.target.checked); setSaved(false); }} />
              <span><strong>Save chat history in this browser</strong><small>Chat media is never persisted.</small></span>
            </label>
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
              <select className="select" value={ttsModel || ''} onChange={event => { setTtsModel(event.target.value || null); setSaved(false); }}>
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
                <button key={mode.value} type="button" role="radio" aria-checked={ttsReadMode === mode.value} className={ttsReadMode === mode.value ? 'is-active' : ''} onClick={() => { setTtsReadMode(mode.value); setSaved(false); }}>
                  <strong>{mode.title}</strong><small>{mode.description}</small>
                </button>
              ))}
            </div>
          </section>
        </>
      )}

      {section === 'memory' && (
        <>
          <section className="global-settings-card">
            <div className="global-settings-card__head">
              <div><Icon name="gauge" size={18} /><h3>Memory budget</h3></div>
              <span>{loadedModels.length} loaded · {formatGb(loadedEstimate)} estimated</span>
            </div>
            <p className="global-settings-card__description">The client uses known model sizes to pre-evict before loading. Server-managed mode leaves memory decisions entirely to Lemonade.</p>
            <div className="global-settings-grid global-settings-grid--two">
              <label className="global-settings-field">
                <span>Budget source</span>
                <select className="select" value={draft.resourceBudgetMode} onChange={event => patchDraft('resourceBudgetMode', event.target.value as ResourceBudgetMode)}>
                  <option value="server">Automatic / server managed</option>
                  <option value="vram">Custom VRAM budget</option>
                  <option value="memory">Custom system memory budget</option>
                </select>
              </label>
              <label className="global-settings-field">
                <span>Budget</span>
                <div className="global-settings-number">
                  <input
                    className="input"
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
                <select className="select" value={draft.loadingPolicy} onChange={event => patchDraft('loadingPolicy', event.target.value as ModelLoadingPolicy)}>
                  <option value="keep-loaded">Keep loaded models</option>
                  <option value="single-active">Single active model</option>
                  <option value="budget-aware">Stay within budget</option>
                </select>
              </label>
              <label className="global-settings-field">
                <span>Eviction order</span>
                <select className="select" value={draft.evictionPolicy} onChange={event => patchDraft('evictionPolicy', event.target.value as ModelEvictionPolicy)}>
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
        </>
      )}

      {section === 'updates' && (
        <section className="global-settings-card">
          <div className="global-settings-card__head"><div><Icon name="rotate-ccw" size={18} /><h3>Model updates</h3></div></div>
          <label className="global-settings-toggle">
            <input type="checkbox" checked={draft.automaticModelUpdates} onChange={event => patchDraft('automaticModelUpdates', event.target.checked)} />
            <span><strong>Automatic model updates</strong><small>Off by default. When enabled, GUI3 checks downloaded models at most once per day.</small></span>
          </label>
        </section>
      )}

      <WorkspaceActionGroup label={`${section} settings actions`}>
        <WorkspaceActionButton appearance="primary" icon="check" onClick={handleSave}>
          {saved ? 'Saved' : 'Save settings'}
        </WorkspaceActionButton>
        <WorkspaceActionButton appearance="quiet" icon="rotate-ccw" onClick={handleReset}>Reset defaults</WorkspaceActionButton>
      </WorkspaceActionGroup>
    </div>
  );
};

export default GlobalModelSettingsPanel;
