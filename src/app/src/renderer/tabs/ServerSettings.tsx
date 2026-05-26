import React, { ReactNode, useCallback, useEffect, useState } from 'react';
import { open } from '@tauri-apps/plugin-dialog';
import {
  RuntimeConfigUpdateResult,
  clearExtraModelsDir,
  getRuntimeConfig,
  isModelsDirAuto,
  MODELS_DIR_AUTO_SENTINEL,
  resetModelsDir,
  setExtraModelsDir,
  setModelsDir,
} from '../utils/serverRuntimeConfig';

type Status =
  | { kind: 'loading' }
  | { kind: 'ready' }
  | { kind: 'saving' }
  | { kind: 'unauthorized' }
  | { kind: 'error'; message: string }
  | { kind: 'saved' };

interface DirectoryFieldProps {
  label: string;
  description: ReactNode;
  placeholder: string;
  dialogTitle: string;
  // Current server-persisted value. The field considers itself "at default"
  // — and disables Reset — when `isDefault(serverValue)` returns true.
  serverValue: string;
  isDefault: (value: string) => boolean;
  // Disable browse/inputs when the parent is busy or auth has failed.
  disabled: boolean;
  // Apply a user-typed or user-picked value. The parent handles status updates
  // and re-fetches config afterward.
  onApply: (value: string) => Promise<void>;
  // Reset to the field's default (which differs between models_dir and
  // extra_models_dir, so it stays a callback rather than a sentinel here).
  onReset: () => Promise<void>;
}

const DirectoryField: React.FC<DirectoryFieldProps> = ({
  label,
  description,
  placeholder,
  dialogTitle,
  serverValue,
  isDefault,
  disabled,
  onApply,
  onReset,
}) => {
  // Local draft tracks what the user has typed but not yet applied. We re-sync
  // it from `serverValue` whenever the parent reloads, so the input always
  // reflects the server after a save.
  const [draft, setDraft] = useState(serverValue);
  useEffect(() => {
    setDraft(serverValue);
  }, [serverValue]);

  const handleBrowse = async () => {
    // Seed the picker with the current path when it's a real directory; for
    // the default sentinel (or empty), let the OS pick the start location.
    const defaultPath = isDefault(draft) ? undefined : draft || undefined;
    const selected = await open({
      directory: true,
      multiple: false,
      defaultPath,
      title: dialogTitle,
    });
    if (typeof selected === 'string' && selected.length > 0) {
      setDraft(selected);
      await onApply(selected);
    }
  };

  const isDirty = draft.trim() !== serverValue;
  const atDefault = isDefault(serverValue);

  return (
    <div className={`settings-section ${atDefault ? 'settings-section-default' : ''}`}>
      <div className="settings-label-row">
        <label className="settings-label">
          <span className="settings-label-text">{label}</span>
          <span className="settings-description">{description}</span>
        </label>
        <button
          type="button"
          className="settings-field-reset"
          onClick={() => { void onReset(); }}
          disabled={disabled || atDefault}
        >
          Reset
        </button>
      </div>
      <div className="settings-path-row">
        <input
          type="text"
          value={draft}
          onChange={(e) => setDraft(e.target.value)}
          className="settings-text-input"
          placeholder={placeholder}
          title={label}
          disabled={disabled}
        />
        <button
          type="button"
          className="settings-field-reset"
          onClick={() => { void handleBrowse(); }}
          disabled={disabled}
        >
          Browse…
        </button>
        <button
          type="button"
          className="settings-field-reset"
          onClick={() => { void onApply(draft.trim()); }}
          disabled={disabled || !isDirty}
        >
          Apply
        </button>
      </div>
    </div>
  );
};

