import React, { useState, useRef, useEffect, useMemo } from 'react';
import { useModels } from '../../hooks/useModels';
import { Modality } from '../../hooks/useInferenceState';
import { ModelsData } from '../../utils/modelData';
import { serverFetch } from '../../utils/serverConfig';
import { adjustTextareaHeight } from '../../utils/textareaUtils';
import InferenceControls from '../InferenceControls';
import ModelSelector from '../ModelSelector';
import EmptyState from '../EmptyState';

interface ImageSettings {
  steps: number;
  cfgScale: number;
  width: number;
  height: number;
  seed: number;
}

const DEFAULT_IMAGE_SETTINGS: ImageSettings = {
  steps: 20,
  cfgScale: 7.0,
  width: 512,
  height: 512,
  seed: -1,
};

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
  const [imagePrompt, setImagePrompt] = useState('');
  const [imageHistory, setImageHistory] = useState<Array<{
    prompt: string;
    imageData: string;
    timestamp: number;
  }>>([]);
  const [imageSettings, setImageSettings] = useState<ImageSettings>(DEFAULT_IMAGE_SETTINGS);
  const inputRef = useRef<HTMLTextAreaElement>(null);
  const messagesEndRef = useRef<HTMLDivElement>(null);

  // Load model-specific image defaults when the selected model changes
  useEffect(() => {
    const modelInfo = modelsData[selectedModel];
    const defaults = modelInfo?.image_defaults;
    setImageSettings({
      steps: defaults?.steps ?? DEFAULT_IMAGE_SETTINGS.steps,
      cfgScale: defaults?.cfg_scale ?? DEFAULT_IMAGE_SETTINGS.cfgScale,
      width: defaults?.width ?? DEFAULT_IMAGE_SETTINGS.width,
      height: defaults?.height ?? DEFAULT_IMAGE_SETTINGS.height,
      seed: -1,
    });
  }, [selectedModel, modelsData]);

  // Auto-scroll to bottom when new images are generated
  useEffect(() => {
    if (imageHistory.length > 0) {
      requestAnimationFrame(() => {
        messagesEndRef.current?.scrollIntoView({ behavior: 'smooth' });
      });
    }
  }, [imageHistory.length]);

  const handleImageGeneration = async () => {
    if (!imagePrompt.trim() || isBusy) return;

    const ready = await runPreFlight('image', {
      modelName: selectedModel,
      modelsData,
      onError: showError,
    });
    if (!ready) return;

    const currentPrompt = imagePrompt;
    setImagePrompt('');

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

      const response = await serverFetch('/images/generations', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(requestBody),
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
        }]);
      } else {
        throw new Error('Unexpected response format');
      }
    } catch (error: any) {
      console.error('Failed to generate image:', error);
      showError(`Failed to generate image: ${error.message || 'Unknown error'}`);
    } finally {
      reset();
    }
  };

  const saveGeneratedImage = (imageData: string, prompt: string) => {
    const link = document.createElement('a');
    link.href = `data:image/png;base64,${imageData}`;
    const sanitizedPrompt = prompt.slice(0, 30).replace(/[^a-z0-9]/gi, '_');
    const filename = `lemonade_${sanitizedPrompt}_${Date.now()}.png`;
    link.download = filename;
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
  };

  return (
    <>
      <div className="chat-messages">
        {imageHistory.length === 0 && <EmptyState title="Lemonade Image Generator" />}

        {imageHistory.map((item, index) => (
          <div key={index} className="image-generation-item">
            <div className="image-prompt-display">
              <span className="prompt-label">Prompt:</span>
              <span className="prompt-text">{item.prompt}</span>
            </div>
            <div className="generated-image-container">
              <img
                src={`data:image/png;base64,${item.imageData}`}
                alt={item.prompt}
                className="generated-image"
              />
              <button
                className="save-image-button"
                onClick={() => saveGeneratedImage(item.imageData, item.prompt)}
              >
                <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                  <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/>
                  <polyline points="7 10 12 15 17 10"/>
                  <line x1="12" y1="15" x2="12" y2="3"/>
                </svg>
                Save Image
              </button>
            </div>
          </div>
        ))}

        {isBusy && activeModality === 'image' && (
          <div className="image-generating-indicator">
            <div className="generating-spinner"></div>
            <span>Generating image...</span>
          </div>
        )}
        <div ref={messagesEndRef} />
      </div>

      {/* Image Settings Panel */}
      <div className="image-settings-panel">
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
        <div className="image-setting">
          <label>Seed</label>
          <input type="number" min="-1" value={imageSettings.seed}
            onChange={(e) => setImageSettings(prev => ({ ...prev, seed: parseInt(e.target.value) || -1 }))}
            disabled={isBusy} placeholder="-1 = random" />
        </div>
      </div>

      <div className="chat-input-container">
        <div className="chat-input-wrapper">
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
            placeholder="Describe the image you want to generate..."
            rows={1}
          />
          <InferenceControls
            isBusy={isBusy}
            isInferring={isInferring}
            stoppable={false}
            onSend={handleImageGeneration}
            sendDisabled={!imagePrompt.trim() || isBusy}
            modelSelector={<ModelSelector disabled={isBusy} />}
          />
        </div>
      </div>
    </>
  );
};

export default ImageGenerationPanel;
