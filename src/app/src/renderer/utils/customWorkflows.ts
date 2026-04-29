import type { ModelInfo, ModelsData } from './modelData';
import { isChatPlannerCandidate } from './modelLabels';

export const CUSTOM_WORKFLOW_PREFIX = 'workflow.';
export const CUSTOM_WORKFLOWS_STORAGE_KEY = 'lemonade.customWorkflows.v1';
export const CUSTOM_WORKFLOWS_EXPORT_VERSION = 1;

export type CustomWorkflowRole = 'llm' | 'vision' | 'image' | 'edit' | 'transcription' | 'speech';

export interface CustomWorkflowComponents {
  llm: string;
  vision?: string;
  image?: string;
  edit?: string;
  transcription?: string;
  speech?: string;
}

export interface CustomWorkflow {
  id: string;
  name: string;
  components: CustomWorkflowComponents;
  createdAt: string;
  updatedAt: string;
}

export interface CustomWorkflowExportPayload {
  version: number;
  exportedAt: string;
  workflows: CustomWorkflow[];
}

export interface CustomWorkflowImportResult {
  imported: number;
  skipped: number;
  workflows: CustomWorkflow[];
}

export type CustomWorkflowDraft = Pick<CustomWorkflow, 'name' | 'components'> & Partial<Pick<CustomWorkflow, 'id' | 'createdAt' | 'updatedAt'>>;

const ROLE_LABELS: Record<CustomWorkflowRole, string> = {
  llm: 'LLM',
  vision: 'Vision / image analysis',
  image: 'Image generation',
  edit: 'Image editing',
  transcription: 'Speech-to-text',
  speech: 'Text-to-speech',
};

const hasStorage = (): boolean => {
  return typeof window !== 'undefined' && typeof window.localStorage !== 'undefined';
};

const slugify = (value: string): string => {
  const slug = value
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-+|-+$/g, '');
  return slug || 'custom-workflow';
};

const buildWorkflowId = (name: string): string => `${CUSTOM_WORKFLOW_PREFIX}${slugify(name)}`;

const normalizeId = (name: string, explicitId?: string): string => {
  if (explicitId && explicitId.startsWith(CUSTOM_WORKFLOW_PREFIX)) {
    return explicitId;
  }
  return buildWorkflowId(name);
};

const makeUniqueWorkflowId = (name: string, workflows: CustomWorkflow[]): string => {
  const baseId = buildWorkflowId(name);
  const existingIds = new Set(workflows.map((workflow) => workflow.id));
  if (!existingIds.has(baseId)) return baseId;

  let suffix = 2;
  let candidate = `${baseId}-${suffix}`;
  while (existingIds.has(candidate)) {
    suffix += 1;
    candidate = `${baseId}-${suffix}`;
  }
  return candidate;
};

const isRecord = (value: unknown): value is Record<string, unknown> => {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
};

const normalizeOptionalModel = (value: unknown): string | undefined => {
  return typeof value === 'string' && value.trim().length > 0 ? value.trim() : undefined;
};

const normalizeComponents = (value: unknown): CustomWorkflowComponents | null => {
  if (!isRecord(value)) return null;
  const llm = normalizeOptionalModel(value.llm);
  if (!llm) return null;
  return {
    llm,
    vision: normalizeOptionalModel(value.vision),
    image: normalizeOptionalModel(value.image),
    edit: normalizeOptionalModel(value.edit),
    transcription: normalizeOptionalModel(value.transcription),
    speech: normalizeOptionalModel(value.speech),
  };
};

export const normalizeCustomWorkflow = (value: unknown): CustomWorkflow | null => {
  if (!isRecord(value)) return null;
  const name = typeof value.name === 'string' && value.name.trim().length > 0
    ? value.name.trim()
    : 'Custom Workflow';
  const components = normalizeComponents(value.components);
  if (!components) return null;
  const now = new Date().toISOString();
  return {
    id: normalizeId(name, typeof value.id === 'string' ? value.id : undefined),
    name,
    components,
    createdAt: typeof value.createdAt === 'string' ? value.createdAt : now,
    updatedAt: typeof value.updatedAt === 'string' ? value.updatedAt : now,
  };
};

