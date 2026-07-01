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
  prompt?: string;
  labels?: string[];
  defaultLabel?: string;
  onError?: 'match_true' | 'match_false';
  referencePhrases?: Record<string, string[]>;
}

export type RouterRuleOperator = 'any' | 'all';

// One atomic condition inside a condition group.
export interface RouterCondition {
  id: string;
  type: 'keywords_any' | 'keywords_all' | 'regex' | 'min_chars' | 'max_chars'
      | 'has_tools' | 'has_images' | 'classifier';
  not?: boolean;
  // keywords_any / keywords_all
  keywords?: string[];
  // regex
  pattern?: string;
  // min_chars / max_chars
  value?: number;
  // classifier
  classifierId?: string;
  label?: string;
  minScore?: number;
  maxScore?: number;
}

// A group of conditions combined by a single AND or OR operator.
// joinOperator is undefined on the first group; on subsequent groups it controls
// how this group joins with the result of all groups before it.
export interface RouterConditionGroup {
  id: string;
  operator: RouterRuleOperator;
  conditions: RouterCondition[];
  joinOperator?: RouterRuleOperator;
}

export interface RouterRule {
  id: string;
  routeTo: string;
  groups: RouterConditionGroup[];
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
      // Wrap a leaf in { not: inner } when the condition is negated.
      const maybeNot = (leaf: Record<string, unknown>, not: boolean | undefined): Record<string, unknown> =>
        not ? { not: leaf } : leaf;

      // Convert one RouterCondition to its JSON leaf.
      const condToLeaf = (c: RouterCondition): Record<string, unknown> | null => {
        if (c.type === 'keywords_any' && c.keywords?.length)
          return maybeNot({ keywords_any: c.keywords }, c.not);
        if (c.type === 'keywords_all' && c.keywords?.length)
          return maybeNot({ keywords_all: c.keywords }, c.not);
        if (c.type === 'regex' && c.pattern?.trim())
          return maybeNot({ regex: c.pattern.trim() }, c.not);
        if (c.type === 'min_chars' && c.value !== undefined)
          return maybeNot({ min_chars: c.value }, c.not);
        if (c.type === 'max_chars' && c.value !== undefined)
          return maybeNot({ max_chars: c.value }, c.not);
        if (c.type === 'has_tools')
          return maybeNot({ has_tools: true }, c.not);
        if (c.type === 'has_images')
          return maybeNot({ has_images: true }, c.not);
        if (c.type === 'classifier' && c.classifierId) {
          const leaf: Record<string, unknown> = { classifier: c.classifierId };
          if (c.label) leaf.label = c.label;
          if (c.minScore !== undefined) leaf.min_score = c.minScore;
          if (c.maxScore !== undefined) leaf.max_score = c.maxScore;
          return maybeNot(leaf, c.not);
        }
        return null;
      };

      // Convert one group to its match expression.
      const groupToMatch = (g: RouterConditionGroup): Record<string, unknown> | null => {
        const leaves = g.conditions.map(condToLeaf).filter((l): l is Record<string, unknown> => l !== null);
        if (leaves.length === 0) return null;
        if (leaves.length === 1) return leaves[0];
        return { [g.operator]: leaves };
      };

      const groups = r.groups ?? [];
      const groupMatches = groups
        .map((g, i) => ({ m: groupToMatch(g), join: i === 0 ? undefined : (g.joinOperator ?? 'any') }))
        .filter((x): x is { m: Record<string, unknown>; join: RouterRuleOperator | undefined } => x.m !== null);

