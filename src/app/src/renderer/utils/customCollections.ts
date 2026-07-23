import type { ModelInfo, ModelsData } from './modelData';
import { USER_MODEL_PREFIX } from './modelData';
import { isChatPlannerCandidate } from './modelLabels';
import { COLLECTION_OMNI_MODEL_RECIPE, COLLECTION_ROUTER_MODEL_RECIPE, isCollectionRecipe } from './recipeNames';
import toolDefinitions from './toolDefinitions.json';
import {
  canonicalizeMatchExpr,
  matchExprRoundTripIsLossy,
  matchExprToRuleNode,
  ruleNodeToMatchExpr,
  stableStringify,
} from './routerTree';

export const CUSTOM_COLLECTION_PREFIX = USER_MODEL_PREFIX;

// The shipped OmniRouter system prompt, exposed so the Omni Model editor can
// show authors the text they're (potentially) overriding. Matched verbatim on
// save: when the textarea content equals this string, the editor stores no
// override and the collection stays on whatever the global default is at
// runtime - so a future tweak to toolDefinitions.json propagates automatically.
export const DEFAULT_OMNI_SYSTEM_PROMPT: string = toolDefinitions.system_prompt;

export type CustomCollectionRole = 'llm' | 'vision' | 'image' | 'edit' | 'transcription' | 'speech';

export interface CustomCollectionComponents {
  llm: string;
  vision?: string;
  image?: string;
  edit?: string;
  transcription?: string;
  speech?: string;
}

export interface CustomCollection {
  id: string;
  name: string;
  createdAt?: string;
  updatedAt?: string;
  components: CustomCollectionComponents;
  // Optional per-collection system prompt template. Overrides the global
  // default in toolDefinitions.json when set; still uses {tool_list} and
  // {tool_guidance} placeholders for runtime substitution.
  systemPrompt?: string;
}

export interface CustomCollectionDraft {
  id?: string;
  name: string;
  createdAt?: string;
  components: CustomCollectionComponents;
  systemPrompt?: string;
}

export interface CustomCollectionPullRequest {
  model_name: string;
  recipe: typeof COLLECTION_OMNI_MODEL_RECIPE;
  components: string[];
  // Optional template (matches the registry's system_prompt field).
  system_prompt?: string;
}

const roleLabels: Record<CustomCollectionRole, string> = {
  llm: 'Planner LLM',
  vision: 'Vision',
  image: 'Image Generation',
  edit: 'Image Editing',
  transcription: 'Transcription',
  speech: 'Text to Speech',
};

export const getCustomCollectionRoleLabel = (role: CustomCollectionRole): string => roleLabels[role];

export const isCustomCollectionId = (modelId: string): boolean => modelId.startsWith(CUSTOM_COLLECTION_PREFIX);

export const isCustomCollectionModel = (modelId: string, info?: ModelInfo): boolean => {
  if (!isCollectionRecipe(info?.recipe)) return false;
  if (modelId.startsWith(CUSTOM_COLLECTION_PREFIX)) return true;
  if ((info?.labels ?? []).includes('custom')) return true;
  if (info?.source === 'user' || info?.source === 'user_models' || info?.source === 'custom') return true;
  return info?.suggested !== true;
};

export const isCollectionEditableAsCustom = (info?: ModelInfo): boolean => isCollectionRecipe(info?.recipe);

const isRecord = (value: unknown): value is Record<string, unknown> => {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
};

const cleanName = (value: string): string => {
  return value.trim().replace(/^user\./, '');
};

const slugify = (value: string): string => {
  const slug = cleanName(value)
    .replace(/[^a-zA-Z0-9._-]+/g, '-')
    .replace(/^-+|-+$/g, '')
    .slice(0, 72);
  return slug || 'CustomCollection';
};

export const makeCollectionId = (name: string): string => {
  const trimmed = name.trim();
  return trimmed.startsWith(CUSTOM_COLLECTION_PREFIX)
    ? trimmed
    : `${CUSTOM_COLLECTION_PREFIX}${slugify(trimmed)}`;
};

export const getCollectionDisplayName = (modelId: string): string => {
  return modelId.startsWith(CUSTOM_COLLECTION_PREFIX)
    ? modelId.slice(CUSTOM_COLLECTION_PREFIX.length)
    : modelId;
};

const normalizeComponentValue = (value: unknown): string | undefined => {
  return typeof value === 'string' && value.trim() ? value.trim() : undefined;
};

const firstComponentWithLabel = (
  components: string[],
  modelsData: ModelsData,
  labelsToMatch: string[],
): string | undefined => {
  const labels = new Set(labelsToMatch);
  return components.find((component) => (modelsData[component]?.labels ?? []).some((label) => labels.has(label)));
};

