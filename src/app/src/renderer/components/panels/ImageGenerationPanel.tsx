import React, { useState, useRef, useEffect, useMemo } from 'react';
import { useModels } from '../../hooks/useModels';
import { Modality } from '../../hooks/useInferenceState';
import { ModelsData } from '../../utils/modelData';
import { serverFetch } from '../../utils/serverConfig';
import { pullModel } from '../../utils/backendInstaller';
import { adjustTextareaHeight } from '../../utils/textareaUtils';
import InferenceControls from '../InferenceControls';
import ModelSelector from '../ModelSelector';
import EmptyState from '../EmptyState';
import { ImageUploadIcon } from '../Icons';

type ImageMode = 'generate' | 'edit' | 'variations';

const IMAGE_MODE_LABELS: Record<string, string> = {
  generate: 'Generate',
  edit: 'Edit',
  // variations: 'Variations',  // omitted from dropdown
};

interface ImageSettings {
  steps: number;
  cfgScale: number;
  width: number;
  height: number;
  seed: number;
  upscaleModel: string;
}

const DEFAULT_IMAGE_SETTINGS: ImageSettings = {
  steps: 20,
  cfgScale: 7.0,
  width: 512,
  height: 512,
  seed: -1,
  upscaleModel: '',
};

interface ImageHistoryItem {
  prompt: string;
  imageData: string;
  originalImageData?: string;
  timestamp: number;
  generateMs?: number;
  upscaleMs?: number;
  isUpscaling?: boolean;
  mode: ImageMode;
}

interface ImageGenerationPanelProps {
  isBusy: boolean;
  isInferring: boolean;
  activeModality: Modality | null;
  runPreFlight: (modality: Modality, options: { modelName: string; modelsData: ModelsData; onError: (msg: string) => void }) => Promise<boolean>;
  reset: () => void;
  showError: (msg: string) => void;
}

