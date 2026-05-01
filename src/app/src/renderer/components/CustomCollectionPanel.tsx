import React, { useEffect, useMemo, useState } from 'react';
import { useModels } from '../hooks/useModels';
import {
  CustomCollection,
  CustomCollectionDraft,
  CustomCollectionRole,
  getCollectionRoleOptions,
  getCustomCollectionRoleLabel,
  loadCustomCollections,
} from '../utils/customCollections';

interface CustomCollectionPanelProps {
  mode: 'create' | 'edit';
  collectionId?: string;
  onClose: () => void;
  onSave: (collection: CustomCollectionDraft) => void | Promise<void>;
  onDelete: (collectionId: string) => void | Promise<void>;
  onExport: (collectionId: string) => void;
}

const DEFAULT_NAME = 'My Omni Collection';

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

const draftFromCollection = (collection: CustomCollection): CollectionForm => ({
  selectedCollectionId: collection.id,
  name: collection.name,
  llm: collection.components.llm,
  vision: collection.components.vision ?? '',
  image: collection.components.image ?? '',
  edit: collection.components.edit ?? '',
  transcription: collection.components.transcription ?? '',
  speech: collection.components.speech ?? '',
  createdAt: collection.createdAt,
});

const CustomCollectionPanel: React.FC<CustomCollectionPanelProps> = ({
  mode,
  collectionId,
  onClose,
  onSave,
  onDelete,
  onExport,
}) => {
  const { modelsData } = useModels();
  const [form, setForm] = useState<CollectionForm>(() => emptyDraft());
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
  const isEditing = mode === 'edit' && !!form.selectedCollectionId;

  useEffect(() => {
    setError(null);
    if (mode !== 'edit' || !collectionId) {
      setForm(emptyDraft());
      return;
    }

    const collection = loadCustomCollections().find((item) => item.id === collectionId);
    setForm(collection ? draftFromCollection(collection) : emptyDraft());
    if (!collection) {
      setError('This custom collection could not be found.');
    }
  }, [mode, collectionId]);

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

  const renderRoleSelect = (role: CustomCollectionRole, required = false) => {
    const roleOptions = options[role];
    return (
      <div className="form-section collection-role-row" key={role}>
        <label className="form-label" title={roleDescriptions[role]}>
          {getCustomCollectionRoleLabel(role)}{required ? ' *' : ''}
        </label>
        <select
          className="form-input form-select collection-model-select"
          value={valueForRole(role)}
          onChange={(e) => setRole(role, e.target.value)}
          title={roleDescriptions[role]}
        >
          {!required && <option value="">None</option>}
          {required && <option value="">Select a model...</option>}
          {roleOptions.map((model) => (
            <option key={model.id} value={model.id}>{model.info.model_name ?? model.id}</option>
          ))}
        </select>
        {roleOptions.length === 0 && (
          <div className="collection-role-empty">No downloaded compatible model found.</div>
        )}
      </div>
    );
  };

  const handleSave = async () => {
    const cleanName = form.name.trim();
    if (!cleanName) {
      setError('Collection name is required.');
      return;
    }
    if (!form.llm) {
      setError('Select an LLM model for the collection.');
      return;
    }

    await onSave({
      id: form.selectedCollectionId || undefined,
      name: cleanName,
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
  };

  const handleDelete = async () => {
    if (!form.selectedCollectionId) return;
    await onDelete(form.selectedCollectionId);
  };

  return (
    <>
      <div className="settings-header">
        <h3>{isEditing ? 'Collection Options' : 'New Collection'}</h3>
        <button className="settings-close-button" onClick={onClose} title="Close">
          <svg width="14" height="14" viewBox="0 0 14 14">
            <path d="M 1,1 L 13,13 M 13,1 L 1,13" stroke="currentColor" strokeWidth="2" strokeLinecap="round"/>
          </svg>
        </button>
      </div>

      <div className="settings-content custom-collection-content">
        <div className="form-section">
          <label className="form-label" title="A friendly name shown in the model picker">Collection Name</label>
          <div className="input-with-prefix">
            <span className="input-prefix">collection.</span>
            <input
              type="text"
              className="form-input with-prefix"
              value={form.name}
              onChange={(e) => updateForm({ name: e.target.value })}
              placeholder="Creator Studio"
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
        {isEditing && (
          <button className="collection-delete-button" onClick={handleDelete}>
            Delete
          </button>
        )}
        {isEditing && (
          <button className="settings-reset-button" onClick={() => onExport(form.selectedCollectionId)}>
            Export Collection
          </button>
        )}
        <button className="settings-reset-button" onClick={onClose}>Cancel</button>
        <button className="settings-save-button" onClick={handleSave}>Save</button>
      </div>
    </>
  );
};

export default CustomCollectionPanel;
