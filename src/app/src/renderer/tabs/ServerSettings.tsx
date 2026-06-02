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
  | { kind: 'forbidden_remote' }
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
  // Surface dialog/plugin failures to the parent so they appear in the shared
  // status row instead of becoming unhandled promise rejections.
  onError: (message: string) => void;
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
  onError,
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
    try {
      const selected = await open({
        directory: true,
        multiple: false,
        defaultPath,
        title: dialogTitle,
      });
      // A null return means the user cancelled the dialog — not an error.
      if (typeof selected === 'string' && selected.length > 0) {
        setDraft(selected);
        await onApply(selected);
      }
    } catch (error) {
      onError(error instanceof Error ? error.message : String(error));
    }
  };

  const isDirty = draft.trim() !== serverValue;
  const atDefault = isDefault(serverValue);

  return (
    <div className={`settings-section settings-directory-field ${atDefault ? 'settings-section-default' : ''}`}>
      <div className="settings-label-row">
        <span className="settings-label-text">{label}</span>
        <button
          type="button"
          className="settings-field-reset"
          onClick={() => { void onReset(); }}
          disabled={disabled || atDefault}
        >
          Reset
        </button>
      </div>
      <div className="settings-description">{description}</div>
      <input
        type="text"
        value={draft}
        onChange={(e) => setDraft(e.target.value)}
        className="settings-text-input"
        placeholder={placeholder}
        title={label}
        disabled={disabled}
      />
      <div className="settings-path-actions">
        <button
          type="button"
          className="settings-field-reset"
          onClick={() => { void handleBrowse(); }}
          disabled={disabled}
        >
          Browse
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
    } else if (result.kind === 'forbidden_remote') {
      setStatus({ kind: 'forbidden_remote' });
    } else {
      setStatus({ kind: 'error', message: result.message });
    }
  }, []);

  useEffect(() => {
    void loadConfig();
  }, [loadConfig]);

  const finishApply = async (result: RuntimeConfigUpdateResult) => {
    if (result.kind !== 'ok') {
      if (result.kind === 'unauthorized') {
        setStatus({ kind: 'unauthorized' });
      } else if (result.kind === 'forbidden_remote') {
        setStatus({ kind: 'forbidden_remote' });
      } else {
        setStatus({ kind: 'error', message: result.message });
      }
      return;
    }
    // Write succeeded — re-read BEFORE claiming "Saved" so we never show
    // "Saved" alongside stale values if the refresh itself fails.
    const fresh = await getRuntimeConfig();
    if (fresh.kind === 'ok') {
      setServerModelsDir(fresh.modelsDir);
      setServerExtraModelsDir(fresh.extraModelsDir);
      setStatus({ kind: 'saved' });
    } else if (fresh.kind === 'unauthorized') {
      // Write went through but we lost read access on the follow-up; surface
      // that explicitly rather than implying the displayed values are current.
      setStatus({
        kind: 'error',
        message:
          'Saved, but could not re-read the configuration (admin authorization required). Displayed values may be stale.',
      });
    } else if (fresh.kind === 'forbidden_remote') {
      setStatus({
        kind: 'error',
        message:
          'Saved, but could not re-read the configuration (server restricts /internal/* to local clients). Displayed values may be stale.',
      });
    } else {
      setStatus({
        kind: 'error',
        message: `Saved, but could not re-read the configuration: ${fresh.message}. Displayed values may be stale.`,
      });
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
  const isForbiddenRemote = status.kind === 'forbidden_remote';
  // We keep the inputs visible in every state so the user can see what is
  // configured (e.g. on a remote server they can still read the values they
  // typed), but we disable editing whenever the server will reject the write.
  const fieldsDisabled = isBusy || isUnauthorized || isForbiddenRemote;

  const handleFieldError = (message: string) => {
    setStatus({ kind: 'error', message });
  };

  return (
    <div className="settings-section-container">
      <DirectoryField
        label="Model Download Folder"
        description={
          <>
            Where the server downloads and stores models pulled from Hugging Face. 
          </>
        }
        placeholder="auto"
        dialogTitle="Choose model download folder"
        serverValue={serverModelsDir}
        isDefault={isModelsDirAuto}
        disabled={fieldsDisabled}
        onApply={applyModelsDir}
        onReset={() => applyModelsDir(MODELS_DIR_AUTO_SENTINEL)}
        onError={handleFieldError}
      />

      <DirectoryField
        label="Extra Models Folder"
        description={
          <>
            An additional folder the server recursively scans for loose <code>.gguf</code>{' '}
            files.
          </>
        }
        placeholder="(none — feature disabled)"
        dialogTitle="Choose extra models folder"
        serverValue={serverExtraModelsDir}
        isDefault={(value) => value.trim() === ''}
        disabled={fieldsDisabled}
        onApply={applyExtraModelsDir}
        onReset={() => applyExtraModelsDir('')}
        onError={handleFieldError}
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
          The server rejected the request with HTTP 401. Editing server configuration
          requires the admin API key (<code>LEMONADE_ADMIN_API_KEY</code>). Set it on
          the server and in Connection → API Key, then reload.
        </div>
      )}
      {status.kind === 'forbidden_remote' && (
        <div className="settings-description">
          This server restricts <code>/internal/*</code> endpoints to local clients
          (HTTP 403), so server-wide configuration cannot be edited from a remote
          desktop app. To change <code>models_dir</code> or <code>extra_models_dir</code>,
          run the desktop app (or <code>lemonade config set</code>) on the same machine
          as <code>lemond</code>. Setting <code>LEMONADE_ADMIN_API_KEY</code> will not
          unlock this from a remote client.
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