const inferComponentsFromList = (components: string[], modelsData: ModelsData): CustomCollectionComponents | null => {
  const ordered = components.filter((component): component is string => typeof component === 'string' && component.length > 0);
  if (ordered.length === 0) return null;

  const llm = ordered.find((component) => isChatPlannerCandidate(modelsData[component])) ?? ordered[0];
  const result: CustomCollectionComponents = { llm };

  const vision = firstComponentWithLabel(ordered, modelsData, ['vision']);
  if (vision) result.vision = vision;

  const image = firstComponentWithLabel(ordered, modelsData, ['image']);
  if (image) result.image = image;

  const edit = firstComponentWithLabel(ordered, modelsData, ['edit']);
  if (edit) result.edit = edit;

  const transcription = firstComponentWithLabel(ordered, modelsData, ['transcription', 'audio']);
  if (transcription) result.transcription = transcription;

  const speech = firstComponentWithLabel(ordered, modelsData, ['tts', 'speech']);
  if (speech) result.speech = speech;

  return result;
};

const normalizeComponents = (value: unknown, modelsData: ModelsData = {}): CustomCollectionComponents | null => {
  if (Array.isArray(value)) {
    return inferComponentsFromList(value.filter((item): item is string => typeof item === 'string'), modelsData);
  }
  if (!isRecord(value)) return null;

  const llm = normalizeComponentValue(value.llm);
  if (!llm) return null;

  const components: CustomCollectionComponents = { llm };
  for (const role of ['vision', 'image', 'edit', 'transcription', 'speech'] as const) {
    const component = normalizeComponentValue(value[role]);
    if (component) components[role] = component;
  }

  return components;
};

export const getCustomCollectionComponentList = (collection: { components: CustomCollectionComponents }): string[] => {
  const components = collection.components;
  const ordered = [
    components.llm,
    components.vision,
    components.image,
    components.edit,
    components.transcription,
    components.speech,
  ].filter((value): value is string => typeof value === 'string' && value.length > 0);
  return Array.from(new Set(ordered));
};

export const modelEntryToCustomCollection = (
  modelId: string,
  info: ModelInfo | undefined,
  modelsData: ModelsData,
): CustomCollection | null => {
  if (!isCollectionEditableAsCustom(info)) return null;

  const components = normalizeComponents(info?.components, modelsData);
  if (!components) return null;

  const systemPrompt = typeof info?.system_prompt === 'string' && info.system_prompt
    ? info.system_prompt
    : undefined;

  return {
    id: modelId,
    name: getCollectionDisplayName(modelId),
    components,
    systemPrompt,
  };
};

// ---------------------------------------------------------------------------
// collection.router types + builder
// ---------------------------------------------------------------------------

// L2/L3 classifier declared in the routing.classifiers array.
export interface RouterClassifier {
  id: string;
  type: 'classifier' | 'semantic_similarity' | 'llm';
  model: string;
  prompt?: string;
  labels?: string[];
  defaultLabel?: string;
  onError?: 'match_true' | 'match_false';
  referencePhrases?: Record<string, string[]>;
}

export interface RouterRule {
  id: string;
  routeTo: string;
  conditionTree: import('./routerTree').RuleNode | null;
  outputs?: Record<string, unknown>;
}

export type RouterRoutingMode = 'llm' | 'quick' | 'rules';

export interface RouterCollectionDraft {
  id?: string;
  name: string;
  createdAt?: string;
  candidates: string[];        // routing.candidates - the LLMs that answer requests
  defaultModel: string;        // routing.default_model - must be in candidates
  routingMode: RouterRoutingMode;
  // L0(a) fields - used when routingMode === 'llm'
  routerModel?: string;        // routing.router.model - the small classifier LLM (not a candidate)
  routerPrompt?: string;       // routing.router.prompt
  // L1–L3 fields - used when routingMode === 'rules'
  classifiers?: RouterClassifier[];  // L2/L3 declared classifiers
  rules?: RouterRule[];
  // Rule IDs whose condition trees could not be fully reconstructed from the server
  // policy (e.g. metadata leaves, deeply nested composites). Saving will drop those
  // conditions. The panel surfaces a warning and requires acknowledgment before save.
  lossyRuleIds?: string[];
}

export interface RouterCollectionPullRequest {
  version: '1';
  model_name: string;
  recipe: typeof COLLECTION_ROUTER_MODEL_RECIPE;
  components: string[];
  routing: Record<string, unknown>;
}

