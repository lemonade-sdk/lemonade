import React, { useEffect, useMemo, useState } from 'react';
import { api, friendlyErrorMessage, type LoadedModel, type ModelInfo } from '../api';
import { capabilityFromModelInfo } from '../modelCapabilities';
import { loadChatHistoryPreference, saveChatHistoryPreference } from '../features/chatHistory/historySettings';
import {
  DEFAULT_GLOBAL_MODEL_SETTINGS,
  autoCheckModelUpdatesFromConfig,
  loadGlobalModelSettings,
  memoryRuntimeConfigChanges,
  memoryRuntimeSettingsFromConfig,
  saveGlobalModelSettings,
  type GlobalModelSettings,
  type MemoryRuntimeSettings,
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
  connected: boolean;
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

const READ_MODES: Array<{ value: TtsReadMode; title: string; description: string }> = [
  { value: 'on-demand', title: 'Agent read on demand', description: 'Play speech only when the speaker action is used.' },
  { value: 'agent', title: 'Read agent', description: 'Automatically read every assistant response.' },
  { value: 'agent-and-user', title: 'Read agent and user', description: 'Read assistant responses and submitted user text.' },
];

const GlobalModelSettingsPanel: React.FC<GlobalModelSettingsPanelProps> = ({ section, models, loadedModels, connected }) => {
  const [draft, setDraft] = useState<GlobalModelSettings>(() => loadGlobalModelSettings());
  const [memoryDraft, setMemoryDraft] = useState<MemoryRuntimeSettings | null>(null);
  const [autoCheckModelUpdates, setAutoCheckModelUpdates] = useState<boolean | null>(null);
  const [persistHistory, setPersistHistory] = useState(() => loadChatHistoryPreference());
  const [ttsModel, setTtsModel] = useState<string | null>(() => loadTtsPlaybackSettings().modelName);
  const [ttsReadMode, setTtsReadMode] = useState<TtsReadMode>(() => ttsReadModeFromSettings(loadTtsPlaybackSettings()));
  const [serverLoading, setServerLoading] = useState(false);
  const [serverSaving, setServerSaving] = useState(false);
  const [serverError, setServerError] = useState<string | null>(null);
  const [saved, setSaved] = useState(false);

  useEffect(() => {
    setDraft(loadGlobalModelSettings());
    setPersistHistory(loadChatHistoryPreference());
    const speech = loadTtsPlaybackSettings();
    setTtsModel(speech.modelName);
    setTtsReadMode(ttsReadModeFromSettings(speech));
    setSaved(false);
  }, [section]);

  useEffect(() => {
    if (section === 'chat') {
      setServerError(null);
      setServerLoading(false);
      return;
    }
    if (!connected) {
      setMemoryDraft(null);
      setAutoCheckModelUpdates(null);
      setServerError('Connect to a server to manage these settings.');
      setServerLoading(false);
      return;
    }

    let cancelled = false;
    setServerLoading(true);
    setServerError(null);
    void api.getRuntimeConfig()
      .then(config => {
        if (cancelled) return;
        if (section === 'memory') setMemoryDraft(memoryRuntimeSettingsFromConfig(config));
        else setAutoCheckModelUpdates(autoCheckModelUpdatesFromConfig(config));
      })
      .catch(error => {
        if (!cancelled) setServerError(`Could not load server settings: ${friendlyErrorMessage(error)}`);
      })
      .finally(() => {
        if (!cancelled) setServerLoading(false);
      });
    return () => { cancelled = true; };
  }, [connected, section]);

  const sortedModels = useMemo(() => [...models]
    .filter(model => modelName(model))
    .sort((a, b) => modelDisplayName(a).localeCompare(modelDisplayName(b))), [models]);
  const ttsModels = useMemo(() => sortedModels.filter(model => capabilityFromModelInfo(model) === 'tts'), [sortedModels]);
  const kokoroModels = ttsModels.filter(model => modelRecipe(model).includes('kokoro'));
  const openMossModels = ttsModels.filter(model => modelRecipe(model).includes('openmoss') && !/voicegen/i.test(modelName(model)));
  const otherTtsModels = ttsModels.filter(model => !kokoroModels.includes(model) && !openMossModels.includes(model));

  const patchDraft = <K extends keyof GlobalModelSettings>(key: K, value: GlobalModelSettings[K]) => {
    setDraft(current => ({ ...current, [key]: value }));
    setSaved(false);
  };

  const patchMemoryDraft = <K extends keyof MemoryRuntimeSettings>(key: K, value: MemoryRuntimeSettings[K]) => {
    setMemoryDraft(current => current ? { ...current, [key]: value } : current);
    setSaved(false);
  };

  const stepEvictionThreshold = (direction: -1 | 1) => {
    if (!memoryDraft) return;
    const next = memoryDraft.autoEvictThresholdPercent + direction * 5;
    if (next <= 0 || next > 100) return;
    patchMemoryDraft('autoEvictThresholdPercent', next);
  };

  const stepMaxLoadedModels = (direction: -1 | 1) => {
    if (!memoryDraft) return;
    const current = memoryDraft.maxLoadedModels;
    const next = direction > 0
      ? (current < 1 ? 1 : current + 1)
      : (current <= 1 ? -1 : current - 1);
    patchMemoryDraft('maxLoadedModels', next);
  };

  const refetchServerDraft = async () => {
    const config = await api.getRuntimeConfig();
    if (section === 'memory') setMemoryDraft(memoryRuntimeSettingsFromConfig(config));
    else if (section === 'updates') setAutoCheckModelUpdates(autoCheckModelUpdatesFromConfig(config));
  };

  const handleSave = async () => {
    setSaved(false);
    if (section === 'chat') {
      const savedSettings = saveGlobalModelSettings({ collapseThinkingByDefault: draft.collapseThinkingByDefault });
      setDraft(savedSettings);
      saveChatHistoryPreference(persistHistory);
      saveActiveTtsModel(ttsModel);
      saveTtsReadMode(ttsReadMode);
      setSaved(true);
      window.setTimeout(() => setSaved(false), 1600);
      return;
    }

    if (!connected) {
      setServerError('Connect to a server to manage these settings.');
      return;
    }

    setServerSaving(true);
    setServerError(null);
    try {
      if (section === 'memory') {
        if (!memoryDraft) throw new Error('Server memory settings are not available.');
        await api.setRuntimeConfig(memoryRuntimeConfigChanges(memoryDraft));
      } else {
        if (autoCheckModelUpdates === null) throw new Error('Server update settings are not available.');
        await api.setRuntimeConfig({ auto_check_model_updates: autoCheckModelUpdates });
      }
      await refetchServerDraft();
      setSaved(true);
      window.setTimeout(() => setSaved(false), 1600);
    } catch (error) {
      setServerError(`Failed to save settings: ${friendlyErrorMessage(error)}`);
      // A rejected or unconfirmed write must not leave the attempted draft
      // looking authoritative. Refetch if possible; otherwise clear it.
      try {
        await refetchServerDraft();
      } catch {
        if (section === 'memory') setMemoryDraft(null);
        else setAutoCheckModelUpdates(null);
      }
    } finally {
      setServerSaving(false);
    }
  };

  const handleReset = async () => {
    setSaved(false);
    if (section === 'chat') {
      setDraft(current => ({ ...current, collapseThinkingByDefault: DEFAULT_GLOBAL_MODEL_SETTINGS.collapseThinkingByDefault }));
      setPersistHistory(false);
      setTtsModel(null);
      setTtsReadMode('on-demand');
      return;
    }
    if (!connected) return;
    setServerLoading(true);
    setServerError(null);
    try {
      // There is no complete authoritative reset for all lifecycle fields in the
      // current runtime contract, so discard the draft and reload server state.
      await refetchServerDraft();
    } catch (error) {
      setServerError(`Could not reload server settings: ${friendlyErrorMessage(error)}`);
    } finally {
      setServerLoading(false);
    }
  };

  const serverControlsDisabled = serverLoading || serverSaving || !connected;
  const serverDraftUnavailable = section === 'memory' ? !memoryDraft : autoCheckModelUpdates === null;

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
              <div><Icon name="gauge" size={18} /><h3>Memory management</h3></div>
              <span>{loadedModels.length} loaded</span>
            </div>
            <p className="global-settings-card__description">Lemonade owns model residency and memory-pressure eviction.</p>
            <label className="global-settings-toggle">
              <input
                type="checkbox"
                checked={memoryDraft?.autoEvict ?? false}
                disabled={serverControlsDisabled || !memoryDraft}
                onChange={event => patchMemoryDraft('autoEvict', event.target.checked)}
              />
              <span><strong>Automatic eviction</strong><small>Let Lemonade manage model residency when VRAM pressure is high.</small></span>
            </label>
            {memoryDraft?.autoEvict && (
              <div className="global-settings-field">
                <label htmlFor="global-auto-evict-threshold">Eviction threshold</label>
                <div className="detail-configuration__number-control global-settings-number">
                  <input
                    id="global-auto-evict-threshold"
                    className="input detail-configuration__number-input"
                    type="number"
                    max={100}
                    step={5}
                    disabled={serverControlsDisabled}
                    value={memoryDraft.autoEvictThresholdPercent}
                    onKeyDown={event => {
                      if (event.key !== 'ArrowUp' && event.key !== 'ArrowDown') return;
                      event.preventDefault();
                      stepEvictionThreshold(event.key === 'ArrowUp' ? 1 : -1);
                    }}
                    onChange={event => {
                      const next = Number(event.target.value);
                      if (Number.isFinite(next) && next > 0 && next <= 100) {
                        patchMemoryDraft('autoEvictThresholdPercent', next);
                      } else {
                        event.currentTarget.value = String(memoryDraft.autoEvictThresholdPercent);
                      }
                    }}
                  />
                  <strong>%</strong>
                  <span className="detail-configuration__context-stepper">
                    <button type="button" disabled={serverControlsDisabled || memoryDraft.autoEvictThresholdPercent + 5 > 100} onClick={() => stepEvictionThreshold(1)} aria-label="Increase eviction threshold">
                      <Icon name="chevron-up" size={11} aria-hidden="true" />
                    </button>
                    <button type="button" disabled={serverControlsDisabled || memoryDraft.autoEvictThresholdPercent - 5 <= 0} onClick={() => stepEvictionThreshold(-1)} aria-label="Decrease eviction threshold">
                      <Icon name="chevron-down" size={11} aria-hidden="true" />
                    </button>
                  </span>
                </div>
              </div>
            )}
          </section>

          <section className="global-settings-card">
            <div className="global-settings-card__head"><div><Icon name="layers" size={18} /><h3>Loading and eviction</h3></div></div>
            <div className="global-settings-field">
              <label htmlFor="global-max-loaded-models">Maximum loaded models per type</label>
              <div className="detail-configuration__number-control">
                <input
                  id="global-max-loaded-models"
                  className="input detail-configuration__number-input"
                  type="number"
                  min={-1}
                  step={1}
                  disabled={serverControlsDisabled || !memoryDraft}
                  value={memoryDraft?.maxLoadedModels ?? ''}
                  onKeyDown={event => {
                    if (event.key !== 'ArrowUp' && event.key !== 'ArrowDown') return;
                    event.preventDefault();
                    stepMaxLoadedModels(event.key === 'ArrowUp' ? 1 : -1);
                  }}
                  onChange={event => {
                    const next = Number(event.target.value);
                    if (Number.isInteger(next) && (next === -1 || next > 0)) {
                      patchMemoryDraft('maxLoadedModels', next);
                    } else if (memoryDraft) {
                      event.currentTarget.value = String(memoryDraft.maxLoadedModels);
                    }
                  }}
                />
                <span className="detail-configuration__context-stepper">
                  <button type="button" disabled={serverControlsDisabled || !memoryDraft} onClick={() => stepMaxLoadedModels(1)} aria-label="Increase maximum loaded models per type">
                    <Icon name="chevron-up" size={11} aria-hidden="true" />
                  </button>
                  <button type="button" disabled={serverControlsDisabled || !memoryDraft || memoryDraft.maxLoadedModels <= -1} onClick={() => stepMaxLoadedModels(-1)} aria-label="Decrease maximum loaded models per type">
                    <Icon name="chevron-down" size={11} aria-hidden="true" />
                  </button>
                </span>
              </div>
              <small>Use -1 for unlimited. Lemonade applies this limit independently per model type.</small>
            </div>
            <p className="global-settings-card__description">Eviction order is managed by Lemonade. Pinned models are protected from automatic eviction.</p>
          </section>
        </>
      )}

      {section === 'updates' && (
        <section className="global-settings-card">
          <div className="global-settings-card__head"><div><Icon name="rotate-ccw" size={18} /><h3>Model updates</h3></div></div>
          <label className="global-settings-toggle">
            <input
              type="checkbox"
              checked={autoCheckModelUpdates ?? false}
              disabled={serverControlsDisabled || autoCheckModelUpdates === null}
              onChange={event => { setAutoCheckModelUpdates(event.target.checked); setSaved(false); }}
            />
            <span><strong>Check for model updates on server startup</strong><small>Lemonade checks downloaded models for available updates when the server starts.</small></span>
          </label>
        </section>
      )}

      {section !== 'chat' && serverLoading && <div className="connect__notice">Loading server settings...</div>}
      {section !== 'chat' && serverError && <div className="connect__error" role="alert">{serverError}</div>}

      <WorkspaceActionGroup label={`${section} settings actions`}>
        <WorkspaceActionButton
          appearance="primary"
          icon="check"
          onClick={() => { void handleSave(); }}
          disabled={section !== 'chat' && (serverControlsDisabled || serverDraftUnavailable)}
        >
          {serverSaving ? 'Saving...' : saved ? 'Saved' : 'Save settings'}
        </WorkspaceActionButton>
        <WorkspaceActionButton
          appearance="quiet"
          icon="rotate-ccw"
          onClick={() => { void handleReset(); }}
          disabled={section !== 'chat' && (serverControlsDisabled || serverDraftUnavailable)}
        >
          {section === 'chat' ? 'Reset defaults' : 'Discard changes'}
        </WorkspaceActionButton>
      </WorkspaceActionGroup>
    </div>
  );
};

export default GlobalModelSettingsPanel;
