import React, { useState, useRef } from 'react';
import { useModels } from '../../hooks/useModels';
import { Modality } from '../../hooks/useInferenceState';
import { ModelsData } from '../../utils/modelData';
import { serverFetch } from '../../utils/serverConfig';
import ModelSelector from '../ModelSelector';
import EmptyState from '../EmptyState';

interface UpscaleQueueItem {
  originalData: string;
  upscaledData?: string;
  fileName: string;
  status: 'pending' | 'processing' | 'done' | 'error';
  error?: string;
}

interface UpscalePanelProps {
  isBusy: boolean;
  isInferring: boolean;
  activeModality: Modality | null;
  runPreFlight: (modality: Modality, options: { modelName: string; modelsData: ModelsData; onError: (msg: string) => void }) => Promise<boolean>;
  reset: () => void;
  showError: (msg: string) => void;
}

const UpscalePanel: React.FC<UpscalePanelProps> = ({
  isBusy, isInferring, activeModality,
  runPreFlight, reset, showError,
}) => {
  const { selectedModel, modelsData } = useModels();
  const [upscaleQueue, setUpscaleQueue] = useState<UpscaleQueueItem[]>([]);
  const [isUpscaling, setIsUpscaling] = useState(false);
  const [upscaleProgress, setUpscaleProgress] = useState({ current: 0, total: 0 });
  const upscaleFileInputRef = useRef<HTMLInputElement>(null);
  const [isDragOver, setIsDragOver] = useState(false);
  const messagesEndRef = useRef<HTMLDivElement>(null);

  const readFileAsDataURL = (file: File): Promise<string> => {
    return new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.onload = () => resolve(reader.result as string);
      reader.onerror = reject;
      reader.readAsDataURL(file);
    });
  };

  const addImagesToUpscaleQueue = async (files: File[]) => {
    const imageFiles = files.filter(f => f.type.startsWith('image/'));
    if (imageFiles.length === 0) return;

    const newItems = await Promise.all(imageFiles.map(async (file) => {
      const dataUrl = await readFileAsDataURL(file);
      return {
        originalData: dataUrl,
        fileName: file.name,
        status: 'pending' as const,
      };
    }));

    setUpscaleQueue(prev => [...prev, ...newItems]);
  };

  const handleUpscaleDrop = async (e: React.DragEvent) => {
    e.preventDefault();
    setIsDragOver(false);
    const files = Array.from(e.dataTransfer.files);
    await addImagesToUpscaleQueue(files);
  };

  const handleUpscaleFileSelect = async (e: React.ChangeEvent<HTMLInputElement>) => {
    if (!e.target.files) return;
    const files = Array.from(e.target.files);
    await addImagesToUpscaleQueue(files);
    e.target.value = '';
  };

  const handleUpscalePaste = async (e: React.ClipboardEvent) => {
    const items = Array.from(e.clipboardData.items);
    const imageItems = items.filter(item => item.type.startsWith('image/'));
    if (imageItems.length === 0) return;

    const files = imageItems.map(item => item.getAsFile()).filter(Boolean) as File[];
    await addImagesToUpscaleQueue(files);
  };

  const handleUpscaleProcess = async () => {
    const pendingIndices = upscaleQueue
      .map((item, idx) => ({ item, idx }))
      .filter(({ item }) => item.status === 'pending')
      .map(({ idx }) => idx);

    if (pendingIndices.length === 0) return;

    setIsUpscaling(true);
    setUpscaleProgress({ current: 0, total: pendingIndices.length });

    for (let i = 0; i < pendingIndices.length; i++) {
      const idx = pendingIndices[i];
      setUpscaleProgress({ current: i + 1, total: pendingIndices.length });

      setUpscaleQueue(prev => prev.map((item, j) =>
        j === idx ? { ...item, status: 'processing' } : item
      ));

      try {
        const dataUrl = upscaleQueue[idx].originalData;
        const base64 = dataUrl.includes(',') ? dataUrl.split(',')[1] : dataUrl;

        const response = await serverFetch('/images/upscale', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ model: selectedModel, image: base64 }),
        });

        if (!response.ok) {
          const err = await response.json().catch(() => ({ error: 'Unknown error' }));
          throw new Error(err.error?.message || err.error || 'Upscale failed');
        }

        const data = await response.json();
        const upscaledB64 = data.data?.[0]?.b64_json;

        if (!upscaledB64) {
          throw new Error('No image data in response');
        }

        setUpscaleQueue(prev => prev.map((item, j) =>
          j === idx ? { ...item, upscaledData: upscaledB64, status: 'done' } : item
        ));
      } catch (error: any) {
        console.error('Upscale failed for', upscaleQueue[idx].fileName, error);
        setUpscaleQueue(prev => prev.map((item, j) =>
          j === idx ? { ...item, status: 'error', error: error.message } : item
        ));
      }
    }

    setIsUpscaling(false);
  };

  const saveUpscaledImage = (imageData: string, originalName: string) => {
    const link = document.createElement('a');
    link.href = `data:image/png;base64,${imageData}`;
    const baseName = originalName.replace(/\.[^/.]+$/, '');
    link.download = `${baseName}_upscaled_4x.png`;
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
  };

  const removeFromUpscaleQueue = (index: number) => {
    setUpscaleQueue(prev => prev.filter((_, i) => i !== index));
  };

  return (
    <>
      <div
        className={`chat-messages upscale-container${isDragOver ? ' drag-over' : ''}`}
        onDragOver={(e) => { e.preventDefault(); setIsDragOver(true); }}
        onDragLeave={() => setIsDragOver(false)}
        onDrop={handleUpscaleDrop}
        onPaste={handleUpscalePaste}
        tabIndex={0}
      >
        {upscaleQueue.length === 0 && (
          <div className="upscale-dropzone">
            <svg width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5">
              <rect x="3" y="3" width="18" height="18" rx="2" ry="2"/>
              <circle cx="8.5" cy="8.5" r="1.5"/>
              <polyline points="21 15 16 10 5 21"/>
            </svg>
            <p>Drop images here, paste from clipboard, or use the button below</p>
            <p className="upscale-hint">Images will be upscaled 4x using Real-ESRGAN</p>
          </div>
        )}

        {upscaleQueue.map((item, index) => (
          <div key={index} className="upscale-item">
            <div className="upscale-item-header">
              <span className="upscale-filename">{item.fileName}</span>
              <div className="upscale-item-actions">
                {item.status === 'processing' && (
                  <span className="upscale-status processing">
                    <div className="generating-spinner"></div>
                    Upscaling...
                  </span>
                )}
                {item.status === 'pending' && (
                  <span className="upscale-status pending">Pending</span>
                )}
                {item.status === 'error' && (
                  <span className="upscale-status error" title={item.error}>Failed</span>
                )}
                {item.status !== 'processing' && (
                  <button
                    className="upscale-remove-button"
                    onClick={() => removeFromUpscaleQueue(index)}
                    title="Remove"
                  >
                    &times;
                  </button>
                )}
              </div>
            </div>
            <div className="upscale-comparison">
              <div className="upscale-image-panel">
                <span className="upscale-panel-label">Original</span>
                <img src={item.originalData} alt="Original" className="upscale-image" />
              </div>
              {item.upscaledData && (
                <div className="upscale-image-panel">
                  <span className="upscale-panel-label">Upscaled (4x)</span>
                  <img
                    src={`data:image/png;base64,${item.upscaledData}`}
                    alt="Upscaled"
                    className="upscale-image"
                  />
                  <button
                    className="save-image-button"
                    onClick={() => saveUpscaledImage(item.upscaledData!, item.fileName)}
                  >
                    <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                      <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/>
                      <polyline points="7 10 12 15 17 10"/>
                      <line x1="12" y1="15" x2="12" y2="3"/>
                    </svg>
                    Save
                  </button>
                </div>
              )}
            </div>
          </div>
        ))}

        <div ref={messagesEndRef} />
      </div>

      <div className="chat-input-container">
        <div className="chat-input-wrapper upscale-controls">
          <input
            type="file"
            ref={upscaleFileInputRef}
            accept="image/*"
            multiple
            style={{ display: 'none' }}
            onChange={handleUpscaleFileSelect}
          />
          <div className="chat-controls">
            <div className="chat-controls-left">
              <ModelSelector disabled={isUpscaling} />
              <button
                className="image-upload-button"
                onClick={() => upscaleFileInputRef.current?.click()}
                disabled={isUpscaling}
                title="Add images"
              >
                <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                  <rect x="3" y="3" width="18" height="18" rx="2" ry="2"/>
                  <line x1="12" y1="8" x2="12" y2="16"/>
                  <line x1="8" y1="12" x2="16" y2="12"/>
                </svg>
              </button>
            </div>
            {isUpscaling && (
              <span className="upscale-progress">
                Processing {upscaleProgress.current}/{upscaleProgress.total}
              </span>
            )}
            <button
              className="chat-send-button"
              onClick={handleUpscaleProcess}
              disabled={isUpscaling || upscaleQueue.filter(i => i.status === 'pending').length === 0}
              title="Upscale all pending images"
            >
              {isUpscaling ? (
                <div className="typing-indicator small">
                  <span></span>
                  <span></span>
                  <span></span>
                </div>
              ) : (
                <svg width="16" height="16" viewBox="0 0 24 24" fill="none">
                  <path d="M7 17L17 7M17 7H7M17 7V17" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"/>
                </svg>
              )}
            </button>
          </div>
        </div>
      </div>
    </>
  );
};

export default UpscalePanel;
