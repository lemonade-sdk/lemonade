import React, { useEffect, useState, useCallback } from 'react';
import { open } from '@tauri-apps/plugin-dialog';
import {
  getRuntimeConfig,
  setModelsDir,
  resetModelsDir,
  isModelsDirAuto,
  MODELS_DIR_AUTO_SENTINEL,
} from '../utils/serverRuntimeConfig';

type Status =
  | { kind: 'loading' }
  | { kind: 'ready' }
  | { kind: 'saving' }
  | { kind: 'unauthorized' }
  | { kind: 'error'; message: string }
  | { kind: 'saved' };

const ServerSettings: React.FC = () => {
  // models_dir is server-wide config — it does NOT live in AppSettings.
  // We hold a local draft of the text input plus the last-known server value
  // so the Reset/Apply buttons can do the right thing.
  const [serverModelsDir, setServerModelsDir] = useState<string>(MODELS_DIR_AUTO_SENTINEL);
  const [draftModelsDir, setDraftModelsDir] = useState<string>(MODELS_DIR_AUTO_SENTINEL);
  const [extraModelsDir, setExtraModelsDir] = useState<string>('');
  const [status, setStatus] = useState<Status>({ kind: 'loading' });

  const loadConfig = useCallback(async () => {
    setStatus({ kind: 'loading' });
    const result = await getRuntimeConfig();
    if (result.kind === 'ok') {
      setServerModelsDir(result.modelsDir);
      setDraftModelsDir(result.modelsDir);
      setExtraModelsDir(result.extraModelsDir);
      setStatus({ kind: 'ready' });
    } else if (result.kind === 'unauthorized') {
      setStatus({ kind: 'unauthorized' });
    } else {
      setStatus({ kind: 'error', message: result.message });
    }
  }, []);

  useEffect(() => {
    loadConfig();
  }, [loadConfig]);

  const applyValue = async (path: string) => {
    setStatus({ kind: 'saving' });
    const result = path === MODELS_DIR_AUTO_SENTINEL || isModelsDirAuto(path)
      ? await resetModelsDir()
      : await setModelsDir(path);
    if (result.kind === 'ok') {
      setStatus({ kind: 'saved' });
      // Re-read so we display whatever the server actually persisted.
      const fresh = await getRuntimeConfig();
      if (fresh.kind === 'ok') {
        setServerModelsDir(fresh.modelsDir);
        setDraftModelsDir(fresh.modelsDir);
        setExtraModelsDir(fresh.extraModelsDir);
      }
    } else if (result.kind === 'unauthorized') {
      setStatus({ kind: 'unauthorized' });
    } else {
      setStatus({ kind: 'error', message: result.message });
    }
  };

  const handleBrowse = async () => {
    try {
      // Seed the picker with the current path when it's a real directory; for
      // the `"auto"` sentinel, let the OS default the starting location.
      const defaultPath = isModelsDirAuto(draftModelsDir) ? undefined : draftModelsDir;
      const selected = await open({
        directory: true,
        multiple: false,
        defaultPath,
        title: 'Choose model download folder',
      });
      if (typeof selected === 'string' && selected.length > 0) {
        setDraftModelsDir(selected);
        await applyValue(selected);
      }
    } catch (error) {
      setStatus({
        kind: 'error',
        message: error instanceof Error ? error.message : String(error),
      });
    }
  };

  const handleApply = () => applyValue(draftModelsDir.trim());
  const handleReset = () => applyValue(MODELS_DIR_AUTO_SENTINEL);

  const isAuto = isModelsDirAuto(serverModelsDir);
  const isDirty = draftModelsDir.trim() !== serverModelsDir;
  const isBusy = status.kind === 'loading' || status.kind === 'saving';

  return (
    <div className="settings-section-container">
      <div className={`settings-section ${isAuto ? 'settings-section-default' : ''}`}>
        <div className="settings-label-row">
          <label className="settings-label">
            <span className="settings-label-text">Model download folder</span>
            <span className="settings-description">
              Where the server downloads and stores models. Use <code>auto</code> to follow
              the Hugging Face cache (<code>~/.cache/huggingface/hub</code>). This is a server-wide
              setting — changes apply to every client connected to this server.
            </span>
          </label>
          <button
            type="button"
            className="settings-field-reset"
            onClick={handleReset}
            disabled={isBusy || isAuto}
          >
            Reset
          </button>
        </div>
        <div className="settings-path-row">
          <input
            type="text"
            value={draftModelsDir}
            onChange={(e) => setDraftModelsDir(e.target.value)}
            className="settings-text-input"
            placeholder="auto"
            title="Model download folder"
            disabled={isBusy || status.kind === 'unauthorized'}
          />
          <button
            type="button"
            className="settings-field-reset"
            onClick={handleBrowse}
            disabled={isBusy || status.kind === 'unauthorized'}
          >
            Browse…
          </button>
          <button
            type="button"
            className="settings-field-reset"
            onClick={handleApply}
            disabled={isBusy || !isDirty || status.kind === 'unauthorized'}
          >
            Apply
          </button>
        </div>

        {status.kind === 'loading' && (
          <div className="settings-description">Loading current value…</div>
        )}
        {status.kind === 'saving' && (
          <div className="settings-description">Saving…</div>
        )}
        {status.kind === 'saved' && (
          <div className="settings-description">Saved. Server is using the new folder.</div>
        )}
        {status.kind === 'unauthorized' && (
          <div className="settings-description">
            The server rejected the request. Editing server configuration requires the
            admin API key (<code>LEMONADE_ADMIN_API_KEY</code>). Set it on the server and
            in Connection → API Key, then reload.
          </div>
        )}
        {status.kind === 'error' && (
          <div className="settings-description">Could not read or update the setting: {status.message}</div>
        )}
      </div>

      {/* <div className="settings-section settings-section-default">
        <div className="settings-label-row">
          <label className="settings-label">
            <span className="settings-label-text">Extra models folder (read-only)</span>
            <span className="settings-description">
              Additional path the server searches recursively for GGUF files. Configure
              with <code>lemonade config set extra_models_dir=...</code>.
            </span>
          </label>
        </div>
        <input
          type="text"
          value={extraModelsDir || '(not set)'}
          readOnly
          className="settings-text-input"
          title="Extra models folder"
          placeholder="(not set)"
        />
      </div> */}
    </div>
  );
};

export default ServerSettings;
