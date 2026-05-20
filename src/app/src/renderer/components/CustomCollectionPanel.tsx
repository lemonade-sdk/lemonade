import React, { useEffect, useMemo, useState } from 'react';
import { useModels } from '../hooks/useModels';
import { getModelDisplayName } from '../utils/modelDisplayName';
import {
  CustomCollection,
  CustomCollectionDraft,
  CustomCollectionRole,
  getCollectionRoleOptions,
  getCustomCollectionRoleLabel,
  getCustomCollectionComponentList,
  getCollectionDisplayName,
  isCustomCollectionId,
  modelEntryToCustomCollection,
} from '../utils/customCollections';

interface CustomCollectionPanelProps {
  mode: 'create' | 'edit';
  collectionId?: string;
  onClose: () => void;
  onSave: (collection: CustomCollectionDraft) => void | Promise<void>;
  onExport: (collection: CustomCollectionDraft) => void;
}

const DEFAULT_NAME = 'MyOmniCollection';

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

const draftFromCollection = (collection: CustomCollection, sourceId: string): CollectionForm => {
  const isCustom = isCustomCollectionId(sourceId);
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

  const selectedLlmLabels = form.llm ? (modelsData[form.llm]?.labels ?? []) : [];
  const selectedLlmHasToolCalling = selectedLlmLabels.includes('tool-calling') || selectedLlmLabels.includes('tools');
  const currentComponents = componentListForForm(form);
  const hasExistingCustomCollection = mode === 'edit' && !!form.selectedCollectionId;
  const isTemplateEdit = mode === 'edit' && !!form.sourceCollectionId && !form.selectedCollectionId;
  const hasComponentChanges = originalComponents.length > 0 && (
    originalComponents.length !== currentComponents.length ||
    originalComponents.some((component, index) => component !== currentComponents[index])
  );
  const checkpointValue = `collection.omni components (${currentComponents.length})${hasComponentChanges ? ' — modified draft' : ''}`;

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
      setError('This collection could not be found. Refresh models and try again.');
      return;
    }

    const next = draftFromCollection(collection, collectionId);
    setForm(next);
    setOriginalComponents(getCustomCollectionComponentList(collection));
  }, [mode, collectionId, modelsData]);

  const updateForm = (patch: Partial<CollectionForm>) => {
    setForm((prev) => ({ ...prev, ...patch }));
    setError(null);
  };

  const modelHasVision = (modelId: string): boolean => {
    return (modelsData[modelId]?.labels ?? []).includes('vision');
  };

  const setRole = (role: CustomCollectionRole, value: string) => {
    if (role !== 'llm') {
      updateForm({ [role]: value } as Partial<CollectionForm>);
      return;
    }

    setForm((prev) => {
      const patch: Partial<CollectionForm> = { llm: value };
      const selectedPlannerHasVision = value.length > 0 && modelHasVision(value);
      const visionWasEmptyOrPlannerDefault = !prev.vision || prev.vision === prev.llm;

      if (selectedPlannerHasVision && visionWasEmptyOrPlannerDefault) {
        patch.vision = value;
      } else if (!selectedPlannerHasVision && prev.vision === prev.llm) {
        patch.vision = '';
      }

      return { ...prev, ...patch };
    });
    setError(null);
  };

  const valueForRole = (role: CustomCollectionRole): string => form[role];

  const getComponentSourceLabel = (modelId: string): string | null => {
    const info = modelsData[modelId];
    if (!info) return null;
    if (typeof info.checkpoint === 'string' && info.checkpoint.length > 0) {
      return info.checkpoint;
    }
    const checkpoints = info.checkpoints;
    if (checkpoints && typeof checkpoints === 'object' && !Array.isArray(checkpoints)) {
      const main = checkpoints.main;
      if (typeof main === 'string' && main.length > 0) return main;
      const first = Object.values(checkpoints).find((value): value is string => typeof value === 'string' && value.length > 0);
      if (first) return first;
    }
    return null;
  };

  const renderRoleSelect = (role: CustomCollectionRole, required = false) => {
    const selectedValue = valueForRole(role);
    const roleOptions = options[role];
    const selectedSourceLabel = selectedValue ? getComponentSourceLabel(selectedValue) : null;
    const optionIds = new Set(roleOptions.map((model) => model.id));
    const selectedFallback = selectedValue && !optionIds.has(selectedValue) && modelsData[selectedValue]
      ? [{ id: selectedValue, info: modelsData[selectedValue] }]
      : [];
    const selectOptions = selectedFallback.concat(roleOptions);
    const availableOptions = selectOptions.filter((model) => model.info.downloaded === true);
    const registeredOptions = selectOptions.filter((model) => model.info.downloaded !== true);
    const renderModelOption = (model: (typeof selectOptions)[number]) => {
      const label = model.info.model_name ?? getModelDisplayName(model.id);
      const stateLabel = model.info.downloaded === true ? 'downloaded' : 'registered - will download';
      return <option key={model.id} value={model.id}>{label} ({stateLabel})</option>;
    };

    return (
      <div className="form-section collection-role-row" key={role}>
        <label className="form-label" title={roleDescriptions[role]}>
          {getCustomCollectionRoleLabel(role)}{required ? ' *' : ''}
        </label>
        <select
          className="form-input form-select collection-model-select"
          value={selectedValue}
          onChange={(e) => setRole(role, e.target.value)}
          title={roleDescriptions[role]}
        >
          {!required && <option value="">None</option>}
          {required && <option value="">Select a model...</option>}
          {availableOptions.length > 0 && (
            <optgroup label="Available locally">
              {availableOptions.map(renderModelOption)}
            </optgroup>
          )}
          {registeredOptions.length > 0 && (
            <optgroup label="Registered - will download when pulled">
              {registeredOptions.map(renderModelOption)}
            </optgroup>
          )}
        </select>
        {selectedSourceLabel && (
          <div className="collection-role-source">Checkpoint: {selectedSourceLabel}</div>
        )}
        {roleOptions.length === 0 && (
          <div className="collection-role-empty">No registered compatible model found. Add a custom model first so its checkpoint can be pulled as a component.</div>
        )}
      </div>
    );
  };

  const validateDraft = (): CustomCollectionDraft | null => {
    const cleanName = form.name.trim();
    if (!cleanName) {
      setError('Collection name is required.');
      return null;
    }
    if (!form.llm) {
      setError('Select an LLM model for the collection.');
      return null;
    }
    return formToDraft({ ...form, name: cleanName });
  };

  const handleSave = async () => {
    const draft = validateDraft();
    if (!draft) return;
    await onSave(draft);
  };

  const handleExport = () => {
    const draft = validateDraft();
    if (!draft) return;
    onExport(draft);
  };

  return (
    <>
      <div className="settings-header">
        <h3>{hasExistingCustomCollection ? 'Collection Options' : 'New Collection'}</h3>
        <button className="settings-close-button" onClick={onClose} title="Close">
          <svg width="14" height="14" viewBox="0 0 14 14">
            <path d="M 1,1 L 13,13 M 13,1 L 1,13" stroke="currentColor" strokeWidth="2" strokeLinecap="round"/>
          </svg>
        </button>
      </div>

      <div className="settings-content custom-collection-content">
        <div className="collection-options-summary">
          <div className="model-info-row">
            <span className="model-info-label">Name</span>
            <span className="model-info-value">{form.selectedCollectionId || `user.${form.name || DEFAULT_NAME}`}</span>
          </div>
          <div className="model-info-row">
            <span className="model-info-label">Checkpoint</span>
            <span className="model-info-value">{checkpointValue}</span>
          </div>
          {isTemplateEdit && (
            <div className="collection-warning">You are using an existing Omni collection as a template. Saving creates a new user collection; the original collection is unchanged.</div>
          )}
          {hasComponentChanges && hasExistingCustomCollection && (
            <div className="collection-warning">Components have changed. Saving re-registers this collection through /pull with recipe collection.omni and the updated components list.</div>
          )}
        </div>

        <div className="form-section">
          <label className="form-label" title="Registered user collection name">Collection Name</label>
          <div className="input-with-prefix">
            <span className="input-prefix">user.</span>
            <input
              type="text"
              className="form-input with-prefix"
              value={form.name}
              onChange={(e) => updateForm({ name: e.target.value })}
              placeholder="CreatorStudio"
              disabled={hasExistingCustomCollection}
            />
          </div>
        </div>

        <div className="collection-role-list">
          {renderRoleSelect('llm', true)}
          {form.llm && !selectedLlmHasToolCalling && (
            <div className="collection-warning">This LLM is not labeled as tool-calling. The collection can still be saved, but OmniRouter tools may be unreliable with this model.</div>
          )}
          {OPTIONAL_ROLES.map((role) => renderRoleSelect(role))}
        </div>

        {error && <div className="form-error">{error}</div>}
      </div>

      <div className="settings-footer custom-collection-footer">
        <button className="settings-reset-button" onClick={handleExport}>Export Collection</button>
        <button className="settings-reset-button" onClick={onClose}>Cancel</button>
        <button className="settings-save-button" onClick={handleSave}>{hasExistingCustomCollection ? 'Save' : 'Create'}</button>
      </div>
    </>
  );
};

export default CustomCollectionPanel;
