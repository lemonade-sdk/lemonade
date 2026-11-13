import React, { useEffect, useMemo, useState } from 'react';
import api, { friendlyErrorMessage, type LoadedModel, type ModelInfo } from '../api';
import { capabilityFromModelInfo, modelStructure } from '../modelCapabilities';
import { useI18n } from '../i18n';
import { Icon } from './Icon';
import {
  ADAPTIVE_PROMPT_SELECTION_EVENT,
  loadLastAdaptivePromptSelection,
  updateManualAdaptiveIds,
  type AdaptivePromptCatalogResponse,
  type AdaptivePromptSettings,
} from '../features/adaptivePrompts/adaptivePromptSettings';

interface Props {
  value: AdaptivePromptSettings;
  onChange: (value: AdaptivePromptSettings) => void;
  connected: boolean;
  models: ModelInfo[];
  loadedModels: LoadedModel[];
}

export default function AdaptivePromptSettingsPanel({ value, onChange, connected, models, loadedModels }: Props) {
  const { t } = useI18n();
  const [catalog, setCatalog] = useState<AdaptivePromptCatalogResponse | null>(null);
  const [catalogError, setCatalogError] = useState('');
  const [loading, setLoading] = useState(false);
  const [search, setSearch] = useState('');
  const [lastSelection, setLastSelection] = useState<string[] | null>(loadLastAdaptivePromptSelection);

  // The normal model catalog includes downloaded models which are not yet loaded.
  // Only zero-shot/systemone classifiers accept a per-request instruction menu.
  const decisionModels = useMemo(() => {
    const loadedNames = new Set(loadedModels.map(model => model.model_name.toLowerCase()));
    const chat: Array<{ id: string; label: string }> = [];
    const classifiers: Array<{ id: string; label: string }> = [];
    const seen = new Set<string>();
    for (const model of models) {
      const id = String(model.model_name || model.name || model.id || '').trim();
      const normalized = id.toLowerCase();
      if (!id || seen.has(normalized) || modelStructure(String(model.recipe || '')) !== 'single') continue;
      if (model.downloaded !== true && !loadedNames.has(normalized)) continue;
      const capability = capabilityFromModelInfo(model);
      const labels = (model.labels || []).map(label => label.toLowerCase());
      const compatibleClassifier = capability === 'classification' &&
        (labels.includes('systemone') || labels.includes('zero-shot'));
      if (capability !== 'chat' && !compatibleClassifier) continue;
      seen.add(normalized);
      const entry = { id, label: String(model.display_name || id) };
      (compatibleClassifier ? classifiers : chat).push(entry);
    }
    chat.sort((a, b) => a.label.localeCompare(b.label));
    classifiers.sort((a, b) => a.label.localeCompare(b.label));
    return { chat, classifiers };
  }, [models, loadedModels]);

  useEffect(() => {
    const handler = (event: Event) => {
      if (event instanceof CustomEvent && Array.isArray(event.detail)) {
        setLastSelection(event.detail.filter((id: unknown) => typeof id === 'string'));
      }
    };
    window.addEventListener(ADAPTIVE_PROMPT_SELECTION_EVENT, handler);
    return () => window.removeEventListener(ADAPTIVE_PROMPT_SELECTION_EVENT, handler);
  }, []);

  useEffect(() => {
    if (!connected) {
      setCatalog(null);
      setCatalogError('');
      return;
    }
    let disposed = false;
    setLoading(true);
    setCatalogError('');
    void api.listAdaptivePrompts().then(data => {
      if (!disposed) setCatalog(data);
    }).catch(error => {
      if (!disposed) {
        setCatalog(null);
        setCatalogError(friendlyErrorMessage(error));
      }
    }).finally(() => { if (!disposed) setLoading(false); });
    return () => { disposed = true; };
  }, [connected]);

  const filtered = useMemo(() => {
    const q = search.trim().toLowerCase();
    return (catalog?.modules || []).filter(module => !q ||
      `${module.id} ${module.description} ${module.keywords.join(' ')}`.toLowerCase().includes(q));
  }, [catalog, search]);

  const chooseManual = (id: string) => {
    onChange({ ...value, manualIds: updateManualAdaptiveIds(value.manualIds, id, catalog?.modules || []) });
  };

  return (
    <section className="global-settings-card" aria-label={t('Adaptive prompts')}>
      <div className="global-settings-card__head"><div><Icon name="brain" size={18} /><h3>{t('Adaptive prompts')}</h3></div></div>
      <label className="global-settings-toggle">
        <input type="checkbox" checked={value.enabled} onChange={e => onChange({ ...value, enabled: e.target.checked })} />
        <span><strong>{t('Enable adaptive prompts in Lemonade Chat')}</strong>
          <small>{t('Experimental. Only Lemonade Chat opts in; external API clients remain unchanged.')}</small>
        </span>
      </label>
      <label className="global-settings-field">
        <span>{t('Selection mode')}</span>
        <select className="select" disabled={!value.enabled} value={value.mode}
          onChange={e => onChange({ ...value, mode: e.target.value as AdaptivePromptSettings['mode'] })}>
          <option value="rules">{t('Rules (no extra model call)')}</option>
          <option value="llm">{t('Decision model (chat or classifier)')}</option>
          <option value="manual">{t('Manual selection')}</option>
        </select>
      </label>
      {value.enabled && value.mode === 'llm' && (
        <label className="global-settings-field">
          <span>{t('Decision model')}</span>
          <select className="select" value={value.decisionModel}
            onChange={e => onChange({ ...value, decisionModel: e.target.value })}>
            <option value="">{t('Same model as Chat (additional inference)')}</option>
            <optgroup label={t('Downloaded chat models')}>
              {decisionModels.chat.map(model => <option key={model.id} value={model.id}>{model.label}</option>)}
            </optgroup>
            <optgroup label={t('Downloaded classifiers')}>
              {decisionModels.classifiers.map(model => <option key={model.id} value={model.id}>{model.label}</option>)}
            </optgroup>
            {value.decisionModel && ![...decisionModels.chat, ...decisionModels.classifiers]
              .some(model => model.id === value.decisionModel) && (
                <option value={value.decisionModel}>{value.decisionModel} ({t('Unavailable')})</option>
              )}
          </select>
          <small>{t('Downloaded decision models load on demand. A second chat LLM may cause a model swap.')}</small>
        </label>
      )}
      <div className="global-settings-field">
        <strong>{t('Prompt library')}</strong>
        {!connected && <small>{t('Connect to the Lemonade server to browse the library.')}</small>}
        {loading && <small>{t('Loading prompt library...')}</small>}
        {catalogError && <div className="connect__error" role="alert">{t('Prompt library unavailable')}: {catalogError}</div>}
        {catalog && (
          <>
            <small>{t('{count} modules available', { count: catalog.count })} · {catalog.source === 'custom' ? t('Custom catalog') : t('Built-in catalog')}</small>
            <input type="search" className="input" value={search} onChange={e => setSearch(e.target.value)}
              placeholder={t('Search prompt modules')} aria-label={t('Search prompt modules')} />
            {value.mode === 'manual' && <small>{t('Select up to one role and two skills. Save settings to apply.')}</small>}
            <div className="adaptive-prompt-list" role="list" aria-label={t('Prompt library')}>
              {filtered.map(module => (
                <div key={module.id} className="adaptive-prompt-list__entry" role="listitem">
                  <div className="adaptive-prompt-list__heading">
                    {value.mode === 'manual' && (
                      <input type="checkbox" aria-label={module.id} checked={value.manualIds.includes(module.id)}
                        disabled={!value.manualIds.includes(module.id) &&
                          !updateManualAdaptiveIds(value.manualIds, module.id, catalog.modules).includes(module.id)}
                        onChange={() => chooseManual(module.id)} />
                    )}
                    <strong>{module.id}</strong><small>{module.type}</small>
                  </div>
                  <small>{module.description}</small>
                  <details><summary>{t('Show instructions')}</summary>
                    <p className="adaptive-prompt-list__instruction">{module.prompt}</p>
                  </details>
                </div>
              ))}
              {filtered.length === 0 && <small>{t('No matching prompt modules.')}</small>}
            </div>
          </>
        )}
      </div>
      <p className="global-settings-card__description">
        {t('Last selected modules')}: {lastSelection === null ? t('Not tested yet') : lastSelection.length ? lastSelection.join(', ') : t('None')}
      </p>
    </section>
  );
}
