import React, { useMemo, useRef, useState } from 'react';
import { useModels } from '../hooks/useModels';
import {
  CustomWorkflow,
  CustomWorkflowDraft,
  CustomWorkflowRole,
  buildCustomWorkflowsExportPayload,
  getCustomWorkflowRoleLabel,
  getWorkflowRoleOptions,
  importCustomWorkflows,
  loadCustomWorkflows,
} from '../utils/customWorkflows';

interface CustomWorkflowPanelProps {
  onClose: () => void;
  onSave: (workflow: CustomWorkflowDraft) => void | Promise<void>;
  onDelete: (workflowId: string) => void | Promise<void>;
}

const DEFAULT_NAME = 'My Omni Workflow';

const OPTIONAL_ROLES: CustomWorkflowRole[] = ['vision', 'image', 'edit', 'transcription', 'speech'];

const roleDescriptions: Record<CustomWorkflowRole, string> = {
  llm: 'Required. This model drives the chat loop and decides which tools to call. Only one planner LLM is used in this first workflow UI.',
  vision: 'Optional. Used for analyze_image when the main LLM is not the vision model you want.',
  image: 'Optional. Used for generate_image.',
  edit: 'Optional. Used for edit_image. If your image model also has the edit label, select it here too.',
  transcription: 'Optional. Used for audio transcription.',
  speech: 'Optional. Used for text-to-speech.',
};

const emptyDraft = () => ({
  selectedWorkflowId: '',
  name: DEFAULT_NAME,
  llm: '',
  vision: '',
  image: '',
  edit: '',
  transcription: '',
  speech: '',
  createdAt: undefined as string | undefined,
});

const draftFromWorkflow = (workflow: CustomWorkflow) => ({
  selectedWorkflowId: workflow.id,
  name: workflow.name,
  llm: workflow.components.llm,
  vision: workflow.components.vision ?? '',
  image: workflow.components.image ?? '',
  edit: workflow.components.edit ?? '',
  transcription: workflow.components.transcription ?? '',
  speech: workflow.components.speech ?? '',
  createdAt: workflow.createdAt,
});

