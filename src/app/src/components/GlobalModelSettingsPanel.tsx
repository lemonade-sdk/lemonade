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
import { useI18n } from '../i18n';

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

const READ_MODES: Array<{ value: TtsReadMode; titleKey: string; descriptionKey: string }> = [
  { value: 'on-demand', titleKey: 'global.chat.readModes.onDemand.title', descriptionKey: 'global.chat.readModes.onDemand.description' },
  { value: 'agent', titleKey: 'global.chat.readModes.agent.title', descriptionKey: 'global.chat.readModes.agent.description' },
  { value: 'agent-and-user', titleKey: 'global.chat.readModes.agentAndUser.title', descriptionKey: 'global.chat.readModes.agentAndUser.description' },
];

const GlobalModelSettingsPanel: React.FC<GlobalModelSettingsPanelProps> = ({ section, models, loadedModels }) => {
  const { t, formatNumber } = useI18n('settings');
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
  const loadedEstimateLabel = Number.isFinite(loadedEstimate) && loadedEstimate > 0
    ? `${formatNumber(loadedEstimate, { maximumFractionDigits: loadedEstimate >= 10 ? 0 : 1 })} GB`
    : t('global.memory.unknown');

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
            <div className="global-settings-card__head"><div><Icon name="chat" size={18} /><h3>{t('global.chat.historyTitle')}</h3></div></div>
            <label className="global-settings-toggle">
              <input type="checkbox" checked={persistHistory} onChange={event => { setPersistHistory(event.target.checked); setSaved(false); }} />
              <span><strong>{t('global.chat.saveHistory')}</strong><small>{t('global.chat.mediaNotPersisted')}</small></span>
            </label>
          </section>

          <section className="global-settings-card">
            <div className="global-settings-card__head"><div><Icon name="brain" size={18} /><h3>{t('global.chat.behaviorTitle')}</h3></div></div>
            <label className="global-settings-toggle">
              <input type="checkbox" checked={draft.collapseThinkingByDefault} onChange={event => patchDraft('collapseThinkingByDefault', event.target.checked)} />
              <span><strong>{t('global.chat.collapseThinking')}</strong><small>{t('global.chat.collapseThinkingDescription')}</small></span>
            </label>
          </section>

          <section className="global-settings-card">
            <div className="global-settings-card__head"><div><Icon name="tts" size={18} /><h3>{t('global.chat.speechTitle')}</h3></div></div>
            <label className="global-settings-field">
              <span>{t('global.chat.defaultTts')}</span>
              <select className="select" value={ttsModel || ''} onChange={event => { setTtsModel(event.target.value || null); setSaved(false); }}>
                <option value="">{t('global.chat.noDefaultSpeech')}</option>
                <optgroup label={t('global.chat.kokoroGroup')}>
                  {kokoroModels.length
                    ? kokoroModels.map(model => <option key={modelName(model)} value={modelName(model)}>{modelDisplayName(model)}</option>)
                    : <option disabled value="__kokoro_missing">{t('global.chat.kokoroMissing')}</option>}
                </optgroup>
                <optgroup label={t('global.chat.openMossGroup')}>
                  {openMossModels.length
                    ? openMossModels.map(model => <option key={modelName(model)} value={modelName(model)}>{modelDisplayName(model)}</option>)
                    : <option disabled value="__openmoss_missing">{t('global.chat.openMossMissing')}</option>}
                </optgroup>
                {otherTtsModels.length > 0 && <optgroup label={t('global.chat.otherTts')}>
                  {otherTtsModels.map(model => <option key={modelName(model)} value={modelName(model)}>{modelDisplayName(model)}</option>)}
                </optgroup>}
              </select>
            </label>
            <div className="global-settings-read-modes" role="radiogroup" aria-label={t('global.chat.playbackMode')}>
              {READ_MODES.map(mode => (
                <button key={mode.value} type="button" role="radio" aria-checked={ttsReadMode === mode.value} className={ttsReadMode === mode.value ? 'is-active' : ''} onClick={() => { setTtsReadMode(mode.value); setSaved(false); }}>
                  <strong>{t(mode.titleKey)}</strong><small>{t(mode.descriptionKey)}</small>
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
              <div><Icon name="gauge" size={18} /><h3>{t('global.memory.budgetTitle')}</h3></div>
              <span>{t('global.memory.loadedEstimate', { count: loadedModels.length, size: loadedEstimateLabel })}</span>
            </div>
            <p className="global-settings-card__description">{t('global.memory.budgetDescription')}</p>
            <div className="global-settings-grid global-settings-grid--two">
              <label className="global-settings-field">
                <span>{t('global.memory.budgetSource')}</span>
                <select className="select" value={draft.resourceBudgetMode} onChange={event => patchDraft('resourceBudgetMode', event.target.value as ResourceBudgetMode)}>
                  <option value="server">{t('global.memory.automaticServer')}</option>
                  <option value="vram">{t('global.memory.customVram')}</option>
                  <option value="memory">{t('global.memory.customMemory')}</option>
                </select>
              </label>
              <label className="global-settings-field">
                <span>{t('global.memory.budget')}</span>
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
              <span><strong>{t('global.memory.autoEvict')}</strong><small>{t('global.memory.autoEvictDescription')}</small></span>
            </label>
          </section>

          <section className="global-settings-card">
            <div className="global-settings-card__head"><div><Icon name="layers" size={18} /><h3>{t('global.memory.loadingTitle')}</h3></div></div>
            <div className="global-settings-grid global-settings-grid--two">
              <label className="global-settings-field">
                <span>{t('global.memory.loadingPolicy')}</span>
                <select className="select" value={draft.loadingPolicy} onChange={event => patchDraft('loadingPolicy', event.target.value as ModelLoadingPolicy)}>
                  <option value="keep-loaded">{t('global.memory.keepLoaded')}</option>
                  <option value="single-active">{t('global.memory.singleActive')}</option>
                  <option value="budget-aware">{t('global.memory.stayWithinBudget')}</option>
                </select>
              </label>
              <label className="global-settings-field">
                <span>{t('global.memory.evictionOrder')}</span>
                <select className="select" value={draft.evictionPolicy} onChange={event => patchDraft('evictionPolicy', event.target.value as ModelEvictionPolicy)}>
                  <option value="lru">{t('global.memory.leastRecentlyUsed')}</option>
                  <option value="largest">{t('global.memory.largestFirst')}</option>
                  <option value="oldest-process">{t('global.memory.oldestProcess')}</option>
                  <option value="manual">{t('global.memory.manualOnly')}</option>
                </select>
              </label>
            </div>
            <label className="global-settings-toggle">
              <input type="checkbox" checked={draft.protectPinnedModels} onChange={event => patchDraft('protectPinnedModels', event.target.checked)} />
              <span><strong>{t('global.memory.protectPinned')}</strong><small>{t('global.memory.protectPinnedDescription')}</small></span>
            </label>
          </section>
        </>
      )}

      {section === 'updates' && (
        <section className="global-settings-card">
          <div className="global-settings-card__head"><div><Icon name="rotate-ccw" size={18} /><h3>{t('global.updates.title')}</h3></div></div>
          <label className="global-settings-toggle">
            <input type="checkbox" checked={draft.automaticModelUpdates} onChange={event => patchDraft('automaticModelUpdates', event.target.checked)} />
            <span><strong>{t('global.updates.automatic')}</strong><small>{t('global.updates.automaticDescription')}</small></span>
          </label>
        </section>
      )}

      <WorkspaceActionGroup label={t('global.actionsLabel', { section: t(`global.sectionNames.${section}`) })}>
        <WorkspaceActionButton appearance="primary" icon="check" onClick={handleSave}>
          {saved ? t('global.saved') : t('global.save')}
        </WorkspaceActionButton>
        <WorkspaceActionButton appearance="quiet" icon="rotate-ccw" onClick={handleReset}>{t('global.reset')}</WorkspaceActionButton>
      </WorkspaceActionGroup>
    </div>
  );
};

export default GlobalModelSettingsPanel;
