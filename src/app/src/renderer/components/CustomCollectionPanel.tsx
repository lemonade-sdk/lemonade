import React, { useEffect, useMemo, useState } from 'react';
import { useModels } from '../hooks/useModels';
import { getModelDisplayName } from '../utils/modelDisplayName';
import type { ModelInfo } from '../utils/modelData';
import {
  CustomCollection,
  CustomCollectionDraft,
  CustomCollectionRole,
  getCollectionRoleOptions,
  getCustomCollectionRoleLabel,
  getCustomCollectionComponentList,
  getCollectionDisplayName,
  isCustomCollectionModel,
  modelEntryToCustomCollection,
} from '../utils/customCollections';

interface CustomCollectionPanelProps {
  mode: 'create' | 'edit';
  collectionId?: string;
  onClose: () => void;
  onSave: (collection: CustomCollectionDraft) => void | Promise<void>;
  onExport: (collection: CustomCollectionDraft) => void;
}

const DEFAULT_NAME = 'MyOmniModel';

const OPTIONAL_ROLES: CustomCollectionRole[] = ['vision', 'image', 'edit', 'transcription', 'speech'];

const roleDescriptions: Record<CustomCollectionRole, string> = {
  llm: 'Required planner model for chat and tool calls.',
  vision: 'Optional model used for image analysis.',
  image: 'Optional model used for image generation.',
  edit: 'Optional model used for image editing.',
  transcription: 'Optional model used for speech-to-text.',
  speech: 'Optional model used for text-to-speech.',
};

const emptyDraft = () => ({
  selectedCollectionId: '',
  sourceCollectionId: '',
  name: DEFAULT_NAME,
  llm: '',
  vision: '',
  image: '',
  edit: '',
  transcription: '',
  speech: '',
  createdAt: undefined as string | undefined,
});

type CollectionForm = ReturnType<typeof emptyDraft>;

const draftFromCollection = (collection: CustomCollection, sourceId: string, isCustom: boolean): CollectionForm => {
  return {
    selectedCollectionId: isCustom ? collection.id : '',
    sourceCollectionId: sourceId,
    name: isCustom ? collection.name : `${getCollectionDisplayName(sourceId)} Custom`,
    llm: collection.components.llm,
    vision: collection.components.vision ?? '',
    image: collection.components.image ?? '',
    edit: collection.components.edit ?? '',
    transcription: collection.components.transcription ?? '',
    speech: collection.components.speech ?? '',
    createdAt: collection.createdAt,
  };
};

const formToDraft = (form: CollectionForm): CustomCollectionDraft => ({
  id: form.selectedCollectionId || undefined,
  name: form.name.trim(),
  createdAt: form.createdAt,
  components: {
    llm: form.llm,
    vision: form.vision || undefined,
    image: form.image || undefined,
    edit: form.edit || undefined,
    transcription: form.transcription || undefined,
    speech: form.speech || undefined,
  },
});

const componentListForForm = (form: CollectionForm): string[] => getCustomCollectionComponentList(formToDraft(form));

