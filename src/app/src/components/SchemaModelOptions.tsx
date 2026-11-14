import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { api, friendlyErrorMessage, type ConnectionSnapshot, type LoadedModel } from '../api';
import { BackendRecipe, isRecord, own } from '../features/backends/backendSchema';
import { refreshBackendCatalog } from '../features/backends/backendCatalog';
import {
  RecipeForm,
  createForm,
  draftPayload,
  formErrors,
  mutationPayload,
  parseModelOptions,
  reconcileForm,
} from '../features/backends/recipeForm';
import { useI18n } from '../i18n';
import { SchemaRecipeOptions } from './SchemaRecipeOptions';

interface Props {
  modelName: string;
  recipe: BackendRecipe;
  catalogId: string;
  connection: ConnectionSnapshot;
  systemInfo: Record<string, unknown> | null;
  loadedModel: LoadedModel | null;
  isLoadingThis?: boolean;
  onReloadModel?: (model: LoadedModel, recipeOptions?: Record<string, unknown>) => Promise<void>;
  onDirtyChange?: (dirty: boolean) => void;
  loadOptionsRef?: React.MutableRefObject<(() => Record<string, unknown>) | null>;
}

type BackendStatus = { state?: string; message?: string };

function recipeBackends(systemInfo: Record<string, unknown> | null, recipe: string): Record<string, BackendStatus> {
  const recipes = isRecord(systemInfo?.recipes) ? systemInfo.recipes : undefined;
  const row = own(recipes, recipe);
  return isRecord(row) && isRecord(row.backends) ? row.backends as Record<string, BackendStatus> : {};
}

async function fetchSnapshot(connection: ConnectionSnapshot, recipe: BackendRecipe, modelName: string, signal?: AbortSignal) {
  const response = await api.fetchForConnection(connection, `/models/${encodeURIComponent(modelName)}/options`, { signal });
  if (!response.ok) throw new Error(`Cannot read saved model options (${response.status}).`);
  const snapshot = parseModelOptions(await response.json(), recipe.recipe, modelName);
  api.assertCurrentConnection(connection);
  return snapshot;
}

