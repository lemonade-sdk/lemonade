import React, { useState, useRef, useEffect } from 'react';
import { useModels, DEFAULT_MODEL_ID } from '../hooks/useModels';
import { isCollectionModel } from '../utils/collectionModels';
import { isCustomCollectionModel } from '../utils/customCollections';
import { COLLECTION_ROUTER_MODEL_RECIPE } from '../utils/recipeNames';
import { getModelDisplayName } from '../utils/modelDisplayName';

interface ModelSelectorProps {
  disabled: boolean;
  filterLabel?: string;
  effectiveModel?: string;
}

type SelectorModel = { id: string; info?: ReturnType<typeof useModels>['downloadedModels'][number]['info']; unavailable?: boolean };

const ModelSelector: React.FC<ModelSelectorProps> = ({ disabled, filterLabel, effectiveModel }) => {
  const {
    downloadedModels,
    modelsData,
    selectedModel,
    setSelectedModel,
    isDefaultModelPending,
    setUserHasSelectedModel,
  } = useModels();

  const [isOpen, setIsOpen] = useState(false);
  const [searchQuery, setSearchQuery] = useState('');
  const containerRef = useRef<HTMLDivElement>(null);
  const searchRef = useRef<HTMLInputElement>(null);

  const visibleDownloadedModels = downloadedModels.filter((model) => {
    if (model.info?.labels?.includes('upscaling')) return false;
    if (filterLabel && !model.info?.labels?.includes(filterLabel)) return false;
    if (!isCollectionModel(model.info)) {
      return true;
    }
    return model.info.suggested === true || isCustomCollectionModel(model.id, model.info);
  });

  const visibleDownloadedModelIds = new Set(visibleDownloadedModels.map((model) => model.id));

  const unavailableCustomCollections: SelectorModel[] = filterLabel ? [] : Object.entries(modelsData)
    .filter(([id, info]) => isCollectionModel(info) && isCustomCollectionModel(id, info) && !visibleDownloadedModelIds.has(id))
    .map(([id, info]) => ({ id, info, unavailable: true }));

  const allModels: SelectorModel[] = isDefaultModelPending
    ? [{ id: DEFAULT_MODEL_ID }]
    : [...visibleDownloadedModels, ...unavailableCustomCollections];

  const renderModelLabel = (id: string, info?: SelectorModel['info']) => {
    if (isCollectionModel(info)) {
      return info?.model_name ?? getModelDisplayName(id);
    }

    return getModelDisplayName(id);
  };

  const dropdownModels = searchQuery.trim()
    ? allModels.filter((model) => {
        const query = searchQuery.toLowerCase();
        return model.id.toLowerCase().includes(query) ||
          renderModelLabel(model.id, model.info).toLowerCase().includes(query);
      })
    : allModels;

  useEffect(() => {
    const handleClickOutside = (e: MouseEvent) => {
      if (containerRef.current && !containerRef.current.contains(e.target as Node)) {
        setIsOpen(false);
      }
    };
    document.addEventListener('mousedown', handleClickOutside);
    return () => document.removeEventListener('mousedown', handleClickOutside);
  }, []);

  useEffect(() => {
    if (isOpen) {
      setSearchQuery('');
      setTimeout(() => searchRef.current?.focus(), 0);
    }
  }, [isOpen]);

  const handleSelect = (model: SelectorModel) => {
    if (model.unavailable) return;
    setUserHasSelectedModel(true);
    setSelectedModel(model.id);
    setIsOpen(false);
  };

  const displayedModel = effectiveModel ?? selectedModel;
  const displayedModelInfo = allModels.find((model) => model.id === displayedModel)?.info ?? modelsData[displayedModel];

  return (
    <div
      ref={containerRef}
      className={`model-selector-custom${disabled ? ' disabled' : ''}`}
    >
      <button
        className="model-selector-trigger"
        onClick={() => !disabled && setIsOpen(prev => !prev)}
        disabled={disabled}
        title={displayedModel}
      >
        <span className="model-selector-label">{renderModelLabel(displayedModel, displayedModelInfo)}</span>
        <svg className="model-selector-chevron" width="10" height="10" viewBox="0 0 10 10">
          <path d="M2 3.5L5 6.5L8 3.5" stroke="currentColor" strokeWidth="1.5" fill="none" strokeLinecap="round" strokeLinejoin="round"/>
        </svg>
      </button>

      {isOpen && (
        <div className="model-selector-dropdown">
          <div className="model-selector-list">
            {dropdownModels.length > 0 ? dropdownModels.map((model) => (
              <div
                key={model.id}
                className={`model-selector-option${model.id === displayedModel ? ' selected' : ''}${model.info?.recipe === COLLECTION_ROUTER_MODEL_RECIPE ? ' router-option' : isCustomCollectionModel(model.id, model.info) ? ' collection-option' : ''}${model.unavailable ? ' unavailable' : ''}`}
                onClick={() => handleSelect(model)}
                title={model.unavailable ? `${model.id} is not available until all component models are downloaded.` : model.id}
                aria-disabled={model.unavailable ? true : undefined}
                style={model.unavailable ? { opacity: 0.55, cursor: 'not-allowed' } : undefined}
              >
                {renderModelLabel(model.id, model.info)}{model.unavailable ? ' (not available)' : ''}
              </div>
            )) : (
              <div className="model-selector-empty">No models match</div>
            )}
          </div>
          <div className="model-selector-search-bar">
            <input
              ref={searchRef}
              type="text"
              className="model-selector-search"
              placeholder="Search models..."
              value={searchQuery}
              onChange={(e) => setSearchQuery(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === 'Escape') { e.stopPropagation(); setIsOpen(false); }
              }}
            />
          </div>
        </div>
      )}
    </div>
  );
};

export default ModelSelector;