const ServerSettings: React.FC = () => {
  // Server-wide config does NOT live in AppSettings. We mirror the last-known
  // server values here and rewrite them via the helpers in serverRuntimeConfig.
  const [serverModelsDir, setServerModelsDir] = useState<string>(MODELS_DIR_AUTO_SENTINEL);
  const [serverExtraModelsDir, setServerExtraModelsDir] = useState<string>('');
  const [status, setStatus] = useState<Status>({ kind: 'loading' });

  const loadConfig = useCallback(async () => {
    setStatus({ kind: 'loading' });
    const result = await getRuntimeConfig();
    if (result.kind === 'ok') {
      setServerModelsDir(result.modelsDir);
      setServerExtraModelsDir(result.extraModelsDir);
      setStatus({ kind: 'ready' });
    } else if (result.kind === 'unauthorized') {
      setStatus({ kind: 'unauthorized' });
    } else {
      setStatus({ kind: 'error', message: result.message });
    }
  }, []);

  useEffect(() => {
    void loadConfig();
  }, [loadConfig]);

  const finishApply = async (result: RuntimeConfigUpdateResult) => {
    if (result.kind === 'ok') {
      setStatus({ kind: 'saved' });
      // Re-read so we display whatever the server actually persisted.
      const fresh = await getRuntimeConfig();
      if (fresh.kind === 'ok') {
        setServerModelsDir(fresh.modelsDir);
        setServerExtraModelsDir(fresh.extraModelsDir);
      }
    } else if (result.kind === 'unauthorized') {
      setStatus({ kind: 'unauthorized' });
    } else {
      setStatus({ kind: 'error', message: result.message });
    }
  };

  const applyModelsDir = async (value: string) => {
    setStatus({ kind: 'saving' });
    const result = value === MODELS_DIR_AUTO_SENTINEL || isModelsDirAuto(value)
      ? await resetModelsDir()
      : await setModelsDir(value);
    await finishApply(result);
  };

  const applyExtraModelsDir = async (value: string) => {
    setStatus({ kind: 'saving' });
    const result = value === ''
      ? await clearExtraModelsDir()
      : await setExtraModelsDir(value);
    await finishApply(result);
  };

  const isBusy = status.kind === 'loading' || status.kind === 'saving';
  const isUnauthorized = status.kind === 'unauthorized';
  const fieldsDisabled = isBusy || isUnauthorized;

  return (
    <div className="settings-section-container">
      <DirectoryField
        label="Model download folder"
        description={
          <>
            Where the server downloads and stores models pulled from Hugging Face. Use{' '}
            <code>auto</code> to follow the Hugging Face cache
            (<code>~/.cache/huggingface/hub</code>). This is a server-wide setting — changes
            apply to every client connected to this server. Loose GGUF files placed in this
            folder are <em>not</em> detected; for that, use the Extra models folder below.
          </>
        }
        placeholder="auto"
        dialogTitle="Choose model download folder"
        serverValue={serverModelsDir}
        isDefault={isModelsDirAuto}
        disabled={fieldsDisabled}
        onApply={applyModelsDir}
        onReset={() => applyModelsDir(MODELS_DIR_AUTO_SENTINEL)}
      />

      <DirectoryField
        label="Extra models folder"
        description={
          <>
            An additional folder the server recursively scans for loose <code>.gguf</code>{' '}
            files. Imported models appear in the model list with the <code>extra.</code>{' '}
            prefix. Leave empty to disable. Tip: after changing this, refresh Model Manager
            to see imported models.
          </>
        }
        placeholder="(none — feature disabled)"
        dialogTitle="Choose extra models folder"
        serverValue={serverExtraModelsDir}
        isDefault={(value) => value.trim() === ''}
        disabled={fieldsDisabled}
        onApply={applyExtraModelsDir}
        onReset={() => applyExtraModelsDir('')}
      />

      {status.kind === 'loading' && (
        <div className="settings-description">Loading current values…</div>
      )}
      {status.kind === 'saving' && (
        <div className="settings-description">Saving…</div>
      )}
      {status.kind === 'saved' && (
        <div className="settings-description">Saved. Server is using the new value.</div>
      )}
      {status.kind === 'unauthorized' && (
        <div className="settings-description">
          The server rejected the request. Editing server configuration requires the
          admin API key (<code>LEMONADE_ADMIN_API_KEY</code>). Set it on the server and
          in Connection → API Key, then reload.
        </div>
      )}
      {status.kind === 'error' && (
        <div className="settings-description">
          Could not read or update the setting: {status.message}
        </div>
      )}
    </div>
  );
};

export default ServerSettings;