export const buildRouterCollectionPullRequest = (draft: RouterCollectionDraft): RouterCollectionPullRequest => {
  const modelName = makeCollectionId(draft.id ?? draft.name);

  // components = union of candidates + routerModel (L0a) + classifier models (L2/L3)
  const componentSet = new Set(draft.candidates);
  if (draft.routingMode === 'llm' && draft.routerModel) {
    componentSet.add(draft.routerModel);
  }
  if (draft.routingMode === 'rules' || draft.routingMode === 'quick') {
    for (const c of draft.classifiers ?? []) {
      if (c.model) componentSet.add(c.model);
    }
  }
  const components = Array.from(componentSet);

  if (components.length === 0) {
    throw new Error('Router collection requires at least one candidate model.');
  }
  if (!draft.defaultModel || !draft.candidates.includes(draft.defaultModel)) {
    throw new Error('Default model must be one of the selected candidates.');
  }

  const routing: Record<string, unknown> = {
    candidates: draft.candidates,
    default_model: draft.defaultModel,
  };

  if (draft.routingMode === 'llm') {
    if (!draft.routerModel || !draft.routerPrompt?.trim()) {
      throw new Error('LLM router requires a router model and a routing prompt.');
    }
    routing.router = { type: 'llm', model: draft.routerModel, prompt: draft.routerPrompt.trim() };
  } else {
    // Both 'quick' and 'rules' emit the same rules-based routing JSON
    if (!draft.rules?.length) {
      throw new Error('Rules router requires at least one rule.');
    }

    // Emit classifiers[] if any are declared
    if (draft.classifiers?.length) {
      routing.classifiers = draft.classifiers.map((c) => {
        const base: Record<string, unknown> = { id: c.id, type: c.type, model: c.model };
        if (c.type === 'llm') {
          // #2698 contract: llm classifiers require prompt AND a non-empty labels array.
          base.prompt = (c.prompt ?? '').trim();
          if (c.labels?.length) base.labels = c.labels;
          if (c.defaultLabel && c.labels?.includes(c.defaultLabel)) base.default_label = c.defaultLabel;
          base.on_error = c.onError ?? 'match_false';
        } else if (c.type === 'classifier') {
          if (c.labels?.length) base.labels = c.labels;
          // Server rejects a default_label that is missing from labels (or has no labels).
          if (c.defaultLabel && c.labels?.includes(c.defaultLabel)) base.default_label = c.defaultLabel;
          base.on_error = c.onError ?? 'match_false';
        } else {
          // semantic_similarity: reference_phrases is { concept: string[] }
          const referencePhrases = c.referencePhrases ?? {};
          base.reference_phrases = referencePhrases;
          const concepts = Object.keys(referencePhrases);
          // The server requires an explicit default_label whenever a rule omits `label`,
          // with no single-concept exception - so materialize the implicit default here.
          const defaultLabel = (c.defaultLabel && concepts.includes(c.defaultLabel))
            ? c.defaultLabel
            : (concepts.length === 1 ? concepts[0] : undefined);
          if (defaultLabel) base.default_label = defaultLabel;
          base.on_error = c.onError ?? 'match_false';
        }
        return base;
      });
    }

    routing.rules = draft.rules.map((r) => {
      const match: Record<string, unknown> = r.conditionTree
        ? (ruleNodeToMatchExpr(r.conditionTree) ?? {})
        : {};
      const emitted: Record<string, unknown> = { id: r.id, match, route_to: r.routeTo };
      if (r.outputs && Object.keys(r.outputs).length > 0) emitted.outputs = r.outputs;
      return emitted;
    });
  }

  return { version: '1', model_name: modelName, recipe: COLLECTION_ROUTER_MODEL_RECIPE, components, routing };
};