export const isCustomWorkflowId = (modelId: string): boolean => {
  return modelId.startsWith(CUSTOM_WORKFLOW_PREFIX);
};

export const getCustomWorkflowRoleLabel = (role: CustomWorkflowRole): string => ROLE_LABELS[role];

export const getCustomWorkflowComponentList = (workflow: Pick<CustomWorkflow, 'components'>): string[] => {
  const ordered = [
    workflow.components.llm,
    workflow.components.vision,
    workflow.components.image,
    workflow.components.edit,
    workflow.components.transcription,
    workflow.components.speech,
  ].filter((model): model is string => typeof model === 'string' && model.length > 0);

  return Array.from(new Set(ordered));
};

export const loadCustomWorkflows = (): CustomWorkflow[] => {
  if (!hasStorage()) return [];
  try {
    const raw = window.localStorage.getItem(CUSTOM_WORKFLOWS_STORAGE_KEY);
    if (!raw) return [];
    const parsed = JSON.parse(raw);
    if (!Array.isArray(parsed)) return [];
    return parsed
      .map(normalizeCustomWorkflow)
      .filter((workflow): workflow is CustomWorkflow => workflow !== null)
      .sort((a, b) => a.name.localeCompare(b.name));
  } catch (error) {
    console.error('Failed to load custom workflows:', error);
    return [];
  }
};

const persistCustomWorkflows = (workflows: CustomWorkflow[]): void => {
  if (!hasStorage()) return;
  try {
    window.localStorage.setItem(CUSTOM_WORKFLOWS_STORAGE_KEY, JSON.stringify(workflows));
    window.dispatchEvent(new CustomEvent('customWorkflowsUpdated'));
  } catch (error) {
    console.error('Failed to save custom workflows:', error);
  }
};

export const saveCustomWorkflow = (draft: CustomWorkflowDraft): CustomWorkflow => {
  const workflows = loadCustomWorkflows();
  const explicitId = typeof draft.id === 'string' && draft.id.startsWith(CUSTOM_WORKFLOW_PREFIX)
    ? draft.id
    : undefined;
  const workflowId = explicitId ?? makeUniqueWorkflowId(draft.name, workflows);

  const normalized = normalizeCustomWorkflow({
    ...draft,
    id: workflowId,
    createdAt: draft.createdAt,
    updatedAt: new Date().toISOString(),
  });

  if (!normalized) {
    throw new Error('Custom workflow requires a name and an LLM model.');
  }

  const existing = workflows.find((workflow) => workflow.id === normalized.id);
  const saved: CustomWorkflow = {
    ...normalized,
    createdAt: existing?.createdAt ?? normalized.createdAt,
  };
  const next = workflows.filter((workflow) => workflow.id !== saved.id).concat(saved)
    .sort((a, b) => a.name.localeCompare(b.name));

  persistCustomWorkflows(next);
  return saved;
};

export const deleteCustomWorkflow = (workflowId: string): void => {
  const next = loadCustomWorkflows().filter((workflow) => workflow.id !== workflowId);
  persistCustomWorkflows(next);
};


export const buildCustomWorkflowsExportPayload = (): CustomWorkflowExportPayload => ({
  version: CUSTOM_WORKFLOWS_EXPORT_VERSION,
  exportedAt: new Date().toISOString(),
  workflows: loadCustomWorkflows(),
});

const extractImportCandidates = (value: unknown): unknown[] => {
  if (Array.isArray(value)) return value;
  if (!isRecord(value)) return [];

  if (Array.isArray(value.workflows)) return value.workflows;

  // Be permissive for hand-authored files and single-workflow exports.
  if (isRecord(value.components)) return [value];

  return [];
};

interface NormalizedImportCandidate {
  workflow: CustomWorkflow;
  hasExplicitId: boolean;
}

const normalizeImportCandidate = (value: unknown): NormalizedImportCandidate | null => {
  const workflow = normalizeCustomWorkflow(value);
  if (!workflow) return null;

  const hasExplicitId = isRecord(value)
    && typeof value.id === 'string'
    && value.id.startsWith(CUSTOM_WORKFLOW_PREFIX);

  return { workflow, hasExplicitId };
};

