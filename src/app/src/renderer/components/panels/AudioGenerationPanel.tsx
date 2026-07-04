import React, { useState, useEffect, useRef } from 'react';
import { useModels } from '../../hooks/useModels';
import { Modality } from '../../hooks/useInferenceState';
import { ModelsData } from '../../utils/modelData';
import { AppSettings } from '../../utils/appSettings';
import { serverFetch } from '../../utils/serverConfig';
import ModelSelector from '../ModelSelector';
import EmptyState from '../EmptyState';
import InferenceControls from '../InferenceControls';

interface AudioGenerationPanelProps {
  isBusy: boolean;
  isPreFlight: boolean;
  isInferring: boolean;
  activeModality: Modality | null;
  runPreFlight: (modality: Modality, options: { modelName: string; modelsData: ModelsData; onError: (msg: string) => void }) => Promise<boolean>;
  reset: () => void;
  showError: (msg: string) => void;
  appSettings: AppSettings | null;
}

interface GeneratedClip {
  url: string;
  prompt: string;
}

const AudioGenerationPanel: React.FC<AudioGenerationPanelProps> = ({
  isBusy, isPreFlight, isInferring, activeModality, runPreFlight, reset, showError,
}) => {
  const { selectedModel, modelsData } = useModels();

  const [prompt, setPrompt] = useState('');
  const [duration, setDuration] = useState(10);
  const [clips, setClips] = useState<GeneratedClip[]>([]);
  const clipsRef = useRef<GeneratedClip[]>([]);
  clipsRef.current = clips;

  useEffect(() => () => { clipsRef.current.forEach(c => URL.revokeObjectURL(c.url)); }, []);

  // Music (ACE-Step) is long-form, so default to a full clip; SFX (ThinkSound)
  // stays short. Resets to the model's default when the selected model changes.
  const audioRecipe = modelsData?.[selectedModel]?.recipe || '';
  useEffect(() => {
    setDuration(audioRecipe === 'acestep' ? 150 : 10);
  }, [audioRecipe]);

  const handleGenerate = async () => {
    if (!prompt.trim() || isBusy || !selectedModel) return;

    const ready = await runPreFlight('audio', { modelName: selectedModel, modelsData, onError: showError });
    if (!ready) return;

    try {
      const response = await serverFetch('/audio/generations', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ model: selectedModel, prompt, duration }),
      });
      if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
      const blob = await response.blob();
      setClips(prev => [...prev, { url: URL.createObjectURL(blob), prompt }]);
    } catch (error: any) {
      console.error('Audio generation failed:', error);
      showError(`Failed to generate audio: ${error.message || 'Unknown error'}`);
    } finally {
      reset();
    }
  };

  const handleKeyDown = (e: React.KeyboardEvent) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      handleGenerate();
    }
  };

  return (
    <>
      <div className="chat-messages">
        {clips.length === 0 && !isBusy && <EmptyState title="Lemonade Audio Generator" />}
        {clips.map((clip, index) => (
          <div key={index} className="chat-message user-message">
            <div className="message-content">
              <p>{clip.prompt}</p>
              <audio controls src={clip.url} style={{ width: '100%', marginTop: '8px' }} />
              <a href={clip.url} download={`audio-${index + 1}.wav`} className="download-link" style={{ display: 'inline-block', marginTop: '6px' }}>
                Download
              </a>
            </div>
          </div>
        ))}
        {isPreFlight && activeModality === 'audio' && (
          <div className="model-loading-indicator"><span className="model-loading-text">Loading audio model...</span></div>
        )}
        {isInferring && activeModality === 'audio' && (
          <div className="model-loading-indicator"><span className="model-loading-text">Generating audio...</span></div>
        )}
      </div>

      <div className="chat-input-container">
        <div className="chat-input-wrapper">
          <textarea
            className="chat-input"
            value={prompt}
            onChange={(e) => setPrompt(e.target.value)}
            onKeyDown={handleKeyDown}
            placeholder="Describe the music or sound effect to generate..."
            rows={1}
          />
          <InferenceControls
            isBusy={isBusy}
            isInferring={isInferring}
            stoppable={false}
            onSend={handleGenerate}
            sendDisabled={!prompt.trim()}
            sendTitle="Generate"
            modelSelector={<ModelSelector disabled={isBusy} />}
            leftControls={
              <label className="audio-duration-label" style={{ fontSize: '0.85em', whiteSpace: 'nowrap' }}>
                Duration
                <input
                  type="number"
                  min={1}
                  max={600}
                  value={duration}
                  onChange={(e) => setDuration(Math.max(1, Math.min(600, Number(e.target.value) || 1)))}
                  disabled={isBusy}
                /> s
              </label>
            }
          />
        </div>
      </div>
    </>
  );
};

export default AudioGenerationPanel;