const CustomCollectionPanel: React.FC<CustomCollectionPanelProps> = ({
  mode,
  collectionId,
  onClose,
  onSave,
  onExport,
}) => {
  const { modelsData } = useModels();
  const [form, setForm] = useState<CollectionForm>(() => emptyDraft());
  const [originalComponents, setOriginalComponents] = useState<string[]>([]);
  const [error, setError] = useState<string | null>(null);

  const options = useMemo(() => ({
    llm: getCollectionRoleOptions(modelsData, 'llm'),
    vision: getCollectionRoleOptions(modelsData, 'vision'),
    image: getCollectionRoleOptions(modelsData, 'image'),
    edit: getCollectionRoleOptions(modelsData, 'edit'),
    transcription: getCollectionRoleOptions(modelsData, 'transcription'),
    speech: getCollectionRoleOptions(modelsData, 'speech'),
  }), [modelsData]);

  const modelHasLabel = (modelId: string, label: string): boolean => {
    return (modelsData[modelId]?.labels ?? []).includes(label);
  };

  const plannerProvidesVision = form.llm.length > 0 && modelHasLabel(form.llm, 'vision');
  const imageProvidesEdit = form.image.length > 0 && modelHasLabel(form.image, 'edit');
  const selectedLlmInfo = form.llm ? modelsData[form.llm] : undefined;
  const selectedLlmLabels = selectedLlmInfo?.labels ?? [];
  const selectedLlmHasToolCalling = selectedLlmLabels.includes('tool-calling') || selectedLlmLabels.includes('tools');
  const currentComponents = componentListForForm(form);
  const hasExistingCustomCollection = mode === 'edit' && !!form.selectedCollectionId;
  const isTemplateEdit = mode === 'edit' && !!form.sourceCollectionId && !form.selectedCollectionId;
  const hasComponentChanges = originalComponents.length > 0 && (
    originalComponents.length !== currentComponents.length ||
    originalComponents.some((component, index) => component !== currentComponents[index])
  );

  useEffect(() => {
    setError(null);
    if (mode !== 'edit' || !collectionId) {
      const next = emptyDraft();
      setForm(next);
      setOriginalComponents([]);
      return;
    }

    const collection = modelEntryToCustomCollection(collectionId, modelsData[collectionId], modelsData);
    if (!collection) {
      setForm(emptyDraft());
      setOriginalComponents([]);
      setError('This Omni Model could not be found. Refresh models and try again.');
      return;
    }

    const next = draftFromCollection(collection, collectionId, isCustomCollectionModel(collectionId, modelsData[collectionId]));
    if (!next.vision && next.llm && (modelsData[next.llm]?.labels ?? []).includes('vision')) {
      next.vision = next.llm;
    }
    if (!next.edit && next.image && (modelsData[next.image]?.labels ?? []).includes('edit')) {
      next.edit = next.image;
    }
    setForm(next);
    setOriginalComponents(getCustomCollectionComponentList(collection));
  }, [mode, collectionId, modelsData]);

  const updateForm = (patch: Partial<CollectionForm>) => {
    setForm((prev) => ({ ...prev, ...patch }));
    setError(null);
  };

  const setRole = (role: CustomCollectionRole, value: string) => {
    setForm((prev) => {
      const patch: Partial<CollectionForm> = { [role]: value } as Partial<CollectionForm>;

      if (role === 'llm') {
        const selectedPlannerHasVision = value.length > 0 && modelHasLabel(value, 'vision');
        const visionWasEmptyOrPlannerDefault = !prev.vision || prev.vision === prev.llm;

        if (selectedPlannerHasVision && visionWasEmptyOrPlannerDefault) {
          patch.vision = value;
        } else if (!selectedPlannerHasVision && prev.vision === prev.llm) {
          patch.vision = '';
        }
      }

      if (role === 'image') {
        const selectedImageHasEdit = value.length > 0 && modelHasLabel(value, 'edit');
        const editWasEmptyOrImageDefault = !prev.edit || prev.edit === prev.image;

        if (selectedImageHasEdit && editWasEmptyOrImageDefault) {
          patch.edit = value;
        } else if (!selectedImageHasEdit && prev.edit === prev.image) {
          patch.edit = '';
        }
      }

      return { ...prev, ...patch };
    });
    setError(null);
  };

  const valueForRole = (role: CustomCollectionRole): string => form[role];

  const displayNameForOption = (modelId: string): string => {
    const info = modelsData[modelId];
    const name = info?.model_name ?? getModelDisplayName(modelId);
    return `${name} (${info?.downloaded === true ? 'downloaded' : 'registered - will download'})`;
  };

  const renderOptionsGroup = (roleOptions: Array<{ id: string; info: ModelInfo }>, downloaded: boolean) => {
    const filtered = roleOptions.filter((model) => (model.info.downloaded === true) === downloaded);
    if (filtered.length === 0) return null;
    return (
      <optgroup label={downloaded ? 'Available locally' : 'Registered - will download when pulled'}>
        {filtered.map((model) => (
          <option key={model.id} value={model.id}>{displayNameForOption(model.id)}</option>
        ))}
      </optgroup>
    );
  };

  const renderRoleSelect = (role: CustomCollectionRole, required = false) => {
    const selectedValue = valueForRole(role);
    const roleOptions = options[role];
    const optionIds = new Set(roleOptions.map((model) => model.id));
    const selectedFallback = selectedValue && !optionIds.has(selectedValue) && modelsData[selectedValue]
      ? [{ id: selectedValue, info: modelsData[selectedValue] }]
      : [];
    const autoProviderModel = role === 'vision' ? form.llm : role === 'edit' ? form.image : '';
    const autoProvided = (role === 'vision' && plannerProvidesVision) || (role === 'edit' && imageProvidesEdit);
    const isUsingAutoProvider = autoProvided && selectedValue === autoProviderModel;
    const autoProviderOption = autoProvided && autoProviderModel && modelsData[autoProviderModel]
      ? { id: autoProviderModel, info: modelsData[autoProviderModel] }
      : null;

    const seenOptions = new Set<string>();
    const selectOptions = selectedFallback.concat(roleOptions).filter((model) => {
      if (model.id === autoProviderModel) return false;
      if (seenOptions.has(model.id)) return false;
      seenOptions.add(model.id);
      return true;
    });

    return (
      <div className="form-section collection-role-row" key={role}>
        <label className="form-label" title={roleDescriptions[role]}>
          {getCustomCollectionRoleLabel(role)}{required ? ' *' : ''}
        </label>
        <select
          className={`form-input form-select collection-model-select${isUsingAutoProvider ? ' auto-provided' : ''}`}
          value={selectedValue}
          onChange={(e) => setRole(role, e.target.value)}
          title={autoProvided ? 'Automatically provided by the selected model. Select another compatible model to override.' : roleDescriptions[role]}
        >
          {autoProviderOption && (
            <option className="collection-auto-provided-option" value={autoProviderOption.id}>
              {displayNameForOption(autoProviderOption.id)} (automatic)
            </option>
          )}
          {!required && !autoProvided && <option value="">None</option>}
          {required && <option value="">Select a model...</option>}
          {renderOptionsGroup(selectOptions, true)}
          {renderOptionsGroup(selectOptions, false)}
        </select>
        {roleOptions.length === 0 && !autoProvided && (
          <div className="collection-role-empty">No registered compatible model found. Add a custom model first so its checkpoint can be pulled as a component.</div>
        )}
      </div>
    );
  };

  const validateDraft = (): CustomCollectionDraft | null => {
    const cleanName = form.name.trim();
    if (!cleanName) {
      setError('Omni Model name is required.');
      return null;
    }
    if (!form.llm) {
      setError('Select a planner LLM for the Omni Model.');
      return null;
    }
    return formToDraft({ ...form, name: cleanName });
  };

  const handleSave = async () => {
    const draft = validateDraft();
    if (!draft) return;
    void onSave(draft);
  };

  const handleExport = () => {
    const draft = validateDraft();
    if (!draft) return;
    onExport(draft);
  };

  return (
    <>
      <div className="settings-header">
        <h3>{hasExistingCustomCollection ? 'Omni Model Options' : 'New Omni Model'}</h3>
        <button type="button" className="settings-close-button" onClick={onClose} title="Close">
          <svg width="14" height="14" viewBox="0 0 14 14">
            <path d="M 1,1 L 13,13 M 13,1 L 1,13" stroke="currentColor" strokeWidth="2" strokeLinecap="round"/>
          </svg>
        </button>
      </div>

      <div className="settings-content custom-collection-content">
        {isTemplateEdit && (
          <div className="collection-warning">You are using an existing Omni Model as a template. Saving creates a new user Omni Model; the original stays unchanged.</div>
        )}
        {hasComponentChanges && hasExistingCustomCollection && (
          <div className="collection-warning">Components have changed. Saving re-registers this Omni Model through /pull with recipe collection.omni and the updated components list.</div>
        )}

        <div className="form-section">
          <label className="form-label" title="Registered user Omni Model name">Omni Model Name</label>
          <input
            type="text"
            className="form-input"
            value={form.name}
            onChange={(e) => updateForm({ name: e.target.value.replace(/^user\./, '') })}
            placeholder="MyOmniModel"
            disabled={hasExistingCustomCollection}
          />
        </div>

        <div className="collection-role-list">
          {renderRoleSelect('llm', true)}
          {form.llm && !selectedLlmHasToolCalling && (
            <div className="collection-warning">This planner LLM may not support tool calling. The Omni Model can still be saved, but OmniRouter tools may be unreliable with this model.</div>
          )}
          {OPTIONAL_ROLES.map((role) => renderRoleSelect(role))}
        </div>

        {error && <div className="form-error">{error}</div>}
      </div>

      <div className="settings-footer custom-collection-footer">
        <button type="button" className="settings-reset-button" onClick={handleExport}>Export Omni Model</button>
        <button type="button" className="settings-reset-button" onClick={onClose}>Cancel</button>
        <button type="button" className="settings-save-button" onClick={handleSave}>{hasExistingCustomCollection ? 'Save' : 'Create'}</button>
      </div>
    </>
  );
};

export default CustomCollectionPanel;