// Parse a server-side routing block back into a RouterCollectionDraft for the
// edit panel. Inverts buildRouterCollectionPullRequest for the flat/one-level
// shapes the UI emits. Nested composites (any inside all, etc.) are not
// supported by the panel and are not attempted here.
export const routingToRouterCollectionDraft = (
  collectionId: string,
  routing: Record<string, unknown>,
  _components: string[],
): RouterCollectionDraft => {
  const name = collectionId.replace(/^user\./, '');
  const candidates = Array.isArray(routing.candidates)
    ? (routing.candidates as string[])
    : [];
  const defaultModel = typeof routing.default_model === 'string'
    ? routing.default_model
    : '';

  // L0(a) - routing.router sugar
  if (routing.router && typeof routing.router === 'object') {
    const r = routing.router as Record<string, unknown>;
    return {
      id: collectionId, name, candidates, defaultModel,
      routingMode: 'llm',
      routerModel: typeof r.model === 'string' ? r.model : '',
      routerPrompt: typeof r.prompt === 'string' ? r.prompt : '',
      classifiers: [], rules: [],
    };
  }

  // Rules mode - reconstruct classifiers and rules
  const rawClassifiers = Array.isArray(routing.classifiers) ? routing.classifiers : [];
  const classifiers: RouterClassifier[] = (rawClassifiers as Record<string, unknown>[]).map((c) => ({
    id: typeof c.id === 'string' ? c.id : '',
    type: c.type === 'semantic_similarity' ? ('semantic_similarity' as const)
        : c.type === 'llm' ? ('llm' as const)
        : ('classifier' as const),
    model: typeof c.model === 'string' ? c.model : '',
    prompt: typeof c.prompt === 'string' ? c.prompt : undefined,
    labels: Array.isArray(c.labels) ? (c.labels as string[]) : undefined,
    defaultLabel: typeof c.default_label === 'string' ? c.default_label : undefined,
    onError: c.on_error === 'match_true' ? ('match_true' as const) : ('match_false' as const),
    referencePhrases: (c.reference_phrases && typeof c.reference_phrases === 'object' && !Array.isArray(c.reference_phrases))
        ? (c.reference_phrases as Record<string, string[]>)
        : undefined,
  }));

  const rawRules = Array.isArray(routing.rules) ? routing.rules : [];
  const lossyRuleIds: string[] = [];
  const rules: RouterRule[] = (rawRules as Record<string, unknown>[]).map((r) => {
    const rawMatch = (r.match && typeof r.match === 'object' ? r.match : {}) as Record<string, unknown>;
    const conditionTree = matchExprToRuleNode(rawMatch);
    const ruleId = typeof r.id === 'string' ? r.id : '';

    // Detect lossy round-trip: re-serialize the reconstructed tree and compare
    // canonical semantics with the original match JSON. Any difference means
    // unsupported conditions (e.g. metadata leaves, compound leaves) were dropped.
    if (Object.keys(rawMatch).length > 0 && matchExprRoundTripIsLossy(rawMatch, conditionTree)) {
      lossyRuleIds.push(ruleId || `rule-${lossyRuleIds.length + 1}`);
    }

    return {
      id: ruleId,
      routeTo: typeof r.route_to === 'string' ? r.route_to : '',
      conditionTree,
      outputs: r.outputs && typeof r.outputs === 'object' ? r.outputs as Record<string, unknown> : undefined,
    };
  });

  // Signal types the Quick editor can render. Must stay in sync with QUICK_CONDITIONS
  // in RouterCollectionPanel.tsx - only these types survive a Quick-mode round-trip.
  const QUICK_SIGNAL_TYPES = new Set([
    'min_chars', 'max_chars', 'keywords_any', 'regex', 'has_images', 'has_tools',
  ]);

  // Detect quick mode: no classifiers, every rule's condition tree is either a single
  // Quick-compatible leaf OR a flat AND/OR whose children are all Quick-compatible leaves.
  const isQuickCompatibleTree = (tree: RouterRule['conditionTree']): boolean => {
    if (!tree) return false;
    if ('signalType' in tree) return QUICK_SIGNAL_TYPES.has(tree.signalType);
    if ('operator' in tree && (tree.operator === 'AND' || tree.operator === 'OR')) {
      return tree.conditions.every(c => 'signalType' in c && QUICK_SIGNAL_TYPES.has(c.signalType));
    }
    return false;
  };
  const isQuick = classifiers.length === 0 && rules.length > 0 && rules.every(
    r => isQuickCompatibleTree(r.conditionTree)
  );

  return {
    id: collectionId, name, candidates, defaultModel,
    routingMode: isQuick ? 'quick' : 'rules',
    classifiers, rules,
    ...(lossyRuleIds.length > 0 ? { lossyRuleIds } : {}),
  };
};

