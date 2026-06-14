import React, { useCallback, useEffect, useRef, useState } from 'react';
import api, { friendlyErrorMessage } from '../api';

// `models_dir` uses the sentinel "auto" to mean "follow the Hugging Face cache"
// (HF_HOME / HF_HUB_CACHE, defaulting to ~/.cache/huggingface/hub). `extra_models_dir`
// uses the empty string to mean "feature disabled". Both are server-wide config
// written through POST /internal/set — they affect every client of this server.
const MODELS_DIR_AUTO = 'auto';

type Status =
  | { kind: 'loading' }
  | { kind: 'ready' }
  | { kind: 'saving' }
  | { kind: 'saved' }
  | { kind: 'unauthorized' }
  | { kind: 'error'; message: string };

// /internal/* may require LEMONADE_ADMIN_API_KEY rather than the regular key, so a
// 401/403 is surfaced distinctly to guide the user instead of showing a raw error.
function isUnauthorized(err: unknown): boolean {
  const status = (err as { status?: number })?.status;
  return status === 401 || status === 403;
}

interface DirectoryFieldProps {
  label: string;
  hint: React.ReactNode;
  placeholder: string;
  // Current server-persisted value. The field re-syncs its draft from this whenever
  // the parent reloads, so the input always reflects what the server actually stored.
  value: string;
  atDefault: boolean;
  disabled: boolean;
  onApply: (value: string) => void;
  onReset: () => void;
}

const DirectoryField: React.FC<DirectoryFieldProps> = ({
  label, hint, placeholder, value, atDefault, disabled, onApply, onReset,
}) => {
  const [draft, setDraft] = useState(value);
  useEffect(() => { setDraft(value); }, [value]);

  const dirty = draft.trim() !== value;

  return (
    <div className="form-field server-settings__field">
      <div className="server-settings__label-row">
        <label className="form-field__label">{label}</label>
        <button
          type="button"
          className="btn btn--ghost"
          onClick={onReset}
          disabled={disabled || atDefault}
        >
          Reset
        </button>
      </div>
      <div className="server-settings__row">
        <input
          type="text"
          value={draft}
          placeholder={placeholder}
          disabled={disabled}
          onChange={e => setDraft(e.target.value)}
          onKeyDown={e => { if (e.key === 'Enter' && dirty) onApply(draft.trim()); }}
        />
        <button
          type="button"
          className="btn btn--primary"
          onClick={() => onApply(draft.trim())}
          disabled={disabled || !dirty}
        >
          Apply
        </button>
      </div>
      <span className="form-field__hint">{hint}</span>
    </div>
  );
};

// Server-wide folder configuration, shown under Connect once a server is reachable.
// Unlike per-client preferences (theme, presets, history), these values live in
// lemond's config.json and are read/written over HTTP — they are NOT client state.
const ServerSettings: React.FC = () => {
  const [modelsDir, setModelsDir] = useState(MODELS_DIR_AUTO);
  const [extraModelsDir, setExtraModelsDir] = useState('');
  const [status, setStatus] = useState<Status>({ kind: 'loading' });

  // Guard async setState against an unmount mid-request (e.g. user navigates away
  // or disconnects while a load/save is in flight).
  const mounted = useRef(true);
  useEffect(() => () => { mounted.current = false; }, []);

  const load = useCallback(async () => {
    setStatus({ kind: 'loading' });
    try {
      const cfg = await api.getRuntimeConfig();
      if (!mounted.current) return;
      setModelsDir(
        typeof cfg.models_dir === 'string' && cfg.models_dir ? cfg.models_dir : MODELS_DIR_AUTO,
      );
      setExtraModelsDir(typeof cfg.extra_models_dir === 'string' ? cfg.extra_models_dir : '');
      setStatus({ kind: 'ready' });
    } catch (err) {
      if (!mounted.current) return;
      setStatus(
        isUnauthorized(err)
          ? { kind: 'unauthorized' }
          : { kind: 'error', message: friendlyErrorMessage(err) },
      );
    }
  }, []);

  useEffect(() => { void load(); }, [load]);

  const apply = useCallback(async (updates: Record<string, unknown>) => {
    setStatus({ kind: 'saving' });
    try {
      await api.setRuntimeConfig(updates);
      if (!mounted.current) return;
      // Re-read so we display whatever the server actually persisted (it may
      // normalize the path), then flag success.
      await load();
      if (mounted.current) setStatus({ kind: 'saved' });
    } catch (err) {
      if (!mounted.current) return;
      setStatus(
        isUnauthorized(err)
          ? { kind: 'unauthorized' }
          : { kind: 'error', message: friendlyErrorMessage(err) },
      );
    }
  }, [load]);

  const busy = status.kind === 'loading' || status.kind === 'saving';

  return (
    <section className="server-settings">
      <h2 className="server-settings__title">Server configuration</h2>
      <p className="server-settings__intro">
        Where this lemond server stores and discovers models. These are server-wide
        settings — every client connected to this server is affected.
      </p>

      <DirectoryField
        label="Model download folder"
        placeholder="auto"
        value={modelsDir}
        atDefault={modelsDir.trim().toLowerCase() === MODELS_DIR_AUTO}
        disabled={busy}
        onApply={value => apply({ models_dir: value === '' ? MODELS_DIR_AUTO : value })}
        onReset={() => apply({ models_dir: MODELS_DIR_AUTO })}
        hint={
          <>
            Where the server downloads and stores models pulled from Hugging Face. Use{' '}
            <code>auto</code> to follow the Hugging Face cache
            (<code>~/.cache/huggingface/hub</code>). Loose GGUF files placed here are{' '}
            <em>not</em> detected — use the Extra models folder for that.
          </>
        }
      />

      <DirectoryField
        label="Extra models folder"
        placeholder="(none — feature disabled)"
        value={extraModelsDir}
        atDefault={extraModelsDir.trim() === ''}
        disabled={busy}
        onApply={value => apply({ extra_models_dir: value })}
        onReset={() => apply({ extra_models_dir: '' })}
        hint={
          <>
            An additional folder the server scans recursively for loose <code>.gguf</code>{' '}
            files. Imported models appear in the model list with the <code>extra.</code>{' '}
            prefix. Leave empty to disable.
          </>
        }
      />

      {status.kind === 'loading' && (
        <div className="server-settings__status">Loading current values…</div>
      )}
      {status.kind === 'saving' && (
        <div className="server-settings__status">Saving…</div>
      )}
      {status.kind === 'saved' && (
        <div className="server-settings__status">Saved. The server is using the new value.</div>
      )}
      {status.kind === 'unauthorized' && (
        <div className="server-settings__status server-settings__status--error">
          The server rejected the request. Editing server configuration requires the admin
          API key (<code>LEMONADE_ADMIN_API_KEY</code>). Set it on the server and in the API
          Key field above, then reconnect.
        </div>
      )}
      {status.kind === 'error' && (
        <div className="server-settings__status server-settings__status--error">
          Could not read or update the setting: {status.message}
        </div>
      )}
    </section>
  );
};

export default ServerSettings;