const CustomWorkflowPanel: React.FC<CustomWorkflowPanelProps> = ({ onClose, onSave, onDelete }) => {
  const { modelsData } = useModels();
  const [savedWorkflows, setSavedWorkflows] = useState<CustomWorkflow[]>(() => loadCustomWorkflows());
  const [form, setForm] = useState(() => emptyDraft());
  const [error, setError] = useState<string | null>(null);
  const [notice, setNotice] = useState<string | null>(null);
  const importFileRef = useRef<HTMLInputElement>(null);

  const options = useMemo(() => ({
    llm: getWorkflowRoleOptions(modelsData, 'llm'),
    vision: getWorkflowRoleOptions(modelsData, 'vision'),
    image: getWorkflowRoleOptions(modelsData, 'image'),
    edit: getWorkflowRoleOptions(modelsData, 'edit'),
    transcription: getWorkflowRoleOptions(modelsData, 'transcription'),
    speech: getWorkflowRoleOptions(modelsData, 'speech'),
  }), [modelsData]);

  const selectedLlmLabels = form.llm ? (modelsData[form.llm]?.labels ?? []) : [];
  const selectedLlmHasToolCalling = selectedLlmLabels.includes('tool-calling') || selectedLlmLabels.includes('tools');
  const isEditing = !!form.selectedWorkflowId;

  const refreshSavedWorkflows = () => setSavedWorkflows(loadCustomWorkflows());

  const updateForm = (patch: Partial<typeof form>) => {
    setForm((prev) => ({ ...prev, ...patch }));
    setError(null);
    setNotice(null);
  };

  const setRole = (role: CustomWorkflowRole, value: string) => {
    updateForm({ [role]: value } as Partial<typeof form>);
  };

  const valueForRole = (role: CustomWorkflowRole): string => form[role];

  const handleExistingWorkflowChange = (workflowId: string) => {
    setError(null);
    setNotice(null);
    if (!workflowId) {
      setForm(emptyDraft());
      return;
    }
    const workflow = savedWorkflows.find((item) => item.id === workflowId);
    if (workflow) setForm(draftFromWorkflow(workflow));
  };

  const renderRoleSelect = (role: CustomWorkflowRole, required = false) => {
    const roleOptions = options[role];
    return (
      <div className="workflow-role-row" key={role}>
        <label className="form-label" title={roleDescriptions[role]}>
          {getCustomWorkflowRoleLabel(role)}{required ? ' *' : ''}
        </label>
        <select
          className="form-input form-select workflow-model-select"
          value={valueForRole(role)}
          onChange={(e) => setRole(role, e.target.value)}
        >
          {!required && <option value="">None</option>}
          {required && <option value="">Select a model...</option>}
          {roleOptions.map((model) => (
            <option key={model.id} value={model.id}>{model.info.workflow_name ?? model.id}</option>
          ))}
        </select>
        <div className="workflow-role-help">{roleDescriptions[role]}</div>
        {roleOptions.length === 0 && (
          <div className="workflow-role-empty">No downloaded compatible model found for this role.</div>
        )}
      </div>
    );
  };

  const handleSave = async () => {
    const cleanName = form.name.trim();
    if (!cleanName) {
      setError('Workflow name is required.');
      return;
    }
    if (!form.llm) {
      setError('Select an LLM model for the workflow.');
      return;
    }

    await onSave({
      id: form.selectedWorkflowId || undefined,
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
    refreshSavedWorkflows();
  };

  const handleDelete = async () => {
    if (!form.selectedWorkflowId) return;
    await onDelete(form.selectedWorkflowId);
    setForm(emptyDraft());
    refreshSavedWorkflows();
  };


  const handleExport = () => {
    setError(null);
    setNotice(null);

    const payload = buildCustomWorkflowsExportPayload();
    if (payload.workflows.length === 0) {
      setNotice('No custom workflows to export yet.');
      return;
    }

    const blob = new Blob([JSON.stringify(payload, null, 2)], { type: 'application/json' });
    const url = window.URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.href = url;
    link.download = `lemonade-custom-workflows-${payload.exportedAt.slice(0, 10)}.json`;
    document.body.appendChild(link);
    link.click();
    link.remove();
    window.URL.revokeObjectURL(url);
    setNotice(`Exported ${payload.workflows.length} custom workflow${payload.workflows.length === 1 ? '' : 's'}.`);
  };

  const handleImportFile = (event: React.ChangeEvent<HTMLInputElement>) => {
    const file = event.target.files?.[0];
    event.target.value = '';
    if (!file) return;

    const reader = new FileReader();
    reader.onload = (loadEvent) => {
      try {
        const parsed = JSON.parse(String(loadEvent.target?.result ?? ''));
        const result = importCustomWorkflows(parsed);
        setSavedWorkflows(result.workflows);
        setNotice(`Imported ${result.imported} custom workflow${result.imported === 1 ? '' : 's'}${result.skipped > 0 ? `; skipped ${result.skipped} invalid entr${result.skipped === 1 ? 'y' : 'ies'}` : ''}.`);
        setError(null);
      } catch (importError) {
        setNotice(null);
        setError(importError instanceof Error ? importError.message : 'Failed to import custom workflows.');
      }
    };
    reader.onerror = () => {
      setNotice(null);
      setError('Failed to read the selected file.');
    };
    reader.readAsText(file);
  };

  return (
    <>
      <div className="settings-header">
        <h3>{isEditing ? 'Edit Custom Omni Workflow' : 'Create Custom Omni Workflow'}</h3>
        <button className="settings-close-button" onClick={onClose} title="Close">
          <svg width="14" height="14" viewBox="0 0 14 14">
            <path d="M 1,1 L 13,13 M 13,1 L 1,13" stroke="currentColor" strokeWidth="2" strokeLinecap="round"/>
          </svg>
        </button>
      </div>

      <div className="settings-content custom-workflow-content">
        <div className="workflow-intro">
          Pick already-downloaded models and save them as one OmniRouter workflow. Existing workflows can be selected here, edited, and deleted.
        </div>

        <div className="workflow-policy-note">
          <strong>Compatibility note:</strong> this does not add a separate policy or safety LLM. OmniRouter uses the selected LLM as the planner and exposes tools only for the compatible role models you pick below. Each role model keeps its own registered recipe/backend from Model Manager.
        </div>

        <div className="form-section workflow-backup-section">
          <label className="form-label" title="Export or import saved custom workflows">Backup</label>
          <div className="workflow-io-row">
            <button type="button" className="settings-reset-button" onClick={handleExport}>
              Export Workflows
            </button>
            <button type="button" className="settings-reset-button" onClick={() => importFileRef.current?.click()}>
              Import Workflows
            </button>
            <input
              ref={importFileRef}
              type="file"
              accept=".json,application/json"
              className="workflow-import-input"
              onChange={handleImportFile}
            />
          </div>
        </div>

        {savedWorkflows.length > 0 && (
          <div className="form-section workflow-existing-section">
            <label className="form-label" title="Select a saved workflow to edit it">Existing workflows</label>
            <div className="workflow-existing-row">
              <select
                className="form-input form-select workflow-model-select"
                value={form.selectedWorkflowId}
                onChange={(e) => handleExistingWorkflowChange(e.target.value)}
              >
                <option value="">Create a new workflow...</option>
                {savedWorkflows.map((workflow) => (
                  <option key={workflow.id} value={workflow.id}>{workflow.name}</option>
                ))}
              </select>
              <button type="button" className="settings-reset-button workflow-new-button" onClick={() => setForm(emptyDraft())}>
                New
              </button>
            </div>
          </div>
        )}

        <div className="form-section">
          <label className="form-label" title="A friendly name shown in the model picker">Workflow Name</label>
          <div className="input-with-prefix">
            <span className="input-prefix">workflow.</span>
            <input
              type="text"
              className="form-input with-prefix"
              value={form.name}
              onChange={(e) => updateForm({ name: e.target.value })}
              placeholder="Creator Studio"
            />
          </div>
        </div>

        <div className="workflow-role-list">
          {renderRoleSelect('llm', true)}
          {form.llm && !selectedLlmHasToolCalling && (
            <div className="workflow-warning">This LLM is not labeled as tool-calling. The workflow can still be saved, but OmniRouter tools may be unreliable with this model.</div>
          )}
          {form.llm && selectedLlmHasToolCalling && (
            <div className="workflow-info">The selected LLM will be the only planner. Select a separate Vision model only if image analysis should use a different multimodal LLM.</div>
          )}
          {OPTIONAL_ROLES.map((role) => renderRoleSelect(role))}
        </div>

        {notice && <div className="workflow-info">{notice}</div>}
        {error && <div className="form-error">{error}</div>}
      </div>

      <div className="settings-footer custom-workflow-footer">
        {isEditing && (
          <button className="workflow-delete-button" onClick={handleDelete}>
            Delete Workflow
          </button>
        )}
        <button className="settings-reset-button" onClick={onClose}>Cancel</button>
        <button className="settings-save-button" onClick={handleSave}>{isEditing ? 'Save Changes' : 'Save Workflow'}</button>
      </div>
    </>
  );
};

export default CustomWorkflowPanel;