// Validate an imported Hybrid Router JSON (a /pull registration body) before it
// is sent to the server, so file-picker mistakes get descriptive errors instead
// of raw parser messages. Structural checks only - name resolution, candidate
// membership, and rule semantics stay server-side (the server resolves aliases
// the UI cannot). Returns a normalized copy ready for registration.
export const validateRouterImportPayload = (parsed: unknown): Record<string, unknown> => {
  if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) {
    throw new Error('The selected file is not a valid Router JSON.');
  }
  const record = { ...(parsed as Record<string, unknown>) };
  if (typeof record.model_name !== 'string' || record.model_name.length === 0) {
    throw new Error("The selected file is missing a 'model_name' field.");
  }
  if (record.recipe !== COLLECTION_ROUTER_MODEL_RECIPE) {
    throw new Error(`Expected recipe 'collection.router', got '${String(record.recipe ?? '(missing)')}'.`);
  }
  const routing = record.routing;
  if (!routing || typeof routing !== 'object' || Array.isArray(routing)) {
    throw new Error("The file is missing a 'routing' block. Check that it is a valid Hybrid Router JSON.");
  }
  const r = routing as Record<string, unknown>;
  if (!Array.isArray(r.candidates) || r.candidates.length === 0) {
    throw new Error("'routing.candidates' is missing or empty - the file must list at least one candidate model.");
  }
  if (typeof r.default_model !== 'string' || !r.default_model) {
    throw new Error("'routing.default_model' is missing - the file must specify a fallback candidate.");
  }
  const hasRules = Array.isArray(r.rules) && r.rules.length > 0;
  const hasRouter = isRecord(r.router);
  if (!hasRules && !hasRouter) {
    throw new Error("The 'routing' block must contain either a non-empty 'rules' array or a 'router' entry.");
  }
  if (hasRules && hasRouter) {
    throw new Error("The 'routing' block cannot contain both 'rules' and a 'router' entry.");
  }
  // The server parser requires a root version and only supports "1". Files
  // exported before the version field was preserved lack it - default it so
  // they stay importable.
  if (record.version === undefined) {
    record.version = '1';
  }
  return record;
};

// Order-insensitive, semantics-aware comparison of two routing blocks. Used to
// detect whether the draft in the editor still matches what the server has
// saved (rule match expressions are canonicalized the same way the lossy
// round-trip check does).
export const routingBlocksEquivalent = (a: unknown, b: unknown): boolean => {
  const canon = (routing: unknown): unknown => {
    if (!isRecord(routing)) return routing;
    const out: Record<string, unknown> = { ...routing };
    if (Array.isArray(out.rules)) {
      out.rules = out.rules.map((rule) =>
        isRecord(rule) && isRecord(rule.match)
          ? { ...rule, match: canonicalizeMatchExpr(rule.match) }
          : rule,
      );
    }
    return out;
  };
  return stableStringify(canon(a)) === stableStringify(canon(b));
};

export const getRouterCandidateOptions = (modelsData: ModelsData) => {
  return Object.entries(modelsData)
    .filter(([, info]) => !isCollectionRecipe(info?.recipe) && isChatPlannerCandidate(info))
    .map(([id, info]) => ({ id, info }))
    .sort((a, b) => {
      const downloadedDiff = Number(b.info.downloaded === true) - Number(a.info.downloaded === true);
      if (downloadedDiff !== 0) return downloadedDiff;
      return (a.info.model_name ?? a.id).localeCompare(b.info.model_name ?? b.id);
    });
};

export const buildCustomCollectionPullRequest = (draft: CustomCollectionDraft): CustomCollectionPullRequest => {
  const modelName = makeCollectionId(draft.id ?? draft.name);
  const components = getCustomCollectionComponentList(draft);

  if (components.length === 0 || !draft.components.llm) {
    throw new Error('Omni Model requires a name and a planner LLM.');
  }

  const request: CustomCollectionPullRequest = {
    model_name: modelName,
    recipe: COLLECTION_OMNI_MODEL_RECIPE,
    components,
  };
  const systemPrompt = typeof draft.systemPrompt === 'string' ? draft.systemPrompt.trim() : '';
  if (systemPrompt) {
    request.system_prompt = systemPrompt;
  }
  return request;
};

const isCollectionEligibleModel = (info?: ModelInfo): boolean => {
  if (!info || isCollectionRecipe(info.recipe)) {
    return false;
  }
  return true;
};

export const getCollectionRoleOptions = (modelsData: ModelsData, role: CustomCollectionRole) => {
  return Object.entries(modelsData)
    .filter(([, info]) => isCollectionEligibleModel(info))
    .filter(([, info]) => {
      const labels = info.labels ?? [];
      switch (role) {
        case 'llm':
          return isChatPlannerCandidate(info);
        case 'vision':
          return labels.includes('vision');
        case 'image':
          return labels.includes('image');
        case 'edit':
          return labels.includes('edit');
        case 'transcription':
          return labels.includes('transcription') || labels.includes('audio');
        case 'speech':
          return labels.includes('tts') || labels.includes('speech');
        default:
          return false;
      }
    })
    .map(([id, info]) => ({ id, info }))
    .sort((a, b) => {
      const downloadedDiff = Number(b.info.downloaded === true) - Number(a.info.downloaded === true);
      if (downloadedDiff !== 0) return downloadedDiff;
      return (a.info.model_name ?? a.id).localeCompare(b.info.model_name ?? b.id);
    });
};
