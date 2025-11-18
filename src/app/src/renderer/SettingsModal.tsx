import React, { useState, useEffect } from 'react';

interface SettingsModalProps {
  isOpen: boolean;
  onClose: () => void;
}

interface Settings {
  temperature: number;
  topK: number;
  topP: number;
  repeatPenalty: number;
  enableThinking: boolean;
}

const DEFAULT_SETTINGS: Settings = {
  temperature: 0.7,
  topK: 40,
  topP: 0.9,
  repeatPenalty: 1.1,
  enableThinking: false,
};

const SettingsModal: React.FC<SettingsModalProps> = ({ isOpen, onClose }) => {
  const [settings, setSettings] = useState<Settings>(DEFAULT_SETTINGS);

  const handleReset = () => {
    setSettings(DEFAULT_SETTINGS);
  };

  const handleSave = () => {
    // For now, just close the modal
    // In the future, this will save settings to backend/storage
    console.log('Settings saved:', settings);
    onClose();
  };

  const handleOverlayClick = (e: React.MouseEvent) => {
    if (e.target === e.currentTarget) {
      onClose();
    }
  };

  if (!isOpen) return null;

  return (
    <div className="settings-overlay" onClick={handleOverlayClick}>
      <div className="settings-modal">
        <div className="settings-header">
          <h2>Settings</h2>
          <button className="settings-close-button" onClick={onClose} title="Close">
            <svg width="14" height="14" viewBox="0 0 14 14">
              <path d="M 1,1 L 13,13 M 13,1 L 1,13" stroke="currentColor" strokeWidth="2" strokeLinecap="round"/>
            </svg>
          </button>
        </div>

        <div className="settings-content">
          <div className="settings-section">
            <label className="settings-label">
              <span className="settings-label-text">Temperature</span>
              <span className="settings-description">
                Controls randomness in responses (0 = deterministic, 2 = very random)
              </span>
            </label>
            <div className="settings-input-group">
              <input
                type="range"
                min="0"
                max="2"
                step="0.1"
                value={settings.temperature}
                onChange={(e) => setSettings({ ...settings, temperature: parseFloat(e.target.value) })}
                className="settings-slider"
              />
              <input
                type="number"
                min="0"
                max="2"
                step="0.1"
                value={settings.temperature}
                onChange={(e) => setSettings({ ...settings, temperature: parseFloat(e.target.value) })}
                className="settings-number-input"
              />
            </div>
          </div>

          <div className="settings-section">
            <label className="settings-label">
              <span className="settings-label-text">Top K</span>
              <span className="settings-description">
                Limits token selection to top K most likely tokens
              </span>
            </label>
            <div className="settings-input-group">
              <input
                type="range"
                min="1"
                max="100"
                step="1"
                value={settings.topK}
                onChange={(e) => setSettings({ ...settings, topK: parseInt(e.target.value) })}
                className="settings-slider"
              />
              <input
                type="number"
                min="1"
                max="100"
                step="1"
                value={settings.topK}
                onChange={(e) => setSettings({ ...settings, topK: parseInt(e.target.value) })}
                className="settings-number-input"
              />
            </div>
          </div>

          <div className="settings-section">
            <label className="settings-label">
              <span className="settings-label-text">Top P</span>
              <span className="settings-description">
                Nucleus sampling - considers tokens with cumulative probability up to P
              </span>
            </label>
            <div className="settings-input-group">
              <input
                type="range"
                min="0"
                max="1"
                step="0.01"
                value={settings.topP}
                onChange={(e) => setSettings({ ...settings, topP: parseFloat(e.target.value) })}
                className="settings-slider"
              />
              <input
                type="number"
                min="0"
                max="1"
                step="0.01"
                value={settings.topP}
                onChange={(e) => setSettings({ ...settings, topP: parseFloat(e.target.value) })}
                className="settings-number-input"
              />
            </div>
          </div>

          <div className="settings-section">
            <label className="settings-label">
              <span className="settings-label-text">Repeat Penalty</span>
              <span className="settings-description">
                Penalty for repeating tokens (1 = no penalty, &gt;1 = less repetition)
              </span>
            </label>
            <div className="settings-input-group">
              <input
                type="range"
                min="1"
                max="2"
                step="0.1"
                value={settings.repeatPenalty}
                onChange={(e) => setSettings({ ...settings, repeatPenalty: parseFloat(e.target.value) })}
                className="settings-slider"
              />
              <input
                type="number"
                min="1"
                max="2"
                step="0.1"
                value={settings.repeatPenalty}
                onChange={(e) => setSettings({ ...settings, repeatPenalty: parseFloat(e.target.value) })}
                className="settings-number-input"
              />
            </div>
          </div>

          <div className="settings-section">
            <label className="settings-checkbox-label">
              <input
                type="checkbox"
                checked={settings.enableThinking}
                onChange={(e) => setSettings({ ...settings, enableThinking: e.target.checked })}
                className="settings-checkbox"
              />
              <div className="settings-checkbox-content">
                <span className="settings-label-text">Enable Thinking</span>
                <span className="settings-description">
                  Determines whether hybrid reasoning models, such as Qwen3, will use thinking.
                </span>
              </div>
            </label>
          </div>
        </div>

        <div className="settings-footer">
          <button className="settings-reset-button" onClick={handleReset}>
            Reset to Defaults
          </button>
          <button className="settings-save-button" onClick={handleSave}>
            Save
          </button>
        </div>
      </div>
    </div>
  );
};

export default SettingsModal;