      // Left-fold: accumulate groups one at a time using each group's joinOperator.
      // If consecutive groups share the same joinOperator, flatten them into one array.
      const match: Record<string, unknown> = {};
      if (groupMatches.length === 0) {
        // empty — leave match blank
      } else if (groupMatches.length === 1) {
        Object.assign(match, groupMatches[0].m);
      } else {
        // Build a fold accumulator: { op, items[] } where items are the flat children.
        // When the next joinOperator differs, nest the current accumulator and start fresh.
        type Acc = { op: RouterRuleOperator; items: Record<string, unknown>[] };
        let acc: Acc = { op: groupMatches[1].join!, items: [groupMatches[0].m] };
        for (let i = 1; i < groupMatches.length; i++) {
          const { m, join } = groupMatches[i];
          const op = join!;
          if (op === acc.op) {
            acc.items.push(m);
          } else {
            // Seal current accumulator into a nested node, start new level.
            const sealed: Record<string, unknown> = acc.items.length === 1
              ? acc.items[0]
              : { [acc.op]: acc.items };
            acc = { op, items: [sealed, m] };
          }
        }
        const final: Record<string, unknown> = acc.items.length === 1
          ? acc.items[0]
          : { [acc.op]: acc.items };
        Object.assign(match, final);
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

  let condSeq = 0;
  let groupSeq = 0;
  const nextCondId = () => `cond-${++condSeq}`;
  const nextGroupId = () => `grp-${++groupSeq}`;

  // Parse a JSON leaf (possibly not-wrapped) into a RouterCondition.
  const leafToCondition = (rawLeaf: Record<string, unknown>): RouterCondition | null => {
    let not = false;
    let leaf = rawLeaf;
    if (leaf.not && typeof leaf.not === 'object' && !Array.isArray(leaf.not)) {
      not = true;
      leaf = leaf.not as Record<string, unknown>;
    }
    if (Array.isArray(leaf.keywords_any))
      return { id: nextCondId(), type: 'keywords_any', keywords: leaf.keywords_any as string[], not: not || undefined };
    if (Array.isArray(leaf.keywords_all))
      return { id: nextCondId(), type: 'keywords_all', keywords: leaf.keywords_all as string[], not: not || undefined };
    if (typeof leaf.regex === 'string')
      return { id: nextCondId(), type: 'regex', pattern: leaf.regex, not: not || undefined };
    if (typeof leaf.min_chars === 'number')
      return { id: nextCondId(), type: 'min_chars', value: leaf.min_chars, not: not || undefined };
    if (typeof leaf.max_chars === 'number')
      return { id: nextCondId(), type: 'max_chars', value: leaf.max_chars, not: not || undefined };
    if ('has_tools' in leaf)
      return { id: nextCondId(), type: 'has_tools', not: not || undefined };
    if ('has_images' in leaf)
      return { id: nextCondId(), type: 'has_images', not: not || undefined };
    if (typeof leaf.classifier === 'string')
      return {
        id: nextCondId(), type: 'classifier', classifierId: leaf.classifier,
        label: typeof leaf.label === 'string' ? leaf.label : undefined,
        minScore: typeof leaf.min_score === 'number' ? leaf.min_score : undefined,
        maxScore: typeof leaf.max_score === 'number' ? leaf.max_score : undefined,
        not: not || undefined,
      };
    return null;
  };

  // Parse one item from a top-level any/all array into a group.
  // An item that is itself any/all becomes a group; a flat leaf becomes a 1-condition group.
  const itemToGroup = (item: Record<string, unknown>, defaultOp: RouterRuleOperator): RouterConditionGroup => {
    if (Array.isArray(item.all)) {
      const conds = (item.all as Record<string, unknown>[]).map(leafToCondition).filter((c): c is RouterCondition => c !== null);
      return { id: nextGroupId(), operator: 'all', conditions: conds };
    }
    if (Array.isArray(item.any)) {
      const conds = (item.any as Record<string, unknown>[]).map(leafToCondition).filter((c): c is RouterCondition => c !== null);
      return { id: nextGroupId(), operator: 'any', conditions: conds };
    }
    const cond = leafToCondition(item);
    return { id: nextGroupId(), operator: defaultOp, conditions: cond ? [cond] : [] };
  };

  // Parse a match expression into RouterConditionGroups with joinOperator set on each
  // group after the first (joinOperator = the op that connects it to the previous group).
  const matchToGroups = (rawMatch: Record<string, unknown>): RouterConditionGroup[] => {
    for (const op of ['any', 'all'] as const) {
      if (Array.isArray(rawMatch[op])) {
        const items = rawMatch[op] as Record<string, unknown>[];
        const groups = items.map((item, i) => ({
          ...itemToGroup(item, op),
          joinOperator: i === 0 ? undefined : op,
        }));
        return groups.length ? groups : [{ id: nextGroupId(), operator: op, conditions: [] }];
      }
    }
    // Single flat leaf — one group, no joinOperator.
    const cond = leafToCondition(rawMatch);
    return [{ id: nextGroupId(), operator: 'any', conditions: cond ? [cond] : [] }];
  };

  const rawRules = Array.isArray(routing.rules) ? routing.rules : [];
  const rules: RouterRule[] = (rawRules as Record<string, unknown>[]).map((r) => {
    const rawMatch = (r.match && typeof r.match === 'object' ? r.match : {}) as Record<string, unknown>;
    return {
      id: typeof r.id === 'string' ? r.id : '',
      routeTo: typeof r.route_to === 'string' ? r.route_to : '',
      groups: matchToGroups(rawMatch),
      outputs: r.outputs && typeof r.outputs === 'object' ? r.outputs as Record<string, unknown> : undefined,
    };
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