export const importCustomWorkflows = (value: unknown): CustomWorkflowImportResult => {
  const candidates = extractImportCandidates(value);
  if (candidates.length === 0) {
    throw new Error('No custom workflows found in the selected file.');
  }

  const imported = candidates
    .map(normalizeImportCandidate)
    .filter((candidate): candidate is NormalizedImportCandidate => candidate !== null);

  if (imported.length === 0) {
    throw new Error('The selected file does not contain valid custom workflows.');
  }

  const now = new Date().toISOString();
  const existing = loadCustomWorkflows();
  const byId = new Map<string, CustomWorkflow>();

  for (const workflow of existing) {
    byId.set(workflow.id, workflow);
  }

  for (const { workflow, hasExplicitId } of imported) {
    const storedWorkflows = Array.from(byId.values());
    const workflowId = hasExplicitId ? workflow.id : makeUniqueWorkflowId(workflow.name, storedWorkflows);
    const previous = byId.get(workflowId);
    byId.set(workflowId, {
      ...workflow,
      id: workflowId,
      createdAt: previous?.createdAt ?? workflow.createdAt ?? now,
      updatedAt: now,
    });
  }

  const workflows = Array.from(byId.values()).sort((a, b) => a.name.localeCompare(b.name));
  persistCustomWorkflows(workflows);

  return {
    imported: imported.length,
    skipped: candidates.length - imported.length,
    workflows,
  };
};

export const customWorkflowToModelInfo = (workflow: CustomWorkflow, modelsData: ModelsData): ModelInfo => {
  const components = getCustomWorkflowComponentList(workflow);
  const llmInfo = modelsData[workflow.components.llm];
  const downloaded = components.length > 0 && components.every((component) => modelsData[component]?.downloaded === true);
  const labels = new Set<string>(['custom', 'workflow', 'tool-calling']);

  if (llmInfo?.labels?.includes('vision') || workflow.components.vision) labels.add('vision');
  if (workflow.components.image) labels.add('image');
  if (workflow.components.edit) labels.add('edit');
  if (workflow.components.transcription) labels.add('transcription');
  if (workflow.components.speech) labels.add('speech');

  return {
    checkpoint: '',
    recipe: 'collection',
    suggested: true,
    downloaded,
    labels: Array.from(labels),
    composite_models: components,
    max_prompt_length: llmInfo?.max_prompt_length,
    source: 'custom-workflow',
    workflow_source: 'custom',
    workflow_components: workflow.components,
    workflow_name: workflow.name,
  };
};

export const mergeCustomWorkflowsIntoModelsData = (modelsData: ModelsData): ModelsData => {
  const merged: ModelsData = { ...modelsData };

  for (const workflow of loadCustomWorkflows()) {
    const components = getCustomWorkflowComponentList(workflow);
    if (components.length === 0) continue;

    // Keep stale workflows out of the selector when one of their component
    // models has been deleted or renamed. The workflow remains in localStorage
    // and comes back automatically if the component models reappear.
    if (!components.every((component) => merged[component])) continue;

    merged[workflow.id] = customWorkflowToModelInfo(workflow, merged);
  }

  return merged;
};

export const isWorkflowEligibleModel = (modelId: string, info: ModelInfo | undefined, role: CustomWorkflowRole): boolean => {
  if (!info || isCustomWorkflowId(modelId) || info.recipe === 'collection' || info.downloaded !== true) {
    return false;
  }

  const labels = info.labels ?? [];
  const hasLabel = (...needles: string[]) => labels.some((label) => needles.includes(label));

  switch (role) {
    case 'llm':
      return isChatPlannerCandidate(info);
    case 'vision':
      return hasLabel('vision');
    case 'image':
      return hasLabel('image');
    case 'edit':
      return hasLabel('edit');
    case 'transcription':
      return hasLabel('transcription') || (hasLabel('audio') && !hasLabel('vision', 'tool-calling', 'tools'));
    case 'speech':
      return hasLabel('speech', 'tts');
    default:
      return false;
  }
};

export const getWorkflowRoleOptions = (modelsData: ModelsData, role: CustomWorkflowRole): Array<{ id: string; info: ModelInfo }> => {
  return Object.entries(modelsData)
    .filter(([id, info]) => isWorkflowEligibleModel(id, info, role))
    .map(([id, info]) => ({ id, info }))
    .sort((a, b) => a.id.localeCompare(b.id));
};