const ImageGenerationPanel: React.FC<ImageGenerationPanelProps> = ({
  isBusy, isInferring, activeModality,
  runPreFlight, reset, showError,
}) => {
  const { selectedModel, modelsData } = useModels();
  const [imageMode, setImageMode] = useState<ImageMode>('generate');
  const [imagePrompt, setImagePrompt] = useState('');
  const [imageHistory, setImageHistory] = useState<ImageHistoryItem[]>([]);
  const [imageSettings, setImageSettings] = useState<ImageSettings>(DEFAULT_IMAGE_SETTINGS);
  const [generationStage, setGenerationStage] = useState<string | null>(null);
  const [referenceImage, setReferenceImage] = useState<{ dataUrl: string; blob: Blob } | null>(null);
  const inputRef = useRef<HTMLTextAreaElement>(null);
  const messagesEndRef = useRef<HTMLDivElement>(null);
  const fileInputRef = useRef<HTMLInputElement>(null);

  const supportsEdit = useMemo(() => {
    return modelsData[selectedModel]?.labels?.includes('edit') || false;
  }, [selectedModel, modelsData]);

  useEffect(() => {
    const modelInfo = modelsData[selectedModel];
    const defaults = modelInfo?.image_defaults;
    setImageSettings(prev => ({
      steps: defaults?.steps ?? DEFAULT_IMAGE_SETTINGS.steps,
      cfgScale: defaults?.cfg_scale ?? DEFAULT_IMAGE_SETTINGS.cfgScale,
      width: defaults?.width ?? DEFAULT_IMAGE_SETTINGS.width,
      height: defaults?.height ?? DEFAULT_IMAGE_SETTINGS.height,
      seed: -1,
      upscaleModel: prev.upscaleModel,
    }));
    // Reset to generate mode if the new model doesn't support editing
    if (!modelsData[selectedModel]?.labels?.includes('edit') && imageMode !== 'generate') {
      setImageMode('generate');
      setReferenceImage(null);
    }
  }, [selectedModel, modelsData]);

  const handleUpscaleChange = async (upscaleModel: string) => {
    setImageSettings(prev => ({ ...prev, upscaleModel }));
    if (upscaleModel) {
      try {
        const res = await serverFetch(`/models/${upscaleModel}`);
        const info = await res.json();
        if (!info.downloaded) {
          await pullModel(upscaleModel, { showInDownloadManager: true });
        }
      } catch (error: any) {
        console.error('Failed to download upscale model:', error);
        showError(`Failed to download upscale model: ${error.message || 'Unknown error'}`);
        setImageSettings(prev => ({ ...prev, upscaleModel: '' }));
      }
    }
  };

  useEffect(() => {
    if (imageHistory.length > 0) {
      requestAnimationFrame(() => {
        messagesEndRef.current?.scrollIntoView({ behavior: 'smooth' });
      });
    }
  }, [imageHistory.length]);

  const handleReferenceUpload = (event: React.ChangeEvent<HTMLInputElement>) => {
    const files = event.target.files;
    if (!files || files.length === 0) return;
    const file = files[0];
    const reader = new FileReader();
    reader.onload = () => {
      setReferenceImage({ dataUrl: reader.result as string, blob: file });
    };
    reader.readAsDataURL(file);
    event.target.value = '';
  };

  const base64ToBlob = (b64: string, type: string): Blob => {
    const byteString = atob(b64);
    const bytes = new Uint8Array(byteString.length);
    for (let i = 0; i < byteString.length; i++) bytes[i] = byteString.charCodeAt(i);
    return new Blob([bytes], { type });
  };

  const useAsReference = (imageData: string, mode: ImageMode) => {
    const blob = base64ToBlob(imageData, 'image/png');
    setReferenceImage({ dataUrl: `data:image/png;base64,${imageData}`, blob });
    setImageMode(mode);
  };

  const sendMultipartRequest = async (endpoint: string, buildForm: (form: FormData) => void): Promise<Response> => {
    const formData = new FormData();
    formData.append('model', selectedModel);
    formData.append('size', `${imageSettings.width}x${imageSettings.height}`);
    formData.append('response_format', 'b64_json');
    buildForm(formData);
    return serverFetch(endpoint, { method: 'POST', body: formData });
  };

  const handleImageGeneration = async () => {
    if (imageMode === 'edit') {
      await handleImageEdit();
      return;
    }
    if (imageMode === 'variations') {
      await handleImageVariations();
      return;
    }

    if (!imagePrompt.trim() || isBusy) return;

    const ready = await runPreFlight('image', {
      modelName: selectedModel,
      modelsData,
      onError: showError,
    });
    if (!ready) return;

    const currentPrompt = imagePrompt;
    setImagePrompt('');
    setGenerationStage('generating');

    try {
      const requestBody: Record<string, unknown> = {
        model: selectedModel,
        prompt: currentPrompt,
        size: `${imageSettings.width}x${imageSettings.height}`,
        steps: imageSettings.steps,
        cfg_scale: imageSettings.cfgScale,
        response_format: 'b64_json',
      };

      if (imageSettings.seed > 0) {
        requestBody.seed = imageSettings.seed;
      }

      const genStart = Date.now();
      const genResponse = await serverFetch('/images/generations', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(requestBody),
      });

      if (!genResponse.ok) {
        const errorData = await genResponse.json();
        throw new Error(errorData.error?.message || `HTTP error! status: ${genResponse.status}`);
      }

      const genData = await genResponse.json();
      const generateMs = Date.now() - genStart;

      if (!genData.data?.[0]?.b64_json) {
        throw new Error('Unexpected response format');
      }

      const generatedImage = genData.data[0].b64_json;

      // Show the generated image immediately
      const historyItem: ImageHistoryItem = {
        prompt: currentPrompt,
        imageData: generatedImage,
        timestamp: Date.now(),
        generateMs,
        isUpscaling: !!imageSettings.upscaleModel,
        mode: 'generate',
      };
      setImageHistory(prev => [...prev, historyItem]);

      // Upscale as a second step if enabled
      if (imageSettings.upscaleModel) {
        setGenerationStage('upscaling');
        const upscaleStart = Date.now();

        const upscaleResponse = await serverFetch('/images/upscale', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({
            model: imageSettings.upscaleModel,
            image: generatedImage,
          }),
        });

        if (!upscaleResponse.ok) {
          const errorData = await upscaleResponse.json();
          throw new Error(errorData.error?.message || 'Upscale failed');
        }

        const upscaleData = await upscaleResponse.json();
        const upscaleMs = Date.now() - upscaleStart;

        if (upscaleData.data?.[0]?.b64_json) {
          setImageHistory(prev => prev.map((item, i) =>
            i === prev.length - 1
              ? { ...item, originalImageData: generatedImage, imageData: upscaleData.data[0].b64_json, upscaleMs, isUpscaling: false }
              : item
          ));
        } else {
          setImageHistory(prev => prev.map((item, i) =>
            i === prev.length - 1 ? { ...item, isUpscaling: false } : item
          ));
        }
      }
    } catch (error: any) {
      console.error('Failed to generate image:', error);
      showError(`Failed to generate image: ${error.message || 'Unknown error'}`);
    } finally {
      setImageHistory(prev => prev.map(item =>
        item.isUpscaling ? { ...item, isUpscaling: false } : item
      ));
      setGenerationStage(null);
      reset();
    }
  };

  const handleImageEdit = async () => {
    if (!referenceImage || !imagePrompt.trim() || isBusy) return;

    const ready = await runPreFlight('image', {
      modelName: selectedModel,
      modelsData,
      onError: showError,
    });
    if (!ready) return;

    const currentPrompt = imagePrompt;
    setImagePrompt('');

    try {
      const response = await sendMultipartRequest('/images/edits', (form) => {
        form.append('image', referenceImage.blob, 'image.png');
        form.append('prompt', currentPrompt);
        form.append('steps', String(imageSettings.steps));
        form.append('cfg_scale', String(imageSettings.cfgScale));
        if (imageSettings.seed > 0) form.append('seed', String(imageSettings.seed));
      });

      if (!response.ok) {
        const errorData = await response.json();
        throw new Error(errorData.error?.message || `HTTP error! status: ${response.status}`);
      }

      const data = await response.json();

      if (data.data && data.data[0] && data.data[0].b64_json) {
        setImageHistory(prev => [...prev, {
          prompt: currentPrompt,
          imageData: data.data[0].b64_json,
          timestamp: Date.now(),
          mode: 'edit',
        }]);
      } else {
        throw new Error('Unexpected response format');
      }
    } catch (error: any) {
      console.error('Failed to edit image:', error);
      showError(`Failed to edit image: ${error.message || 'Unknown error'}`);
    } finally {
      reset();
    }
  };

  const handleImageVariations = async () => {
    if (!referenceImage || isBusy) return;

    const ready = await runPreFlight('image', {
      modelName: selectedModel,
      modelsData,
      onError: showError,
    });
    if (!ready) return;

    try {
      const response = await sendMultipartRequest('/images/variations', (form) => {
        form.append('image', referenceImage.blob, 'image.png');
      });

      if (!response.ok) {
        const errorData = await response.json();
        throw new Error(errorData.error?.message || `HTTP error! status: ${response.status}`);
      }

      const data = await response.json();

      if (data.data && data.data[0] && data.data[0].b64_json) {
        setImageHistory(prev => [...prev, {
          prompt: '',
          imageData: data.data[0].b64_json,
          timestamp: Date.now(),
          mode: 'variations',
        }]);
      } else {
        throw new Error('Unexpected response format');
      }
    } catch (error: any) {
      console.error('Failed to generate variation:', error);
      showError(`Failed to generate variation: ${error.message || 'Unknown error'}`);
    } finally {
      reset();
    }
  };

  const saveGeneratedImage = (imageData: string, prompt: string) => {
    const blob = base64ToBlob(imageData, 'image/png');
    const url = URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.href = url;
    const sanitizedPrompt = prompt.slice(0, 30).replace(/[^a-z0-9]/gi, '_');
    const filename = `lemonade_${sanitizedPrompt}_${Date.now()}.png`;
    link.download = filename;
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
    URL.revokeObjectURL(url);
  };

  const formatTime = (ms: number) => {
    if (ms < 1000) return `${ms}ms`;
    return `${(ms / 1000).toFixed(1)}s`;
  };

  const isSendDisabled = () => {
    if (isBusy) return true;
    if (imageMode === 'edit') return !imagePrompt.trim() || !referenceImage;
    if (imageMode === 'variations') return !referenceImage;
    return !imagePrompt.trim();
  };

  const placeholderText = imageMode === 'variations'
    ? 'Upload a reference image to generate variations'
    : imageMode === 'edit'
    ? 'Describe how to edit the reference image...'
    : 'Describe the image you want to generate...';

  return (
    <>
      <div className="chat-messages">
        {imageHistory.length === 0 && <EmptyState title="Lemonade Image Generator" />}

        {imageHistory.map((item, index) => (
          <div key={index} className="image-generation-item">
            <div className="image-prompt-display">
              <span className="prompt-label">
                {item.mode === 'edit' ? 'Edit:' : item.mode === 'variations' ? 'Variation' : 'Prompt:'}
              </span>
              {item.mode !== 'variations' && <span className="prompt-text">{item.prompt}</span>}
              {(item.generateMs || item.upscaleMs) && (
                <span className="image-timing">
                  {item.generateMs ? `Generated in ${formatTime(item.generateMs)}` : ''}
                  {item.generateMs && item.upscaleMs ? ' | ' : ''}
                  {item.upscaleMs ? `Upscaled in ${formatTime(item.upscaleMs)}` : ''}
                </span>
              )}
            </div>
            <div className={`generated-images-row${(item.originalImageData || item.isUpscaling) ? ' side-by-side' : ''}`}>
              <div className="generated-image-column">
                {(item.originalImageData || item.isUpscaling) && <div className="image-label">Original</div>}
                <div className="image-wrapper">
                  <img
                    src={`data:image/png;base64,${item.originalImageData || item.imageData}`}
                    alt={`${item.prompt}${item.originalImageData ? ' (original)' : ''}`}
                    className="generated-image"
                  />
                </div>
              </div>
              {item.isUpscaling && (
                <div className="generated-image-column">
                  <div className="image-label">Upscaled (4x)</div>
                  <div className="upscale-placeholder">
                    <div className="generating-spinner"></div>
                    <span>Upscaling...</span>
                  </div>
                </div>
              )}
              {item.originalImageData && !item.isUpscaling && (
                <div className="generated-image-column">
                  <div className="image-label">Upscaled (4x)</div>
                  <div className="image-wrapper">
                    <img
                      src={`data:image/png;base64,${item.imageData}`}
                      alt={item.prompt}
                      className="generated-image"
                    />
                  </div>
                </div>
              )}
            </div>
            <div className="image-action-buttons">
              {/* Variations button commented out until variations mode is functional
              <button
                className="save-image-button"
                onClick={() => useAsReference(item.imageData, 'variations')}
                disabled={isBusy}
                title="Generate variations of this image"
              >
                <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                  <rect x="2" y="2" width="8" height="8" rx="1"/>
                  <rect x="14" y="2" width="8" height="8" rx="1"/>
                  <rect x="2" y="14" width="8" height="8" rx="1"/>
                  <rect x="14" y="14" width="8" height="8" rx="1"/>
                </svg>
                Variations
              </button>
              */}
              {supportsEdit && (
              <button
                className="save-image-button"
                onClick={() => useAsReference(item.imageData, 'edit')}
                disabled={isBusy}
                title="Edit this image with a prompt"
              >
                <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                  <path d="M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7"/>
                  <path d="M18.5 2.5a2.121 2.121 0 0 1 3 3L12 15l-4 1 1-4 9.5-9.5z"/>
                </svg>
                Edit
              </button>
              )}
              <button
                className="save-image-button"
                onClick={() => saveGeneratedImage(item.originalImageData || item.imageData, item.prompt + (item.originalImageData ? '_original' : ''))}
              >
                <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                  <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/>
                  <polyline points="7 10 12 15 17 10"/>
                  <line x1="12" y1="15" x2="12" y2="3"/>
                </svg>
                {item.originalImageData ? 'Save Original' : 'Save Image'}
              </button>
              {item.originalImageData && !item.isUpscaling && (
              <button
                className="save-image-button"
                onClick={() => saveGeneratedImage(item.imageData, item.prompt)}
              >
                <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                  <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/>
                  <polyline points="7 10 12 15 17 10"/>
                  <line x1="12" y1="15" x2="12" y2="3"/>
                </svg>
                Save Upscaled
              </button>
              )}
            </div>
          </div>
        ))}

        {isBusy && activeModality === 'image' && (
          <div className="image-generating-indicator">
            <div className="generating-spinner"></div>
            <span>
              {generationStage === 'upscaling' ? 'Upscaling image...'
                : imageMode === 'edit' ? 'Editing image...'
                : imageMode === 'variations' ? 'Generating variation...'
                : 'Generating image...'}
            </span>
          </div>
        )}
        <div ref={messagesEndRef} />
      </div>

      {/* Mode Selector & Image Settings Panel */}
      <div className="image-settings-panel">
        <div className="image-setting">
          <label>Mode</label>
          <select value={imageMode}
            onChange={(e) => setImageMode(e.target.value as ImageMode)}
            disabled={isBusy}
            style={{ width: 'auto' }}>
            {Object.entries(IMAGE_MODE_LABELS)
              .filter(([value]) => value !== 'edit' || supportsEdit)
              .map(([value, label]) => (
              <option key={value} value={value}>{label}</option>
            ))}
          </select>
        </div>
        {imageMode !== 'variations' && (
          <>
            <div className="image-setting">
              <label>Steps</label>
              <input type="number" min="1" max="50" value={imageSettings.steps}
                onChange={(e) => setImageSettings(prev => ({ ...prev, steps: parseInt(e.target.value) || 1 }))}
                disabled={isBusy} />
            </div>
            <div className="image-setting">
              <label>CFG Scale</label>
              <input type="number" min="1" max="20" step="0.5" value={imageSettings.cfgScale}
                onChange={(e) => setImageSettings(prev => ({ ...prev, cfgScale: parseFloat(e.target.value) || 1 }))}
                disabled={isBusy} />
            </div>
          </>
        )}
        <div className="image-setting">
          <label>Width</label>
          <select value={imageSettings.width}
            onChange={(e) => setImageSettings(prev => ({ ...prev, width: parseInt(e.target.value) }))}
            disabled={isBusy}>
            <option value="512">512</option>
            <option value="768">768</option>
            <option value="1024">1024</option>
          </select>
        </div>
        <div className="image-setting">
          <label>Height</label>
          <select value={imageSettings.height}
            onChange={(e) => setImageSettings(prev => ({ ...prev, height: parseInt(e.target.value) }))}
            disabled={isBusy}>
            <option value="512">512</option>
            <option value="768">768</option>
            <option value="1024">1024</option>
          </select>
        </div>
        {imageMode !== 'variations' && (
          <div className="image-setting">
            <label>Seed</label>
            <input type="number" min="-1" value={imageSettings.seed}
              onChange={(e) => setImageSettings(prev => ({ ...prev, seed: parseInt(e.target.value) || -1 }))}
              disabled={isBusy} placeholder="-1 = random" />
          </div>
        )}
        <div className="image-setting">
          <label>Upscale</label>
          <select value={imageSettings.upscaleModel}
            onChange={(e) => handleUpscaleChange(e.target.value)}
            disabled={isBusy}>
            <option value="">Off</option>
            {Object.entries(modelsData)
              .filter(([_, info]) => info.labels?.includes('esrgan'))
              .map(([name]) => (
                <option key={name} value={name}>{name}</option>
              ))}
          </select>
        </div>
      </div>

      <div className="chat-input-container">
        <div className="chat-input-wrapper">
          {/* Reference image preview for edit/variations modes */}
          {imageMode !== 'generate' && referenceImage && (
            <div className="image-reference-preview">
              <img src={referenceImage.dataUrl} alt="Reference" className="image-reference-thumb" />
              <button
                className="image-remove-button"
                onClick={() => setReferenceImage(null)}
                disabled={isBusy}
                title="Remove reference image"
              >
                &times;
              </button>
            </div>
          )}
          {imageMode !== 'generate' && !referenceImage && (
            <button
              type="button"
              className="image-reference-dropzone"
              onClick={() => fileInputRef.current?.click()}
              disabled={isBusy}
            >
              <ImageUploadIcon />
              <span>Upload reference image</span>
            </button>
          )}
          <input
            ref={fileInputRef}
            type="file"
            accept="image/*"
            style={{ display: 'none' }}
            onChange={handleReferenceUpload}
          />
          {imageMode !== 'variations' && (
            <textarea
              ref={inputRef}
              className="chat-input"
              value={imagePrompt}
              onChange={(e) => {
                setImagePrompt(e.target.value);
                adjustTextareaHeight(e.target);
              }}
              onKeyDown={(e) => {
                if (e.key === 'Enter' && !e.shiftKey) {
                  e.preventDefault();
                  handleImageGeneration();
                }
              }}
              placeholder={placeholderText}
              rows={1}
            />
          )}
          <InferenceControls
            isBusy={isBusy}
            isInferring={isInferring}
            stoppable={false}
            onSend={handleImageGeneration}
            sendDisabled={isSendDisabled()}
            modelSelector={<ModelSelector disabled={isBusy} />}
          />
        </div>
      </div>
    </>
  );
};

export default ImageGenerationPanel;
