import React, { useState, useEffect, useCallback } from 'react';
import api, { ModelInfo, LoadedModel } from '../api';

function formatSize(bytes: number): string {
  if (bytes >= 1e9) return `${(bytes / 1e9).toFixed(1)} GB`;
  if (bytes >= 1e6) return `${(bytes / 1e6).toFixed(0)} MB`;
  return `${bytes} B`;
}

interface ModelManagerProps {
  onModelSelect: (model: string) => void;
  selectedModel: string | null;
}

const ModelManager: React.FC<ModelManagerProps> = ({ onModelSelect, selectedModel }) => {
  const [models, setModels] = useState<ModelInfo[]>([]);
  const [loadedModels, setLoadedModels] = useState<LoadedModel[]>([]);
  const [loadingModel, setLoadingModel] = useState<string | null>(null);
  const [pullingModel, setPullingModel] = useState<string | null>(null);

  const refresh = useCallback(async () => {
    if (!api.isConnected) return;
    const result = await api.refresh();
    if (result) {
      setModels(result.models.data);
      setLoadedModels(result.health.all_models_loaded);
    }
  }, []);

  useEffect(() => { refresh(); }, [refresh]);

  const handleLoad = async (model: ModelInfo) => {
    if (loadingModel) return;
    const modelName = (model as any).model_name || model.name || model.id;
    setLoadingModel(modelName);
    const success = await api.loadModel(modelName);
    if (success) {
      onModelSelect(modelName);
      await refresh();
    }
    setLoadingModel(null);
  };

  const handleUnload = async (model: LoadedModel) => {
    setLoadingModel(model.model_name);
    await api.unloadModel(model.model_name);
    await refresh();
    setLoadingModel(null);
  };

  const handlePull = async (model: ModelInfo) => {
    if (pullingModel) return;
    const modelName = (model as any).model_name || model.name || model.id;
    setPullingModel(modelName);
    await api.pullModel(modelName);
    await refresh();
    setPullingModel(null);
  };

  const loadedNames = new Set(loadedModels.map(m => m.model_name));
  const availableModels = models.filter(m => {
    const name = (m as any).model_name || m.name || m.id;
    return !loadedNames.has(name);
  });

  return (
    <div className="manager">
      <div className="manager__head">
        <div className="manager__title">
          <h1>Models</h1>
          <span className="manager__title-sub">{models.length} available</span>
        </div>
      </div>

      <div className="manager__body">
        {/* Loaded zone */}
        {loadedModels.length > 0 && (
          <section className="zone">
            <div className="zone__head">
              <span className="zone__title">Loaded</span>
              <span className="zone__count">{loadedModels.length}</span>
              <span className="zone__rule" />
            </div>
            {loadedModels.map(m => (
              <div className="row row--loaded" key={m.model_name}>
                <div className="row__main">
                  <div className="row__icon">
                    {m.model_name.charAt(0).toUpperCase()}
                  </div>
                  <div className="row__text">
                    <span className="row__name">{m.model_name}</span>
                    <span className="row__sub">{m.recipe}</span>
                  </div>
                </div>
                <div className="row__right">
                  <span className="row__device">{m.device}</span>
                  {m.type === 'llm' && selectedModel !== m.model_name && (
                    <button className="row__action" onClick={() => onModelSelect(m.model_name)}>
                      Use
                    </button>
                  )}
                  <button
                    className="row__action row__action--unload"
                    onClick={() => handleUnload(m)}
                    disabled={loadingModel === m.model_name}
                  >
                    {loadingModel === m.model_name ? '…' : 'Unload'}
                  </button>
                </div>
              </div>
            ))}
          </section>
        )}

        {/* Available zone */}
        <section className="zone">
          <div className="zone__head">
            <span className="zone__title">Available</span>
            <span className="zone__count">{availableModels.length}</span>
            <span className="zone__rule" />
          </div>
          {availableModels.map(m => {
            const name = (m as any).model_name || m.name || m.id;
            const isLoading = loadingModel === name;
            const isPulling = pullingModel === name;
            const downloaded = (m as any).downloaded;
            return (
              <div className="row" key={name}>
                <div className="row__main">
                  <div className="row__icon">{name.charAt(0).toUpperCase()}</div>
                  <div className="row__text">
                    <span className="row__name">{m.display_name || name}</span>
                    <span className="row__sub">{(m as any).family || ''}</span>
                  </div>
                </div>
                <div className="row__right">
                  {m.size && <span className="row__size">{formatSize(m.size)}</span>}
                  {downloaded !== false ? (
                    <button
                      className="row__action"
                      onClick={() => handleLoad(m)}
                      disabled={isLoading}
                    >
                      {isLoading ? 'Loading…' : 'Load'}
                    </button>
                  ) : (
                    <button
                      className="row__action"
                      onClick={() => handlePull(m)}
                      disabled={isPulling}
                    >
                      {isPulling ? 'Pulling…' : 'Pull'}
                    </button>
                  )}
                </div>
              </div>
            );
          })}
        </section>
      </div>
    </div>
  );
};

export default ModelManager;