export const SchemaModelOptions: React.FC<Props> = ({
  modelName, recipe, catalogId, connection, systemInfo, loadedModel, isLoadingThis, onReloadModel, onDirtyChange, loadOptionsRef,
}) => {
  const { t } = useI18n();
  const [form, setForm] = useState<RecipeForm>();
  const [message, setMessage] = useState<string>();
  const [busy, setBusy] = useState(false);
  const recipeRef = useRef(recipe);
  recipeRef.current = recipe;
  const catalogIdRef = useRef(catalogId);
  catalogIdRef.current = catalogId;

  const statuses = useMemo(() => recipeBackends(systemInfo, recipe.recipe), [systemInfo, recipe.recipe]);
  const backends = useMemo(() => Object.entries(statuses)
    .filter(([, info]) => info.state && info.state !== 'unsupported').map(([name]) => name), [statuses]);
  const availability = Object.values(statuses)
    .filter(info => info.state !== 'installed').map(info => info.message).filter(Boolean).join(' ');

  useEffect(() => {
    const controller = new AbortController();
    setForm(undefined);
    setMessage(undefined);
    void fetchSnapshot(connection, recipeRef.current, modelName, controller.signal)
      .then(snapshot => {
        if (!controller.signal.aborted) {
          setForm(createForm(recipeRef.current, snapshot, catalogIdRef.current, connection.generation));
        }
      })
      .catch(error => { if (!controller.signal.aborted) setMessage(friendlyErrorMessage(error)); });
    return () => controller.abort();
  }, [modelName, connection.generation, recipe.recipe]);

  useEffect(() => {
    if (!form || form.catalogId === catalogId) return;
    let active = true;
    void fetchSnapshot(connection, recipe, modelName)
      .then(snapshot => { if (active) setForm(previous => previous && reconcileForm(previous, recipe, snapshot, catalogId)); })
      .catch(error => { if (active) setMessage(friendlyErrorMessage(error)); });
    return () => { active = false; };
  }, [catalogId, form?.catalogId]);

  const dirty = Boolean(form && form.edits.size > 0);
  useEffect(() => { onDirtyChange?.(dirty); }, [dirty, onDirtyChange]);

  if (loadOptionsRef) loadOptionsRef.current = form ? () => ({ ...draftPayload(form) }) : null;
  useEffect(() => () => {
    if (loadOptionsRef) loadOptionsRef.current = null;
  }, [loadOptionsRef]);

  const submit = useCallback(async (action: 'save' | 'validate') => {
    if (!form) return;
    setBusy(true);
    setMessage(undefined);
    try {
      api.assertCurrentConnection(connection);
      if (form.connectionGeneration !== connection.generation) throw new Error('Server connection changed; reopen its options.');
      const result = await refreshBackendCatalog();
      if (result.status !== 'ready') throw new Error(result.message);
      const row = result.catalog.recipes.find(item => item.recipe === form.recipe.recipe);
      if (!row || row.source !== 'external' || row.incompatibility) {
        throw new Error(row?.incompatibility ?? 'Unknown recipe on the selected server.');
      }
      if (result.catalog.catalog_id !== form.catalogId) {
        const snapshot = await fetchSnapshot(connection, row, modelName);
        setForm(reconcileForm(form, row, snapshot, result.catalog.catalog_id));
        return;
      }
      const payload = mutationPayload(form, backends);
      const response = await api.saveModelOptions(modelName, payload, { dryRun: action === 'validate' });
      const snapshot = parseModelOptions(response, row.recipe, modelName);
      api.assertCurrentConnection(connection);
      if (action === 'save') {
        setForm(createForm(row, snapshot, form.catalogId, connection.generation));
        setMessage(t('Options saved.'));
      } else {
        setMessage(t('Options are valid; nothing was saved.'));
      }
    } catch (error) {
      setMessage(friendlyErrorMessage(error));
    } finally {
      setBusy(false);
    }
  }, [backends, connection, form, modelName, t]);

  const reload = async () => {
    if (!form || !loadedModel || !onReloadModel) return;
    setBusy(true);
    try {
      await onReloadModel(loadedModel, { ...draftPayload(form) });
      setMessage(t('Model reloaded with the settings shown here.'));
    } catch (error) {
      setMessage(friendlyErrorMessage(error));
    } finally {
      setBusy(false);
    }
  };

  const blocked = busy || !form || !!recipe.incompatibility || form.catalogId !== catalogId
    || formErrors(form, backends).size > 0;

  return (
    <>
      {!form && !message && <p className="schema-recipe-options__notice" role="status">{t('Loading backend schema…')}</p>}
      {form && (
        <SchemaRecipeOptions form={form} backends={backends} availability={availability} disabled={busy} onChange={setForm} />
      )}
      <div className="detail-tuning__actions detail-configuration__actions">
        {loadedModel && onReloadModel && (
          <button type="button" className="btn btn--primary btn--sm" onClick={reload} disabled={blocked || isLoadingThis}>
            {t('Reload model')}
          </button>
        )}
        <button
          type="button"
          className={`btn ${dirty ? 'btn--primary' : 'btn--ghost'} btn--sm`}
          onClick={() => void submit('save')}
          disabled={blocked}
        >
          {t('Save')}
        </button>
        <button type="button" className="btn btn--ghost btn--sm" onClick={() => void submit('validate')} disabled={blocked}>
          {t('Validate')}
        </button>
        {dirty && form && (
          <button
            type="button"
            className="btn btn--ghost btn--sm"
            disabled={busy}
            onClick={() => setForm({ ...form, edits: new Map() })}
          >
            {t('Discard changes')}
          </button>
        )}
      </div>
      {message && <p className="detail-tuning__notice" role="status">{message}</p>}
    </>
  );
};
