import React, { useState, useRef, useEffect } from 'react';
import { useModels, DEFAULT_MODEL_ID } from '../hooks/useModels';
import { isCollectionModel } from '../utils/collectionModels';
import { CUSTOM_COLLECTION_PREFIX } from '../utils/customCollections';

interface ModelSelectorProps {
  disabled: boolean;
}

type SelectorModel = { id: string; info?: ReturnType<typeof useModels>['downloadedModels'][number]['info'] };

const ModelSelector: React.FC<ModelSelectorProps> = ({ disabled }) => {
  const {
    downloadedModels,
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
    if (model.info?.labels?.includes('esrgan')) return false;
    if (!isCollectionModel(model.info)) {
      return true;
    }
    return model.info.suggested === true;
  });

  const allModels: SelectorModel[] = isDefaultModelPending
    ? [{ id: DEFAULT_MODEL_ID }]
    : visibleDownloadedModels;

  const modelDisplayLabel = (id: string, info?: SelectorModel['info']) => {
    if (!id.startsWith(CUSTOM_COLLECTION_PREFIX)) return id;
    return info?.collection_name ?? id;
  };

  const dropdownModels = searchQuery.trim()
    ? allModels.filter((model) => {
      const query = searchQuery.toLowerCase();
      return model.id.toLowerCase().includes(query) || modelDisplayLabel(model.id, model.info).toLowerCase().includes(query);
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

  const handleSelect = (id: string) => {
    setUserHasSelectedModel(true);
    setSelectedModel(id);
    setIsOpen(false);
  };

  const selectedModelInfo = allModels.find((model) => model.id === selectedModel)?.info;
  const renderModelLabel = (id: string, info?: SelectorModel['info']) => modelDisplayLabel(id, info);

  return (
    <div
      ref={containerRef}
      className={`model-selector-custom${disabled ? ' disabled' : ''}`}
    >
      <button
        className="model-selector-trigger"
        onClick={() => !disabled && setIsOpen(prev => !prev)}
        disabled={disabled}
        title={selectedModel}
      >
        <span className="model-selector-label">{renderModelLabel(selectedModel, selectedModelInfo)}</span>
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
                className={`model-selector-option${model.id === selectedModel ? ' selected' : ''}${model.id.startsWith(CUSTOM_COLLECTION_PREFIX) ? ' collection-option' : ''}`}
                onClick={() => handleSelect(model.id)}
                title={model.id}
              >
                {renderModelLabel(model.id, model.info)}
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
