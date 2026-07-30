import React, { useEffect, useMemo, useState } from 'react';
import { useModels } from '../hooks/useModels';
import { getModelDisplayName } from '../utils/modelDisplayName';
import type { ModelInfo } from '../utils/modelData';
import {
  CustomCollection,
  CustomCollectionDraft,
  CustomCollectionRole,
  DEFAULT_OMNI_SYSTEM_PROMPT,
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

const DEFAULT_OMNI_NAME = 'MyOmniModel';
const OPTIONAL_ROLES: CustomCollectionRole[] = ['vision', 'image', 'edit', 'transcription', 'speech'];

const roleDescriptions: Record<CustomCollectionRole, string> = {
  llm: 'Required planner model for chat and tool calls.',
  vision: 'Optional model used for image analysis.',
  image: 'Optional model used for image generation.',
  edit: 'Optional model used for image editing.',
  transcription: 'Optional model used for speech-to-text.',
  speech: 'Optional model used for text-to-speech.',
};

const emptyOmniDraft = () => ({
  selectedCollectionId: '',
  sourceCollectionId: '',
  name: DEFAULT_OMNI_NAME,
  llm: '',
  vision: '',
  image: '',
  edit: '',
  transcription: '',
  speech: '',
  // Pre-fill with the current default so authors can see the prompt they
  // would otherwise inherit. formToDraft compares the textbox to this default
  // and stores no override when they match — see DEFAULT_OMNI_SYSTEM_PROMPT.
  systemPrompt: DEFAULT_OMNI_SYSTEM_PROMPT,
  createdAt: undefined as string | undefined,
});

type OmniForm = ReturnType<typeof emptyOmniDraft>;

const omniDraftFromCollection = (collection: CustomCollection, sourceId: string, isCustom: boolean): OmniForm => ({
  selectedCollectionId: isCustom ? collection.id : '',
  sourceCollectionId: sourceId,
  name: isCustom ? collection.name : `${getCollectionDisplayName(sourceId)} Custom`,
  llm: collection.components.llm,
  vision: collection.components.vision ?? '',
  image: collection.components.image ?? '',
  edit: collection.components.edit ?? '',
  transcription: collection.components.transcription ?? '',
  speech: collection.components.speech ?? '',
  systemPrompt: collection.systemPrompt ?? DEFAULT_OMNI_SYSTEM_PROMPT,
  createdAt: collection.createdAt,
});

const omniFormToDraft = (form: OmniForm): CustomCollectionDraft => {
  // Text identical to the shipped default is treated as "no override" so the
  // collection stays on the global fallback — a later toolDefinitions.json
  // update reaches existing collections instead of being shadowed by a frozen
  // copy. Compare on trimmed values so accidental leading/trailing whitespace
  // around the default doesn't get persisted as a real override. A blank
  // textarea (after trim) maps to the same "no override" state.
  const trimmed = form.systemPrompt.trim();
  const isDefault = !trimmed || trimmed === DEFAULT_OMNI_SYSTEM_PROMPT.trim();
  return {
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
    systemPrompt: isDefault ? undefined : trimmed,
  };
};

const CustomCollectionPanel: React.FC<CustomCollectionPanelProps> = ({
  mode,
  collectionId,
  onClose,
  onSave,
  onExport,
}) => {
  const { modelsData } = useModels();
  const [omniForm, setOmniForm] = useState<OmniForm>(() => emptyOmniDraft());
  const [originalComponents, setOriginalComponents] = useState<string[]>([]);
  const [error, setError] = useState<string | null>(null);

  const omniOptions = useMemo(() => ({
    llm: getCollectionRoleOptions(modelsData, 'llm'),
    vision: getCollectionRoleOptions(modelsData, 'vision'),
    image: getCollectionRoleOptions(modelsData, 'image'),
    edit: getCollectionRoleOptions(modelsData, 'edit'),
    transcription: getCollectionRoleOptions(modelsData, 'transcription'),
    speech: getCollectionRoleOptions(modelsData, 'speech'),
  }), [modelsData]);

  useEffect(() => {
    setError(null);
    if (mode !== 'edit' || !collectionId) {
      setOmniForm(emptyOmniDraft());
      setOriginalComponents([]);
      return;
    }

    const collection = modelEntryToCustomCollection(collectionId, modelsData[collectionId], modelsData);
    if (!collection) {
      setOmniForm(emptyOmniDraft());
      setOriginalComponents([]);
      setError('This collection could not be found. Refresh models and try again.');
      return;
    }

    const next = omniDraftFromCollection(
      collection,
      collectionId,
      isCustomCollectionModel(collectionId, modelsData[collectionId]),
    );
    if (!next.vision && next.llm && (modelsData[next.llm]?.labels ?? []).includes('vision')) {
      next.vision = next.llm;
    }
    if (!next.edit && next.image && (modelsData[next.image]?.labels ?? []).includes('edit')) {
      next.edit = next.image;
    }
    setOmniForm(next);
    setOriginalComponents(getCustomCollectionComponentList(collection));
  }, [mode, collectionId, modelsData]);

  const modelHasLabel = (modelId: string, label: string) =>
    (modelsData[modelId]?.labels ?? []).includes(label);

  const plannerProvidesVision = omniForm.llm.length > 0 && modelHasLabel(omniForm.llm, 'vision');
  const imageProvidesEdit = omniForm.image.length > 0 && modelHasLabel(omniForm.image, 'edit');
  const selectedLlmLabels = omniForm.llm ? (modelsData[omniForm.llm]?.labels ?? []) : [];
  const selectedLlmHasToolCalling =
    selectedLlmLabels.includes('tool-calling') || selectedLlmLabels.includes('tools');
  const currentComponents = getCustomCollectionComponentList(omniFormToDraft(omniForm));
  const hasExistingCustomCollection = mode === 'edit' && !!omniForm.selectedCollectionId;
  const isTemplateEdit =
    mode === 'edit' && !!omniForm.sourceCollectionId && !omniForm.selectedCollectionId;
  const hasComponentChanges =
    originalComponents.length > 0 &&
    (originalComponents.length !== currentComponents.length ||
      originalComponents.some((c, i) => c !== currentComponents[i]));

  const updateOmniForm = (patch: Partial<OmniForm>) => {
    setOmniForm((prev) => ({ ...prev, ...patch }));
    setError(null);
  };

  const setOmniRole = (role: CustomCollectionRole, value: string) => {
    setOmniForm((prev) => {
      const patch: Partial<OmniForm> = { [role]: value } as Partial<OmniForm>;
      if (role === 'llm') {
        const hasVision = value.length > 0 && modelHasLabel(value, 'vision');
        const visionWasDefault = !prev.vision || prev.vision === prev.llm;
        if (hasVision && visionWasDefault) patch.vision = value;
        else if (!hasVision && prev.vision === prev.llm) patch.vision = '';
      }
      if (role === 'image') {
        const hasEdit = value.length > 0 && modelHasLabel(value, 'edit');
        const editWasDefault = !prev.edit || prev.edit === prev.image;
        if (hasEdit && editWasDefault) patch.edit = value;
        else if (!hasEdit && prev.edit === prev.image) patch.edit = '';
      }
      return { ...prev, ...patch };
    });
    setError(null);
  };

  const displayNameForOption = (modelId: string): string => {
    const info = modelsData[modelId];
    const name = info?.model_name ?? getModelDisplayName(modelId);
    return `${name} (${info?.downloaded === true ? 'downloaded' : 'registered - will download'})`;
  };

  const renderOptionsGroup = (
    roleOptions: Array<{ id: string; info: ModelInfo }>,
    downloaded: boolean,
  ) => {
    const filtered = roleOptions.filter((m) => (m.info.downloaded === true) === downloaded);
    if (filtered.length === 0) return null;
    return (
      <optgroup label={downloaded ? 'Available locally' : 'Registered - will download when pulled'}>
        {filtered.map((m) => (
          <option key={m.id} value={m.id}>{displayNameForOption(m.id)}</option>
        ))}
      </optgroup>
    );
  };

  const renderOmniRoleSelect = (role: CustomCollectionRole, required = false) => {
    const selectedValue = omniForm[role];
    const roleOptions = omniOptions[role];
    const optionIds = new Set(roleOptions.map((m) => m.id));
    const selectedFallback =
      selectedValue && !optionIds.has(selectedValue) && modelsData[selectedValue]
        ? [{ id: selectedValue, info: modelsData[selectedValue] }]
        : [];
    const autoProviderModel = role === 'vision' ? omniForm.llm : role === 'edit' ? omniForm.image : '';
    const autoProvided =
      (role === 'vision' && plannerProvidesVision) || (role === 'edit' && imageProvidesEdit);
    const isUsingAutoProvider = autoProvided && selectedValue === autoProviderModel;
    const autoProviderOption =
      autoProvided && autoProviderModel && modelsData[autoProviderModel]
        ? { id: autoProviderModel, info: modelsData[autoProviderModel] }
        : null;

    const seen = new Set<string>();
    const selectOptions = selectedFallback.concat(roleOptions).filter((m) => {
      if (m.id === autoProviderModel) return false;
      if (seen.has(m.id)) return false;
      seen.add(m.id);
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
          onChange={(e) => setOmniRole(role, e.target.value)}
          title={
            autoProvided
              ? 'Automatically provided by the selected model. Select another compatible model to override.'
              : roleDescriptions[role]
          }
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
          <div className="collection-role-empty">
            No registered compatible model found. Add a custom model first so its checkpoint can be
            pulled as a component.
          </div>
        )}
      </div>
    );
  };

  const validateOmni = (): CustomCollectionDraft | null => {
    const cleanName = omniForm.name.trim();
    if (!cleanName) {
      setError('Omni Model name is required.');
      return null;
    }
    if (!omniForm.llm) {
      setError('Select a planner LLM for the Omni Model.');
      return null;
    }
    const draft = omniFormToDraft({ ...omniForm, name: cleanName });
    // Custom prompt templates must keep both substitution placeholders so the
    // planner still sees the tool list/guidance after runtime substitution. The
    // editor's subtext flags this; without enforcement an override missing a
    // placeholder would silently produce a tool-blind planner prompt.
    if (typeof draft.systemPrompt === 'string') {
      const missing: string[] = [];
      if (!draft.systemPrompt.includes('{tool_list}')) missing.push('{tool_list}');
      if (!draft.systemPrompt.includes('{tool_guidance}')) missing.push('{tool_guidance}');
      if (missing.length > 0) {
        setError(`Custom System Prompt is missing required placeholder${missing.length > 1 ? 's' : ''}: ${missing.join(' and ')}. Add them back or click Reset to default.`);
        return null;
      }
    }
    return draft;
  };

  const handleSave = async () => {
    const draft = validateOmni();
    if (!draft) return;
    void onSave(draft);
  };

  const handleExport = () => {
    const draft = validateOmni();
    if (!draft) return;
    onExport(draft);
  };

  const title = hasExistingCustomCollection ? 'Omni Model Options' : 'New Omni Model';

  return (
    <>
      <div className="settings-header">
        <h3>{title}</h3>
        <button type="button" className="settings-close-button" onClick={onClose} title="Close">
          <svg width="14" height="14" viewBox="0 0 14 14">
            <path d="M 1,1 L 13,13 M 13,1 L 1,13" stroke="currentColor" strokeWidth="2" strokeLinecap="round" />
          </svg>
        </button>
      </div>

      <div className="settings-content custom-collection-content">
        {isTemplateEdit && (
          <div className="collection-warning">
            You are using an existing Omni Model as a template. Saving creates a new user Omni
            Model; the original stays unchanged.
          </div>
        )}
        {hasComponentChanges && hasExistingCustomCollection && (
          <div className="collection-warning">
            Components have changed. Saving re-registers this Omni Model through /pull with
            recipe collection.omni and the updated components list.
          </div>
        )}

        <div className="form-section">
          <label className="form-label" title="Registered user Omni Model name">
            Omni Model Name
          </label>
          <input
            type="text"
            className="form-input"
            value={omniForm.name}
            onChange={(e) => updateOmniForm({ name: e.target.value.replace(/^user\./, '') })}
            placeholder="MyOmniModel"
            disabled={hasExistingCustomCollection}
          />
        </div>

        <div className="collection-role-list">
          {renderOmniRoleSelect('llm', true)}
          {omniForm.llm && !selectedLlmHasToolCalling && (
            <div className="collection-warning">
              This planner LLM may not support tool calling. The Omni Model can still be saved,
              but OmniRouter tools may be unreliable with this model.
            </div>
          )}
          {OPTIONAL_ROLES.map((role) => renderOmniRoleSelect(role))}
        </div>

        <div className="form-section">
          <label className="form-label">System Prompt</label>
          <div className="form-subtext">
            Edit to override the default for this Omni Model. Keep the {'{tool_list}'} and {'{tool_guidance}'} placeholders so the planner sees the available tools.
          </div>
          <textarea
            className="form-input"
            rows={8}
            value={omniForm.systemPrompt}
            onChange={(e) => updateOmniForm({ systemPrompt: e.target.value })}
            spellCheck={false}
          />
          <div className="form-actions-inline">
            <button
              type="button"
              className="settings-reset-button"
              onClick={() => updateOmniForm({ systemPrompt: DEFAULT_OMNI_SYSTEM_PROMPT })}
              disabled={(() => { const form = omniForm; return form.systemPrompt.trim() === DEFAULT_OMNI_SYSTEM_PROMPT.trim(); })()}
              title="Restore the shipped default OmniRouter system prompt."
            >
              Reset to default
            </button>
          </div>
        </div>

        {error && <div className="form-error">{error}</div>}
      </div>

      <div className="settings-footer custom-collection-footer">
        <button type="button" className="settings-reset-button" onClick={handleExport}>
          Export
        </button>
        <button type="button" className="settings-reset-button" onClick={onClose}>
          Cancel
        </button>
        <button type="button" className="settings-save-button" onClick={handleSave}>
          {hasExistingCustomCollection ? 'Save' : 'Create'}
        </button>
      </div>
    </>
  );
};

export default CustomCollectionPanel;
