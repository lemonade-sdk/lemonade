import React, { useState, useEffect, useRef } from 'react';
import { createPortal } from 'react-dom';
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

const LYRICS_EXAMPLE = `[verse]
Moonlight spills across the floor
Shadows dancing by the door

[chorus]
We sing until the morning light
Carried on the wind tonight

[bridge]
Hold the note and let it soar

[outro]
Fading softly, nothing more`;

const AudioGenerationPanel: React.FC<AudioGenerationPanelProps> = ({
  isBusy, isPreFlight, isInferring, activeModality, runPreFlight, reset, showError,
}) => {
  const { selectedModel, modelsData } = useModels();

  const [prompt, setPrompt] = useState('');
  const [lyrics, setLyrics] = useState('');
  const [vocalLanguage, setVocalLanguage] = useState('en');
  const [showLyricsHelp, setShowLyricsHelp] = useState(false);
  const [duration, setDuration] = useState(10);
  const [clips, setClips] = useState<GeneratedClip[]>([]);
  const clipsRef = useRef<GeneratedClip[]>([]);
  clipsRef.current = clips;

  useEffect(() => () => { clipsRef.current.forEach(c => URL.revokeObjectURL(c.url)); }, []);

  const audioRecipe = modelsData?.[selectedModel]?.recipe || '';
  const isMusic = audioRecipe === 'acestep';
  useEffect(() => {
    setDuration(isMusic ? 150 : 10);
  }, [audioRecipe]);

  const handleGenerate = async () => {
    if (!prompt.trim() || isBusy || !selectedModel) return;

    const ready = await runPreFlight('audio', { modelName: selectedModel, modelsData, onError: showError });
    if (!ready) return;

    try {
      const trimmedLyrics = lyrics.trim();
      const response = await serverFetch('/audio/generations', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          model: selectedModel,
          prompt,
          duration,
          ...(isMusic && trimmedLyrics
            ? { lyrics, vocal_language: vocalLanguage.trim() || 'en' }
            : {}),
        }),
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
          {isMusic ? (
            <>
              <div className="style-input-row">
                <input
                  type="text"
                  className="chat-input"
                  value={prompt}
                  onChange={(e) => setPrompt(e.target.value)}
                  onKeyDown={handleKeyDown}
                  placeholder="Style description — genre, mood, tempo, instruments, voice..."
                />
                <label className="vocal-language-label" title="Vocal language (BCP-47 code, e.g. en, fr, ja)">
                  Lang
                  <input
                    type="text"
                    value={vocalLanguage}
                    onChange={(e) => setVocalLanguage(e.target.value)}
                    maxLength={8}
                    disabled={isBusy}
                  />
                </label>
              </div>
              <div className="lyrics-input-row">
                <textarea
                  className="chat-input lyrics-input"
                  value={lyrics}
                  onChange={(e) => setLyrics(e.target.value)}
                  placeholder="Lyrics (optional) — leave empty for an instrumental track"
                  rows={4}
                />
                <button
                  className="lyrics-help-button"
                  title="Lyrics syntax guide"
                  onClick={() => setShowLyricsHelp(true)}
                >?</button>
              </div>
            </>
          ) : (
            <textarea
              className="chat-input"
              value={prompt}
              onChange={(e) => setPrompt(e.target.value)}
              onKeyDown={handleKeyDown}
              placeholder="Describe the music or sound effect to generate..."
              rows={1}
            />
          )}
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

      {showLyricsHelp && createPortal(
        <div
          className="settings-overlay"
          onMouseDown={(e: React.MouseEvent<HTMLDivElement>) => { if (e.target === e.currentTarget) setShowLyricsHelp(false); }}
        >
          <div className="settings-modal" onMouseDown={(e: React.MouseEvent) => e.stopPropagation()}>
            <div className="settings-header">
              <h3>Lyrics syntax</h3>
              <button className="settings-close-button" onClick={() => setShowLyricsHelp(false)} title="Close">
                <svg width="14" height="14" viewBox="0 0 14 14">
                  <path d="M 1,1 L 13,13 M 13,1 L 1,13" stroke="currentColor" strokeWidth="2" strokeLinecap="round" />
                </svg>
              </button>
            </div>
            <div className="settings-content lyrics-help-content">
              <p>
                Mark each song section with a structure tag on its own line:{' '}
                <code>[verse]</code>, <code>[chorus]</code>, <code>[bridge]</code>,{' '}
                <code>[intro]</code>, <code>[outro]</code>.
              </p>
              <ul>
                <li>Write one sung phrase per line and separate sections with a blank line.</li>
                <li>Leave the lyrics field empty (or write <code>[instrumental]</code>) for a track without vocals.</li>
                <li>Describe the voice in the style field (e.g. &quot;gentle female vocals&quot;, &quot;raspy male baritone&quot;), not in the lyrics.</li>
                <li>Lyrics don&apos;t have to be English; write them in the language they should be sung in.</li>
              </ul>
              <p>Example:</p>
              <pre>{LYRICS_EXAMPLE}</pre>
            </div>
          </div>
        </div>,
        document.body
      )}
    </>
  );
};

export default AudioGenerationPanel;
