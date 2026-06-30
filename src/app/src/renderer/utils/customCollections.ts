import type { ModelInfo, ModelsData } from './modelData';
import { USER_MODEL_PREFIX } from './modelData';
import { isChatPlannerCandidate } from './modelLabels';
import { COLLECTION_OMNI_MODEL_RECIPE, COLLECTION_ROUTER_MODEL_RECIPE, isCollectionRecipe } from './recipeNames';

export const CUSTOM_COLLECTION_PREFIX = USER_MODEL_PREFIX;

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
}

export interface CustomCollectionDraft {
  id?: string;
  name: string;
  createdAt?: string;
  components: CustomCollectionComponents;
}

export interface CustomCollectionPullRequest {
  model_name: string;
  recipe: typeof COLLECTION_OMNI_MODEL_RECIPE;
  components: string[];
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

  return {
    id: modelId,
    name: getCollectionDisplayName(modelId),
    components,
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
  // llm-only: same concept as NL Router routerPrompt
  prompt?: string;
  // classifier-only fields:
  labels?: string[];                       // text-classification output labels
  defaultLabel?: string;                   // label used when condition omits label
  onError?: 'match_true' | 'match_false';  // fail-closed vs fail-open
  referencePhrases?: Record<string, string[]>;
}

// How multiple conditions on one rule are combined.
export type RouterRuleOperator = 'any' | 'all';

// One keywords-any/all entry (up to 2 per rule, each independently negatable).
export interface RouterKeywordEntry {
  keywords: string[];
  not?: boolean;   // true → emit { not: { keywords_any/all: [...] } }
}

// One regex entry (up to 2 per rule).
export interface RouterRegexEntry {
  pattern: string;
  not?: boolean;
}

// A single rule: one or more conditions + one route_to target.
// keywords-any, keywords-all, and regex each support up to 2 independent
// entries so you can express e.g. "contains 'function' AND does NOT contain 'tutorial'".
export interface RouterRule {
  id: string;
  routeTo: string;                      // must be in candidates
  operator?: RouterRuleOperator;        // 'any' (OR, default) or 'all' (AND)
  // Deterministic conditions:
  matchKeywordsAny?: RouterKeywordEntry[];   // up to 2, each optionally negated
  matchKeywordsAll?: RouterKeywordEntry[];   // up to 2, each optionally negated
  matchRegex?: RouterRegexEntry[];           // up to 2, each optionally negated
  matchMinChars?: number;
  matchMinCharsNot?: boolean;
  matchMaxChars?: number;
  matchMaxCharsNot?: boolean;
  matchHasTools?: boolean;
  matchHasToolsNot?: boolean;
  matchHasImages?: boolean;
  matchHasImagesNot?: boolean;
  // Classifier-band condition:
  matchClassifier?: {
    classifierId: string;               // references a RouterClassifier id
    label?: string;                     // which label's score to apply the band to
    minScore?: number;                  // inclusive [0,1]
    maxScore?: number;                  // inclusive [0,1]
    not?: boolean;                      // wrap this leaf in { not: { classifier: ... } }
  };
  // Engine-opaque pass-through bag copied verbatim into the Decision.
  outputs?: Record<string, unknown>;
}

export type RouterRoutingMode = 'llm' | 'rules';

export interface RouterCollectionDraft {
  id?: string;
  name: string;
  createdAt?: string;
  candidates: string[];        // routing.candidates — the LLMs that answer requests
  defaultModel: string;        // routing.default_model — must be in candidates
  routingMode: RouterRoutingMode;
  // L0(a) fields — used when routingMode === 'llm'
  routerModel?: string;        // routing.router.model — the small classifier LLM (not a candidate)
  routerPrompt?: string;       // routing.router.prompt
  // L1–L3 fields — used when routingMode === 'rules'
  classifiers?: RouterClassifier[];  // L2/L3 declared classifiers
  rules?: RouterRule[];
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
  if (draft.routingMode === 'rules') {
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
    if (!draft.rules?.length) {
      throw new Error('Rules router requires at least one rule.');
    }

    // Emit classifiers[] if any are declared
    if (draft.classifiers?.length) {
      routing.classifiers = draft.classifiers.map((c) => {
        const base: Record<string, unknown> = { id: c.id, type: c.type, model: c.model };
        if (c.type === 'llm') {
          base.prompt = c.prompt ?? '';
        } else if (c.type === 'classifier') {
          if (c.labels?.length) base.labels = c.labels;
          if (c.defaultLabel) base.default_label = c.defaultLabel;
          base.on_error = c.onError ?? 'match_false';
        } else {
          // semantic_similarity: reference_phrases is { concept: string[] }
          base.reference_phrases = c.referencePhrases ?? {};
        }
        return base;
      });
    }

    routing.rules = draft.rules.map((r) => {
      // Helper: wrap a leaf in { not: leaf } when the condition's Not flag is set
      const n = (leaf: Record<string, unknown>, notFlag: boolean | undefined): Record<string, unknown> =>
        notFlag ? { not: leaf } : leaf;

      const leaves: Record<string, unknown>[] = [];
      for (const entry of r.matchKeywordsAny ?? [])
        if (entry.keywords?.length) leaves.push(n({ keywords_any: entry.keywords }, entry.not));
      for (const entry of r.matchKeywordsAll ?? [])
        if (entry.keywords?.length) leaves.push(n({ keywords_all: entry.keywords }, entry.not));
      for (const entry of r.matchRegex ?? [])
        if (entry.pattern?.trim()) leaves.push(n({ regex: entry.pattern.trim() }, entry.not));
      if (r.matchMinChars !== undefined)
        leaves.push(n({ min_chars: r.matchMinChars }, r.matchMinCharsNot));
      if (r.matchMaxChars !== undefined)
        leaves.push(n({ max_chars: r.matchMaxChars }, r.matchMaxCharsNot));
      if (r.matchHasTools !== undefined)
        leaves.push(n({ has_tools: r.matchHasTools }, r.matchHasToolsNot));
      if (r.matchHasImages !== undefined)
        leaves.push(n({ has_images: r.matchHasImages }, r.matchHasImagesNot));
      if (r.matchClassifier?.classifierId) {
        const leaf: Record<string, unknown> = { classifier: r.matchClassifier.classifierId };
        if (r.matchClassifier.label) leaf.label = r.matchClassifier.label;
        if (r.matchClassifier.minScore !== undefined) leaf.min_score = r.matchClassifier.minScore;
        if (r.matchClassifier.maxScore !== undefined) leaf.max_score = r.matchClassifier.maxScore;
        leaves.push(r.matchClassifier.not ? { not: leaf } : leaf);
      }

      const op = r.operator ?? 'any';
      const match: Record<string, unknown> = {};
      if (leaves.length === 1) {
        Object.assign(match, leaves[0]);
      } else if (leaves.length > 1) {
        match[op] = leaves;
      }
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

  // L0(a) — routing.router sugar
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

  // Rules mode — reconstruct classifiers and rules
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
  const rules: RouterRule[] = (rawRules as Record<string, unknown>[]).map((r) => {
    const rawMatch = (r.match && typeof r.match === 'object' ? r.match : {}) as Record<string, unknown>;

    let leaves: Record<string, unknown>[];
    let operator: RouterRuleOperator = 'any';
    if (Array.isArray(rawMatch.any)) {
      leaves = rawMatch.any as Record<string, unknown>[];
      operator = 'any';
    } else if (Array.isArray(rawMatch.all)) {
      leaves = rawMatch.all as Record<string, unknown>[];
      operator = 'all';
    } else {
      // Single leaf (possibly not-wrapped): pass as-is to unwrapNot below
      leaves = [rawMatch];
    }

    const rule: RouterRule = {
      id: typeof r.id === 'string' ? r.id : '',
      routeTo: typeof r.route_to === 'string' ? r.route_to : '',
      operator,
      outputs: r.outputs && typeof r.outputs === 'object' ? r.outputs as Record<string, unknown> : undefined,
    };

    // Helper: unwrap a { not: inner } leaf, returning { inner, negated }
    const unwrapNot = (leaf: Record<string, unknown>) => {
      if (leaf.not && typeof leaf.not === 'object' && !Array.isArray(leaf.not)) {
        return { inner: leaf.not as Record<string, unknown>, negated: true };
      }
      return { inner: leaf, negated: false };
    };

    for (const rawLeaf of leaves) {
      const { inner: leaf, negated } = unwrapNot(rawLeaf);
      if (Array.isArray(leaf.keywords_any)) {
        rule.matchKeywordsAny = [...(rule.matchKeywordsAny ?? []),
          { keywords: leaf.keywords_any as string[], not: negated || undefined }];
      }
      if (Array.isArray(leaf.keywords_all)) {
        rule.matchKeywordsAll = [...(rule.matchKeywordsAll ?? []),
          { keywords: leaf.keywords_all as string[], not: negated || undefined }];
      }
      if (typeof leaf.regex === 'string') {
        rule.matchRegex = [...(rule.matchRegex ?? []),
          { pattern: leaf.regex, not: negated || undefined }];
      }
      if (typeof leaf.min_chars === 'number') {
        rule.matchMinChars = leaf.min_chars;
        if (negated) rule.matchMinCharsNot = true;
      }
      if (typeof leaf.max_chars === 'number') {
        rule.matchMaxChars = leaf.max_chars;
        if (negated) rule.matchMaxCharsNot = true;
      }
      if (typeof leaf.has_tools === 'boolean') {
        rule.matchHasTools = leaf.has_tools;
        if (negated) rule.matchHasToolsNot = true;
      }
      if (typeof leaf.has_images === 'boolean') {
        rule.matchHasImages = leaf.has_images;
        if (negated) rule.matchHasImagesNot = true;
      }
      if (typeof leaf.classifier === 'string') {
        rule.matchClassifier = {
          classifierId: leaf.classifier,
          label: typeof leaf.label === 'string' ? leaf.label : undefined,
          minScore: typeof leaf.min_score === 'number' ? leaf.min_score : undefined,
          maxScore: typeof leaf.max_score === 'number' ? leaf.max_score : undefined,
          not: negated || undefined,
        };
      }
    }
    return rule;
  });

  return {
    id: collectionId, name, candidates, defaultModel,
    routingMode: 'rules',
    classifiers, rules,
  };
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
